// ===========================================================================
// mirror.cpp - Ultra-low-latency monitor mirror (Windows, D3D11 / DXGI)
//
// Mirrors a user-chosen rectangle of a "working" monitor (where your drawing
// software runs) onto a rectangle of a second "mirror" monitor. The mirror
// rectangle is locked to the working rectangle's aspect ratio, so the picture
// is never distorted - it is copied 1:1 when the sizes match, or cleanly scaled
// when they differ.
//
// Everything is configured from a visual GUI (see config_gui.cpp), reached from
// the tray "Edit..." menu or shown automatically on first launch / on any
// error. The configuration is saved next to the exe (mirror.cfg) and can be
// imported / exported.
//
// Architecture (all-GPU, no CPU round-trip):
//   - DXGI Desktop Duplication gives the working monitor as a GPU texture.
//   - CopySubresourceRegion crops the working rectangle into a GPU texture.
//   - A flip-model swapchain presents it with tearing (vsync off), the
//     independent flip is possible when Windows permits it -> minimal latency.
//   - The cursor (delivered separately by Duplication) is composited as a quad.
//   - Runs from the system tray; left-click toggles mirroring.
//
// Build: see build.bat  (MinGW g++).
// ===========================================================================
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <mmsystem.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdarg>

#include "monitors.h"
#include "config.h"
#include "config_gui.h"
#include "window_guard.h"
#include "magnifier_geometry.h"

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
// Globals / tray
// ----------------------------------------------------------------------------
static const wchar_t* kClassName = L"MirrorD3D11Window";
static const UINT WM_TRAY   = WM_APP + 1;
static const UINT IDM_TOGGLE = 1001;
static const UINT IDM_EDIT   = 1002;
static const UINT IDM_QUIT   = 1003;
static const UINT IDM_DIAGNOSTICS = 1004;
static std::atomic<bool> g_diagnostics{false}, g_diagnosticsOpen{false};
static std::atomic<bool> g_lensToggled{false};
static const int ID_MAG_HOTKEY=0x4D47;
static const UINT WM_MAG_HOTKEY_CONFIG=WM_APP+9;

static HINSTANCE   g_hInst = nullptr;
static HWND        g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static HICON       g_icon = nullptr;
static std::atomic<bool> g_enabled{true};
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_reconfigure{false};   // rebuild pipeline (display change / edit-save)
static std::atomic<bool> g_showConfig{false};   // open the setup dialog next loop iteration
static bool        g_trayAdded   = false;
static std::atomic<unsigned long> g_resize{0};
static HWND g_controller = nullptr; // stable, UI-thread-owned tray window
static HANDLE g_wake = nullptr;
static const UINT WM_CREATE_MIRROR = WM_APP + 2, WM_DESTROY_MIRROR = WM_APP + 3;
static const UINT WM_SHOW_MIRROR = WM_APP + 4, WM_TRAY_STATE = WM_APP + 5;
static const UINT WM_RENDER_DONE = WM_APP + 6, WM_NOTICE = WM_APP + 7, WM_DIAGNOSTICS = WM_APP + 8;
static void wakeRenderer() { if (g_wake) SetEvent(g_wake); }

static WindowGuard             g_windowGuard; // lifecycle owned by renderer; work happens on separate thread
static MirrorConfig            g_cfg;
static std::vector<MonitorInfo> g_monitors;
static std::wstring            g_cfgPath;

// Live setup owns a private configuration snapshot. The renderer never waits
// for the dialog; the thread handle signals when its result can be consumed.
struct LiveSetup {
    MirrorConfig edited;
    std::vector<MonitorInfo> monitors;
    std::wstring path, banner;
    bool live=true;
    HINSTANCE instance = nullptr;
    ConfigResult result = ConfigResult::Quit;
};
static LiveSetup g_liveSetup;
static HANDLE g_setupThread = nullptr;
static DWORD WINAPI setupWorker(LPVOID arg) {
    auto& job = *static_cast<LiveSetup*>(arg);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    job.result = RunConfigDialog(job.edited, job.monitors, job.instance,
                                 job.banner, job.path, job.live);
    if (SUCCEEDED(hr)) CoUninitialize();
    wakeRenderer();
    return 0;
}
static bool sameRegion(const RegionCfg& a, const RegionCfg& b) {
    return a.device == b.device && a.hwid == b.hwid && a.x == b.x &&
           a.y == b.y && a.w == b.w && a.h == b.h;
}
static bool samePipeline(const MirrorConfig& a, const MirrorConfig& b) {
    if (a.followMouse && b.followMouse) return a.target.device==b.target.device && a.target.hwid==b.target.hwid;
    return sameRegion(a.source,b.source) && sameRegion(a.target,b.target) &&
           a.followMouse == b.followMouse && a.cover == b.cover &&
           a.windowed == b.windowed;
}

// ----------------------------------------------------------------------------
// Logging: always to the debugger; optionally to mirror.log next to the exe.
// ----------------------------------------------------------------------------
static FILE* g_logFile = nullptr;
static void logf(const char* fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
    if (g_logFile) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(g_logFile, "[%02d:%02d:%02d.%03d] %s\n",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_logFile);
    }
}
static void openLog(bool on) {
    if (on && !g_logFile) {
        std::wstring p = ExeDir() + L"\\mirror.log";
        g_logFile = _wfopen(p.c_str(), L"a");
        if (g_logFile) {
            SYSTEMTIME t; GetLocalTime(&t);
            fprintf(g_logFile, "\n=== mirror started %04d-%02d-%02d %02d:%02d:%02d ===\n",
                    t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            fflush(g_logFile);
        }
    } else if (!on && g_logFile) {
        fclose(g_logFile); g_logFile = nullptr;
    }
}

// output corner -> source corner UVs for a rotation (cw) + optional flips.
// Output vertex order (matches the VS): 0=TL, 1=TR, 2=BL, 3=BR.
static void computeUVs(int rot, bool fh, bool fv, float out[8]) {
    static const float su[4] = {0,1,0,1}, sv[4] = {0,0,1,1};   // src TL,TR,BL,BR
    static const int p0[4]={0,1,2,3}, p90[4]={2,0,3,1}, p180[4]={3,2,1,0}, p270[4]={1,3,0,2};
    const int* p = rot==90 ? p90 : rot==180 ? p180 : rot==270 ? p270 : p0;
    for (int i = 0; i < 4; ++i) {
        int j = i ^ (fh ? 1 : 0) ^ (fv ? 2 : 0);
        float u = su[p[j]], v = sv[p[j]];
        out[i*2] = u; out[i*2+1] = v;
    }
}

// ----------------------------------------------------------------------------
// Renderer + capture state
// ----------------------------------------------------------------------------
struct Mirror {
    ComPtr<ID3D11Device>        dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIAdapter1>       srcAdapter;
    ComPtr<IDXGIOutput1>        srcOutput;
    ComPtr<IDXGIOutputDuplication> dupl;

    ComPtr<IDXGISwapChain2>     swap;
    HANDLE                      waitable = nullptr;
    bool                        allowTearing = false;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D>     cropTex;
    ComPtr<ID3D11ShaderResourceView> cropSRV;

    ComPtr<ID3D11VertexShader>  vs;
    ComPtr<ID3D11PixelShader>   ps, psMask, psLens;
    ComPtr<ID3D11Buffer>        cb;
    ComPtr<ID3D11SamplerState>  sampPoint, sampLinear;
    ComPtr<ID3D11BlendState>    blendOpaque, blendAlpha;
    ComPtr<ID3D11RasterizerState> rsScissor;

    // cursor
    ComPtr<ID3D11Texture2D>     curTex;
    ComPtr<ID3D11ShaderResourceView> curSRV;
    int  curW = 0, curH = 0, curX = 0, curY = 0;
    bool curVisible = false, curMask=false;

    // geometry (derived from config), all in physical pixels
    int cropX=0, cropY=0, cropW=0, cropH=0;   // region on the working monitor
    int winW=0, winH=0;                        // backbuffer / window client size
    int drawX=0, drawY=0, drawW=0, drawH=0;    // where the mirror is drawn in the window
    bool vsync=false, showCursor=true;
    bool lowLatency=false, redrawNeeded=true, frameValid=false, frameHeld=false;
    bool presentationReady=false, occluded=false;
    HRESULT lastError = S_OK;
    unsigned long long presents=0, captures=0, accumulated=0;
    double frameAgeMs=0;
    bool scaling=false;
    MagnifierConfig magnifier;
    bool lensVisible=false;
    float lensCX=0,lensCY=0,lensAX=0,lensAY=0,lensHalfW=0,lensHalfH=0;
    void updateLens();
    // presentation options
    bool windowed=false;                       // floating resizable window
    int  rotation=0; bool flipH=false, flipV=false;
    double drawAspect=1.0;                     // rotation-adjusted w:h of the mirror
    float uvBase[8]={0,0,1,0,0,1,1,1};
    float uvRot[8]={0,0,1,0,0,1,1,1};          // per-corner UVs (rotation/flip baked in)
    UINT swapFlags=0;                          // remembered for ResizeBuffers

    int rawX=0,rawY=0,rawW=0,rawH=0;
    void updateCaptureGeometry();
    int activeSource = -1;
    HWND window = nullptr;
    struct SourceCache { int index; ComPtr<IDXGIOutput1> output; ComPtr<IDXGIOutputDuplication> duplication; ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> srv; bool valid=false; };
    std::vector<SourceCache> sources;
    bool selectSource(int index);
    bool init(const MirrorConfig& cfg, const std::vector<MonitorInfo>& mons, HWND hwnd, std::wstring& err);
    bool createDuplication();
    bool createPipeline();
    void fitDrawRect();                        // contain-fit drawAspect into winW x winH (windowed)
    bool resize(int w, int h);                 // windowed live resize
    bool releaseFrame();
    void reset();
    void applyOptions(const MirrorConfig& cfg);
    void renderFrame();
    void updateCursor(IDXGIOutputDuplication* d, const DXGI_OUTDUPL_FRAME_INFO& fi);
    void buildCursorTexture(const std::vector<BYTE>& bgra, int w, int h);
};

struct CB { float posRect[4]; float uv[8]; float baseRect[4]; float baseUV[8];
    float lensRect[4]; // center xy, half size xy, in target pixels
    float lensParams[4]; // anchor xy, zoom, shape
    float lensColor[4]; // rgb, border width
    float lensFlags[4]; // active, sharp sampling, feather width, reserved
};
static_assert(sizeof(CB)%16==0,"D3D constant buffers require 16-byte alignment");  // posRect + 4 corner UVs

static const char* kVS =
"cbuffer CB:register(b0){float4 posRect;float4 uvA;float4 uvB;};"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"VO main(uint vid:SV_VertexID){"
" float2 g=float2((vid&1),(vid>>1));"
" float2 uvs[4]={uvA.xy,uvA.zw,uvB.xy,uvB.zw};"
" VO o;"
" o.pos=float4(lerp(posRect.x,posRect.z,g.x),lerp(posRect.y,posRect.w,g.y),0,1);"
" o.uv =uvs[vid];"
" return o;}";
static const char* kPS =
"Texture2D tex:register(t0);SamplerState smp:register(s0);"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"float4 main(VO i):SV_Target{return tex.Sample(smp,i.uv);}";
#define LENS_CB "cbuffer CB:register(b0){float4 posRect;float4 uvA;float4 uvB;float4 baseRect;float4 baseA;float4 baseB;float4 lensRect;float4 lensParams;float4 lensColor;float4 lensFlags;};"
#define LENS_COORDS \
"float2 sourceUV(float2 p){float2 q=(p-baseRect.xy)/baseRect.zw;return baseA.xy+q.x*(baseA.zw-baseA.xy)+q.y*(baseB.xy-baseA.xy);}" \
"float lensDist(float2 p){float2 a=abs(p-lensRect.xy);float2 h=max(lensRect.zw,1.0);" \
"if(lensParams.w<0.5){float k=length(a/h);float d=length(a/(h*h));return k<0.0001?-min(h.x,h.y):k*(k-1.0)/max(d,0.000001);}" \
"float r=lensParams.w>1.5?min(24.0,min(h.x,h.y)*0.35):0.0;float2 d=a-h+r;return length(max(d,0.0))+min(max(d.x,d.y),0.0)-r;}" \
"float2 lensUV(float2 p){return sourceUV(lensParams.xy+(p-lensRect.xy)/lensParams.z);}" \
"float lensAlpha(float d){return lensFlags.z>0.0?smoothstep(0.0,lensFlags.z,-d):1.0;}"
static const char* kPSLens = LENS_CB LENS_COORDS
"Texture2D tex:register(t0);SamplerState smp:register(s0);"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"float4 main(VO i):SV_Target{float d=lensDist(i.pos.xy);clip(-d);"
"float a=lensAlpha(d);"
"if(lensColor.w>0.0 && d>=-lensColor.w)return float4(lensColor.rgb,a);"
"return float4(tex.Sample(smp,lensUV(i.pos.xy)).rgb,a);}";
static const char* kPSMask = LENS_CB LENS_COORDS
"Texture2D maskTex:register(t0);Texture2D desktopTex:register(t1);"
"SamplerState smp:register(s0);SamplerState pointSmp:register(s1);SamplerState linearSmp:register(s2);"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"float4 main(VO i):SV_Target{"
"float2 u=sourceUV(i.pos.xy);"
"float dist=lensDist(i.pos.xy);bool inLens=lensFlags.x>0.5 && dist<=0.0;"
"float4 m=maskTex.Sample(pointSmp,i.uv);"
"uint3 x=(uint3)round(saturate(m.rgb)*255.0);"
"float3 bg=desktopTex.Sample(smp,u).rgb;"
"if(inLens){float2 lu=lensUV(i.pos.xy);float3 lc=lensFlags.y>0.5?desktopTex.Sample(pointSmp,lu).rgb:desktopTex.Sample(linearSmp,lu).rgb;"
"if(lensColor.w>0.0 && dist>=-lensColor.w)lc=lensColor.rgb;bg=lerp(bg,lc,lensAlpha(dist));}"
"uint3 d=(uint3)round(saturate(bg)*255.0);"
"return float4((float3)(m.a>0.5 ? (d^x) : x)/255.0,1);}";

static const char* kPSMaskPlain =
"cbuffer CB:register(b0){float4 posRect;float4 uvA;float4 uvB;float4 baseRect;float4 baseA;float4 baseB;};"
"Texture2D maskTex:register(t0);Texture2D desktopTex:register(t1);"
"SamplerState smp:register(s0);SamplerState pointSmp:register(s1);"
"struct VO{float4 pos:SV_Position;float2 uv:TEXCOORD;};"
"float4 main(VO i):SV_Target{"
"float2 q=(i.pos.xy-baseRect.xy)/baseRect.zw;"
"float2 u=baseA.xy+q.x*(baseA.zw-baseA.xy)+q.y*(baseB.xy-baseA.xy);"
"float4 m=maskTex.Sample(pointSmp,i.uv);"
"uint3 x=(uint3)round(saturate(m.rgb)*255.0);"
"uint3 d=(uint3)round(saturate(desktopTex.Sample(smp,u).rgb)*255.0);"
"return float4((float3)(m.a>0.5 ? (d^x) : x)/255.0,1);}";

// ----------------------------------------------------------------------------
// Build all geometry + D3D resources from the config. Returns false + reason.
// ----------------------------------------------------------------------------
bool Mirror::init(const MirrorConfig& cfg, const std::vector<MonitorInfo>& mons, HWND hwnd, std::wstring& err) {
    window = hwnd;
    int si = ResolveMonitor(mons, cfg.source);
    int ti = ResolveMonitor(mons, cfg.target);
    if (si < 0 || ti < 0) { err = L"A configured monitor is not connected."; return false; }

    // ---- find the DXGI output for the working monitor (by current device name) ----
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf()))) {
        err = L"Direct3D initialization failed (CreateDXGIFactory1)."; return false; }
    ComPtr<IDXGIAdapter1> ad;
    bool found = false;
    for (UINT ai = 0; !found && SUCCEEDED(factory->EnumAdapters1(ai, ad.ReleaseAndGetAddressOf())); ++ai) {
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; SUCCEEDED(ad->EnumOutputs(oi, o.ReleaseAndGetAddressOf())); ++oi) {
            DXGI_OUTPUT_DESC d{}; if (FAILED(o->GetDesc(&d))) continue;
            if (!d.AttachedToDesktop) continue;
            if (mons[si].device == d.DeviceName) {
                srcAdapter = ad; o.As(&srcOutput); found = true; break;
            }
        }
    }
    if (!found || !srcOutput) { err = L"Could not open the working monitor for capture."; return false; }

    activeSource = si;
    // ---- geometry ----
    cropX = cfg.source.x; cropY = cfg.source.y; cropW = cfg.source.w; cropH = cfg.source.h;
    vsync = cfg.vsync; showCursor = cfg.cursor; lowLatency = cfg.lowLatency;
    magnifier=cfg.magnifier; magnifier.normalize();
    windowed = cfg.windowed; rotation = cfg.rotation; flipH = cfg.flipH; flipV = cfg.flipV;
    computeUVs(rotation, flipH, flipV, uvRot);
    drawAspect = (rotation == 90 || rotation == 270)
        ? (cropW ? (double)cropH / cropW : 1.0)
        : (cropH ? (double)cropW / cropH : 1.0);

    if (windowed) {
        // floating window: client == mirror rectangle size; letterboxed inside.
        winW = cfg.target.w; winH = cfg.target.h;
        fitDrawRect();
    } else if (cfg.cover) {
        // fill the mirror monitor black; mirror drawn as a sub-rectangle.
        winW = mons[ti].w(); winH = mons[ti].h();
        drawX = cfg.target.x; drawY = cfg.target.y; drawW = cfg.target.w; drawH = cfg.target.h;
    } else {
        // window is exactly the mirror rectangle; desktop around it shows through.
        winW = cfg.target.w; winH = cfg.target.h;
        drawX = 0; drawY = 0; drawW = cfg.target.w; drawH = cfg.target.h;
    }
    bool oneToOne = (rotation % 180 == 0) ? (drawW == cropW && drawH == cropH)
                                          : (drawW == cropH && drawH == cropW);
    scaling = !oneToOne;
    logf("init: crop=(%d,%d %dx%d) win=%dx%d draw=(%d,%d %dx%d) rot=%d flip=%d%d windowed=%d scaling=%d",
         cropX, cropY, cropW, cropH, winW, winH, drawX, drawY, drawW, drawH,
         rotation, flipH, flipV, windowed, scaling);

    // ---- device on the source adapter ----
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(srcAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, dev.GetAddressOf(), &fl, ctx.GetAddressOf()))) {
        err = L"Direct3D device creation failed."; return false; }

    if (!createDuplication()) { err = L"Screen capture (Desktop Duplication) could not start on the working monitor."; return false; }
    updateCaptureGeometry();
    if (!createPipeline())    { err = L"The graphics pipeline could not be created."; return false; }
    if (cfg.followMouse) {
        sources.push_back({si, srcOutput, dupl, cropTex, cropSRV});
        for (int i = 0; i < (int)mons.size(); ++i) {
            if (i == si || i == ti) continue;
            SourceCache s{}; s.index = i;
            ComPtr<IDXGIOutput> output;
            if (FAILED(srcAdapter->EnumOutputs(mons[i].outputIdx, output.GetAddressOf())) ||
                FAILED(output.As(&s.output)) || FAILED(s.output->DuplicateOutput(dev.Get(), s.duplication.GetAddressOf()))) {
                err = L"Could not initialize second source capture."; return false;
            }
            D3D11_TEXTURE2D_DESC td{}; cropTex->GetDesc(&td);
            DXGI_OUTDUPL_DESC dd{}; s.duplication->GetDesc(&dd);
            td.Width = dd.ModeDesc.Width; td.Height = dd.ModeDesc.Height;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, s.texture.GetAddressOf())) ||
                FAILED(dev->CreateShaderResourceView(s.texture.Get(), nullptr, s.srv.GetAddressOf()))) {
                err = L"Could not allocate second source texture."; return false;
            }
            sources.push_back(s);
        }
        fitDrawRect(); scaling = drawW != cropW || drawH != cropH;
    }
    return true;
}

bool Mirror::selectSource(int index) {
    if (index == activeSource) return true;
    if (!releaseFrame()) return false;
    for (auto& old : sources) if (old.index == activeSource) old.valid = frameValid;
    for (auto& s : sources) if (s.index == index) {
        // Keep the same device, swap chain and window; only change capture resources.
        srcOutput = s.output; dupl = s.duplication; cropTex = s.texture; cropSRV = s.srv;
        activeSource = index;
        cropX = cropY = 0; cropW = g_monitors[index].w(); cropH = g_monitors[index].h();
        updateCaptureGeometry();
        drawAspect = (double)cropW / cropH; fitDrawRect();
        scaling = drawW != cropW || drawH != cropH;
        curVisible = false;
        frameValid = s.valid; // previously initialized image until a fresh frame arrives
        redrawNeeded = true;
        return true;
    }
    return false;
}

// Convert logical desktop crop into the unrotated duplication surface.
void Mirror::updateCaptureGeometry() {
    DXGI_OUTDUPL_DESC dd{}; dupl->GetDesc(&dd);
    int dw=g_monitors[activeSource].w(), dh=g_monitors[activeSource].h();
    rawX=cropX; rawY=cropY; rawW=cropW; rawH=cropH;
    switch(dd.Rotation) {
    case DXGI_MODE_ROTATION_ROTATE90:
        rawX=cropY; rawY=dw-cropX-cropW; rawW=cropH; rawH=cropW; break;
    case DXGI_MODE_ROTATION_ROTATE180:
        rawX=dw-cropX-cropW; rawY=dh-cropY-cropH; break;
    case DXGI_MODE_ROTATION_ROTATE270:
        rawX=dh-cropY-cropH; rawY=cropX; rawW=cropH; rawH=cropW; break;
    default: break;
    }
    for (int i=0;i<4;++i) {
        float u=uvRot[2*i], v=uvRot[2*i+1];
        switch(dd.Rotation) {
        case DXGI_MODE_ROTATION_ROTATE90: uvBase[2*i]=v; uvBase[2*i+1]=1-u; break;
        case DXGI_MODE_ROTATION_ROTATE180: uvBase[2*i]=1-u; uvBase[2*i+1]=1-v; break;
        case DXGI_MODE_ROTATION_ROTATE270: uvBase[2*i]=1-v; uvBase[2*i+1]=u; break;
        default: uvBase[2*i]=u; uvBase[2*i+1]=v; break;
        }
    }
}

bool Mirror::createDuplication() {
    dupl.Reset();
    return SUCCEEDED(srcOutput->DuplicateOutput(dev.Get(), dupl.GetAddressOf()));
}

// contain-fit the (rotation-adjusted) mirror aspect into the client, centred.
void Mirror::fitDrawRect() {
    double a = drawAspect > 0 ? drawAspect : 1.0;
    if (winW <= 0 || winH <= 0) { drawX = drawY = 0; drawW = winW; drawH = winH; return; }
    if ((double)winW / winH > a) { drawH = winH; drawW = (int)llround(winH * a); }
    else                         { drawW = winW; drawH = (int)llround(winW / a); }
    drawX = (winW - drawW) / 2; drawY = (winH - drawH) / 2;
}

// windowed live resize: resize the swapchain buffers and re-fit the mirror.
bool Mirror::resize(int w, int h) {
    if (!swap || w < 16 || h < 16) return false;
    if (w==winW && h==winH) return true;
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv.Reset();
    ctx->Flush();
    if (FAILED(swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, swapFlags))) return false;
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.GetAddressOf()))) return false;
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, rtv.GetAddressOf()))) return false;
    winW = w; winH = h;
    fitDrawRect();
    bool oneToOne = (rotation % 180 == 0) ? (drawW == cropW && drawH == cropH)
                                          : (drawW == cropH && drawH == cropW);
    scaling = !oneToOne;
    redrawNeeded = true;
    return true;
}

bool Mirror::createPipeline() {
    ComPtr<IDXGIFactory2> f2;
    ComPtr<IDXGIDevice> dxgiDev; if (FAILED(dev.As(&dxgiDev))) return false;
    ComPtr<IDXGIAdapter> ad; if (FAILED(dxgiDev->GetAdapter(ad.GetAddressOf()))) return false;
    if (FAILED(ad->GetParent(__uuidof(IDXGIFactory2), (void**)f2.GetAddressOf()))) return false;

    ComPtr<IDXGIFactory5> f5; f2.As(&f5);
    BOOL tearing = FALSE;
    if (f5) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    allowTearing = (tearing == TRUE);

    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width = winW; sc.Height = winH;
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.Scaling = DXGI_SCALING_NONE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (allowTearing) sc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    swapFlags = sc.Flags;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(f2->CreateSwapChainForHwnd(dev.Get(), window, &sc, nullptr, nullptr, sc1.GetAddressOf())))
        return false;
    f2->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(sc1.As(&swap))) return false;
    if (FAILED(swap->SetMaximumFrameLatency(1))) return false;
    waitable = swap->GetFrameLatencyWaitableObject();
    if (!waitable) return false;

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)bb.GetAddressOf()))) return false;
    if (FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, rtv.GetAddressOf()))) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = rawW; td.Height = rawH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, cropTex.GetAddressOf()))) return false;
    if (FAILED(dev->CreateShaderResourceView(cropTex.Get(), nullptr, cropSRV.GetAddressOf()))) return false;

    ComPtr<ID3DBlob> vsb, psb, e;
    if (FAILED(D3DCompile(kVS, strlen(kVS), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, vsb.GetAddressOf(), e.GetAddressOf()))) return false;
    e.Reset();
    if (FAILED(D3DCompile(kPS, strlen(kPS), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, psb.GetAddressOf(), e.GetAddressOf()))) return false;
    if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.GetAddressOf()))) return false;
    if (FAILED(dev->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.GetAddressOf()))) return false;
    psb.Reset(); e.Reset();
    bool lensOK=SUCCEEDED(D3DCompile(kPSMask, strlen(kPSMask), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, psb.GetAddressOf(), e.GetAddressOf()));
    if (!lensOK) {
        if (e) logf("magnifier mask shader compile failed: %s",(const char*)e->GetBufferPointer());
        psb.Reset(); e.Reset();
        if (FAILED(D3DCompile(kPSMaskPlain, strlen(kPSMaskPlain), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, psb.GetAddressOf(), e.GetAddressOf()))) return false;
    }
    if (FAILED(dev->CreatePixelShader(psb->GetBufferPointer(),psb->GetBufferSize(),nullptr,psMask.GetAddressOf()))) return false;
    psb.Reset(); e.Reset();
    if (lensOK && SUCCEEDED(D3DCompile(kPSLens, strlen(kPSLens), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, psb.GetAddressOf(), e.GetAddressOf()))) {
        if (FAILED(dev->CreatePixelShader(psb->GetBufferPointer(),psb->GetBufferSize(),nullptr,psLens.GetAddressOf()))) psLens.Reset();
    } else if (e) logf("magnifier shader compile failed: %s",(const char*)e->GetBufferPointer());

    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, cb.GetAddressOf()))) return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(dev->CreateSamplerState(&sd, sampPoint.GetAddressOf()))) return false;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (FAILED(dev->CreateSamplerState(&sd, sampLinear.GetAddressOf()))) return false;

    D3D11_BLEND_DESC bo = {};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bo, blendOpaque.GetAddressOf()))) return false;
    D3D11_BLEND_DESC ba = {};
    auto& rt = ba.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;  rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;   rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&ba, blendAlpha.GetAddressOf()))) return false;

    // rasterizer with scissor enabled so the (scaled) mirror + cursor stay
    // clipped to the draw rectangle, leaving the rest of the monitor black.
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE; rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, rsScissor.GetAddressOf()))) return false;

    return true;
}

void Mirror::buildCursorTexture(const std::vector<BYTE>& bgra, int w, int h) {
    curTex.Reset(); curSRV.Reset();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = {}; srd.pSysMem = bgra.data(); srd.SysMemPitch = w * 4;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &srd, curTex.GetAddressOf())))
        dev->CreateShaderResourceView(curTex.Get(), nullptr, curSRV.GetAddressOf());
    curW = w; curH = h;
}

void Mirror::updateCursor(IDXGIOutputDuplication* d, const DXGI_OUTDUPL_FRAME_INFO& fi) {
    if (fi.LastMouseUpdateTime.QuadPart != 0) {
        curVisible = fi.PointerPosition.Visible;
        curX = fi.PointerPosition.Position.x;
        curY = fi.PointerPosition.Position.y;
    }
    if (fi.PointerShapeBufferSize == 0) return;

    std::vector<BYTE> buf(fi.PointerShapeBufferSize);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
    UINT got = 0;
    HRESULT hr=d->GetFramePointerShape(fi.PointerShapeBufferSize, buf.data(), &got, &si);
    if (FAILED(hr)) { if (hr!=DXGI_ERROR_MORE_DATA) lastError=hr; return; }

    int w = si.Width, h = si.Height;
    if (w<=0 || h<=0 || w>4096 || h>8192 || (size_t)si.Pitch*h>buf.size()) return;
    if (si.Type==DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) { if (h%2 || si.Pitch<(UINT)((w+7)/8)) return; }
    else if (si.Pitch<(UINT)(w*4)) return;
    curMask = si.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    std::vector<BYTE> out;
    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        out.resize(w * h * 4);
        for (int y = 0; y < h; ++y)
            memcpy(&out[y * w * 4], &buf[y * si.Pitch], w * 4);
    } else if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        out.resize(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int s = y * si.Pitch + x * 4, o = (y * w + x) * 4;
            BYTE b = buf[s+0], g = buf[s+1], r = buf[s+2], mask = buf[s+3];
            out[o]=b; out[o+1]=g; out[o+2]=r; out[o+3]=mask;
        }
    } else { // MONOCHROME
        h = si.Height / 2;
        out.resize(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            int bit = 7 - (x % 8);
            int andB = (buf[y * si.Pitch + (x / 8)] >> bit) & 1;
            int xorB = (buf[(h + y) * si.Pitch + (x / 8)] >> bit) & 1;
            BYTE v = xorB ? 255 : 0;
            BYTE a = andB ? 255 : 0;
            int o = (y*w + x)*4; out[o+0]=v; out[o+1]=v; out[o+2]=v; out[o+3]=a;
        }
    }
    if (!out.empty()) buildCursorTexture(out, w, h);
}

bool Mirror::releaseFrame() {
    if (!frameHeld) return true;
    frameHeld = false;
    HRESULT hr = dupl->ReleaseFrame();
    if (FAILED(hr)) { lastError = hr; return false; }
    return true;
}

void Mirror::reset() {
    releaseFrame();
    if (ctx) { ctx->ClearState(); ctx->Flush(); }
    if (waitable) CloseHandle(waitable);
    *this = Mirror();
}

void Mirror::applyOptions(const MirrorConfig& cfg) {
    showCursor = cfg.cursor; lowLatency = cfg.lowLatency; vsync = cfg.vsync;
    magnifier=cfg.magnifier; magnifier.normalize();
    if (!cfg.followMouse) {
        rotation = cfg.rotation; flipH = cfg.flipH; flipV = cfg.flipV;
        computeUVs(rotation, flipH, flipV, uvRot);
        updateCaptureGeometry();
        drawAspect = (rotation % 180) ? double(cropH)/cropW : double(cropW)/cropH;
        if (windowed) fitDrawRect();
        scaling = (rotation % 180) ? drawW != cropH || drawH != cropW : drawW != cropW || drawH != cropH;
    }
    redrawNeeded = true;
}

// No input hooks. Toggle is delivered by the controller; hold mode samples
// the keyboard only when a frame can be presented. The actual cursor is untouched.
void Mirror::updateLens() {
    bool visible=false;
    float ax=0,ay=0,cx=0,cy=0,hw=0,hh=0;
    bool on=magnifier.enabled && (magnifier.mode==2 || (magnifier.mode==0 && g_lensToggled.load()));
    if(magnifier.enabled && magnifier.mode==1) {
        auto down=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
        on=down(magnifier.key) && (!(magnifier.modifiers&MOD_CONTROL)||down(VK_CONTROL)) &&
            (!(magnifier.modifiers&MOD_ALT)||down(VK_MENU)) && (!(magnifier.modifiers&MOD_SHIFT)||down(VK_SHIFT));
    }
    if(on && psLens && activeSource>=0 && activeSource<(int)g_monitors.size() && cropW>0 && cropH>0 && drawW>0 && drawH>0) {
        POINT pt{}; const RECT& r=g_monitors[activeSource].rect;
        if(GetCursorPos(&pt)) {
            float x=(pt.x-r.left-cropX+0.5f)/cropW, y=(pt.y-r.top-cropY+0.5f)/cropH;
            auto g=MakeLensGeometry(x,y,rotation,flipH,flipV,drawX,drawY,drawW,drawH,
                                    magnifier.width,magnifier.height,magnifier.edge==1);
            visible=g.visible; ax=g.ax; ay=g.ay; cx=g.cx; cy=g.cy; hw=g.hw; hh=g.hh;
        }
    }
    if(visible!=lensVisible || (visible && (ax!=lensAX || ay!=lensAY || cx!=lensCX || cy!=lensCY || hw!=lensHalfW || hh!=lensHalfH))) redrawNeeded=true;
    lensVisible=visible; lensAX=ax; lensAY=ay; lensCX=cx; lensCY=cy; lensHalfW=hw; lensHalfH=hh;
}

void Mirror::renderFrame() {
    if (!dupl || !rtv) { lastError = E_FAIL; return; }
    // Wait for presentation capacity first, then acquire the freshest desktop.
    // Keep ownership until the next acquisition to avoid redundant OS copies.
    if (!releaseFrame()) return;
    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO fi = {};
    HRESULT hr = dupl->AcquireNextFrame(lowLatency && !redrawNeeded ? 2 : 0, &fi, res.GetAddressOf());
    bool changed = false;
    if (SUCCEEDED(hr)) {
        frameHeld = true;
        ComPtr<ID3D11Texture2D> frameTex;
        if (FAILED(res.As(&frameTex))) { lastError = E_FAIL; return; }
        D3D11_TEXTURE2D_DESC fd{}; frameTex->GetDesc(&fd);
        if (rawX < 0 || rawY < 0 || (int)fd.Width < rawX + rawW || (int)fd.Height < rawY + rawH) { lastError = DXGI_ERROR_ACCESS_LOST; return; }
        if (!frameValid || fi.LastPresentTime.QuadPart != 0) {
            D3D11_BOX box = { (UINT)rawX, (UINT)rawY, 0, (UINT)(rawX+rawW), (UINT)(rawY+rawH), 1 };
            ID3D11ShaderResourceView* unbind[2]={nullptr,nullptr};
            ctx->PSSetShaderResources(0, 2, unbind);
            ctx->CopySubresourceRegion(cropTex.Get(), 0, 0, 0, 0, frameTex.Get(), 0, &box);
            frameValid = true; changed = true; ++captures; accumulated += fi.AccumulatedFrames;
            if (fi.LastPresentTime.QuadPart) {
                LARGE_INTEGER now{}, freq{}; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
                frameAgeMs = 1000.0 * (now.QuadPart - fi.LastPresentTime.QuadPart) / freq.QuadPart;
            }
        }
        updateCursor(dupl.Get(), fi);
        changed = changed || (showCursor && (fi.LastMouseUpdateTime.QuadPart || fi.PointerShapeBufferSize));
    } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) { lastError = hr; return; }
    updateLens();
    if (lowLatency && !changed && !redrawNeeded) return;

    // ---- render ----
    float black[4] = {0,0,0,1};
    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    ctx->ClearRenderTargetView(rtv.Get(), black);   // letterbox / margins stay black

    D3D11_VIEWPORT vp = { 0, 0, (float)winW, (float)winH, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    D3D11_RECT scissor = { drawX, drawY, drawX + drawW, drawY + drawH };
    ctx->RSSetScissorRects(1, &scissor);
    ctx->RSSetState(rsScissor.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11SamplerState* smp = (scaling ? sampLinear : sampPoint).Get();
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->PSSetSamplers(1, 1, sampPoint.GetAddressOf());
    ctx->PSSetSamplers(2, 1, sampLinear.GetAddressOf());
    float bf[4] = {0,0,0,0};

    // window-pixel rect + per-corner UVs -> NDC quad
    auto pxQuad = [&](float x, float y, float w, float h, const float uv[8],
                      ID3D11ShaderResourceView* srv, ID3D11BlendState* bs){
        float x0 =  (x        / winW) * 2.f - 1.f;
        float x1 = ((x + w)   / winW) * 2.f - 1.f;
        float y0 = 1.f - (y        / winH) * 2.f;
        float y1 = 1.f - ((y + h)  / winH) * 2.f;
        if (FAILED(lastError)) return;
        D3D11_MAPPED_SUBRESOURCE ms{};
        HRESULT mapped = ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        if (FAILED(mapped)) { lastError = mapped; return; }
        CB c{}; c.posRect[0]=x0; c.posRect[1]=y0; c.posRect[2]=x1; c.posRect[3]=y1;
        memcpy(c.uv, uv, sizeof(float) * 8);
        c.baseRect[0]=float(drawX); c.baseRect[1]=float(drawY); c.baseRect[2]=float(drawW); c.baseRect[3]=float(drawH);
        memcpy(c.baseUV,uvBase,sizeof(c.baseUV));
        c.lensRect[0]=lensCX; c.lensRect[1]=lensCY; c.lensRect[2]=lensHalfW; c.lensRect[3]=lensHalfH;
        c.lensParams[0]=lensAX; c.lensParams[1]=lensAY; c.lensParams[2]=magnifier.zoom/100.f; c.lensParams[3]=float(magnifier.shape);
        c.lensColor[0]=GetRValue(magnifier.color)/255.f; c.lensColor[1]=GetGValue(magnifier.color)/255.f; c.lensColor[2]=GetBValue(magnifier.color)/255.f; c.lensColor[3]=lensVisible?std::min(float(magnifier.border),std::max(0.f,std::min(lensHalfW,lensHalfH)-1.f)):0.f;
        c.lensFlags[0]=lensVisible?1.f:0.f; c.lensFlags[1]=magnifier.sharp?1.f:0.f;
        c.lensFlags[2]=std::min(float(magnifier.feather),std::max(0.f,std::min(lensHalfW,lensHalfH)-1.f));
        memcpy(ms.pData, &c, sizeof(c)); ctx->Unmap(cb.Get(), 0);
        ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->OMSetBlendState(bs, bf, 0xffffffff);
        ctx->Draw(4, 0);
    };

    // map a normalized source point (0..1) into normalized OUTPUT space (rotation+flip)
    auto Txf = [&](float nx, float ny, float& ox, float& oy){
        switch (rotation) {
            case 90:  ox = 1.f - ny; oy = nx;        break;
            case 180: ox = 1.f - nx; oy = 1.f - ny;  break;
            case 270: ox = ny;       oy = 1.f - nx;  break;
            default:  ox = nx;       oy = ny;        break;
        }
        if (flipH) ox = 1.f - ox;
        if (flipV) oy = 1.f - oy;
    };

    // base: the mirrored region, rotated/flipped into the draw rectangle
    if (frameValid) pxQuad((float)drawX, (float)drawY, (float)drawW, (float)drawH, uvBase, cropSRV.Get(), blendOpaque.Get());

    // Draw only the lens's bounding quad, sampling the original GPU cache.
    // The shader discards pixels outside the chosen shape, not a CPU screenshot.
    if(frameValid && lensVisible) {
        ctx->PSSetShader(psLens.Get(),nullptr,0);
        ID3D11SamplerState* lensSampler=(magnifier.sharp?sampPoint:sampLinear).Get();
        ctx->PSSetSamplers(0,1,&lensSampler);
        pxQuad(lensCX-lensHalfW,lensCY-lensHalfH,lensHalfW*2,lensHalfH*2,uvBase,cropSRV.Get(),blendAlpha.Get());
        ctx->PSSetShader(ps.Get(),nullptr,0);
        ctx->PSSetSamplers(0,1,&smp);
    }

    // cursor: transform its source-space bounding box the same way, rotate the bitmap to match
    if (frameValid && showCursor && curVisible && curSRV && curW > 0 && cropW > 0 && cropH > 0) {
        float nx0 = (curX - cropX) / (float)cropW, ny0 = (curY - cropY) / (float)cropH;
        float nx1 = (curX - cropX + curW) / (float)cropW, ny1 = (curY - cropY + curH) / (float)cropH;
        float ax, ay, bx, by; Txf(nx0, ny0, ax, ay); Txf(nx1, ny1, bx, by);
        float ox0 = std::min(ax,bx), oy0 = std::min(ay,by), ox1 = std::max(ax,bx), oy1 = std::max(ay,by);
        float rx = drawX + ox0 * drawW, ry = drawY + oy0 * drawH;
        float rw = (ox1 - ox0) * drawW, rh = (oy1 - oy0) * drawH;
        // Keep the pointer readable at normal size; move its hotspot along
        // with the sampled area when the lens is shifted in from an edge.
        if(lensVisible) {rx+=lensCX-lensAX; ry+=lensCY-lensAY;}
        if (rx + rw > drawX && ry + rh > drawY && rx < drawX + drawW && ry < drawY + drawH) {
            if (curMask) {
                ctx->PSSetShader(psMask.Get(),nullptr,0);
                ctx->PSSetShaderResources(1,1,cropSRV.GetAddressOf());
            }
            pxQuad(rx, ry, rw, rh, uvRot, curSRV.Get(), curMask ? blendOpaque.Get() : blendAlpha.Get());
        }
    }

    if (FAILED(lastError)) return;
    UINT flags = (!vsync && allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT presented = swap->Present(vsync ? 1 : 0, flags);
    presentationReady = false; // a Present consumes the acquired presentation slot
    if (FAILED(presented)) lastError = presented;
    else if (presented == DXGI_STATUS_OCCLUDED) occluded=true;
    else { redrawNeeded = false; ++presents; }
}

// ----------------------------------------------------------------------------
// Tray
// ----------------------------------------------------------------------------
static HICON makeTrayIcon(bool on) {
    const int S = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi); bi.bV5Width = S; bi.bV5Height = -S;
    bi.bV5Planes = 1; bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) { if (color) DeleteObject(color); return nullptr; }
    DWORD* px = (DWORD*)bits;

    const DWORD CLEAR = 0x00000000;
    const DWORD FRAME = 0xFF202833;
    const DWORD DIM   = on ? 0xFF14323A : 0xFF2A2A2A;
    const DWORD LIT   = on ? 0xFF35D6F0 : 0xFF555555;
    for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
        DWORD c = CLEAR;
        bool frame = (x >= 2 && x <= 29 && y >= 8 && y <= 23);
        bool inner = (x >= 4 && x <= 27 && y >= 10 && y <= 21);
        bool stand = (x >= 14 && x <= 17 && y >= 24 && y <= 26);
        if (frame) c = FRAME;
        if (inner) c = (x >= 16) ? LIT : DIM;
        if (stand) c = FRAME;
        px[y * S + x] = c;
    }
    std::vector<BYTE> zero((S * S) / 8, 0);
    HBITMAP mask = CreateBitmap(S, S, 1, 1, zero.data());
    ICONINFO ii = {}; ii.fIcon = TRUE; ii.hbmColor = color; ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask);
    return ic;
}

static void addTray(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_icon = makeTrayIcon(true);
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, L"Mirror: ON (left-click to disable)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void removeTray() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_icon) { DestroyIcon(g_icon); g_icon = nullptr; }
}
static void stopMouseLock();
static void updateTray(bool on) {
    HICON old = g_icon;
    g_icon = makeTrayIcon(on);
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, on ? L"Mirror: ON (left-click to disable)" : L"Mirror: OFF / waiting");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    if (old) DestroyIcon(old);
}
static void setEnabled(bool on) { g_enabled = on; wakeRenderer(); }
static void showMenu(HWND hwnd) {
    POINT p; GetCursorPos(&p);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_enabled ? MF_CHECKED : 0), IDM_TOGGLE, L"Enabled");
    AppendMenuW(menu, MF_STRING, IDM_EDIT, L"Edit setup...");
    AppendMenuW(menu, MF_STRING, IDM_DIAGNOSTICS, L"Performance diagnostics...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// Cursor exclusion: native rectangular clip when possible; otherwise a
// dedicated polling worker. No low-level hooks or input-event interception.
static bool g_mouseLocked = false;
static RECT g_lockRect{};
static HANDLE g_cursorStop = nullptr, g_cursorThread = nullptr;
static RECT g_cursorTarget{};
static std::vector<RECT> g_cursorSources;
static DWORD WINAPI cursorWorker(LPVOID) {
    DWORD delay = 2;
    while (WaitForSingleObject(g_cursorStop, delay) == WAIT_TIMEOUT) {
        POINT p{};
        if (!GetCursorPos(&p)) continue;
        RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        RECT existing{};
        if (!GetClipCursor(&existing) || !EqualRect(&desktop, &existing)) { delay = 12; continue; }
        if (!PtInRect(&g_cursorTarget, p)) {
            LONG dx = std::max({g_cursorTarget.left-p.x, p.x-g_cursorTarget.right+1, 0L});
            LONG dy = std::max({g_cursorTarget.top-p.y, p.y-g_cursorTarget.bottom+1, 0L});
            delay = (dx > 150 || dy > 150) ? 8 : 2;
            continue;
        }
        delay = 2;
        POINT best{};
        double bestDist = 1e100;
        for (const RECT& r : g_cursorSources) {
            POINT q{std::clamp(p.x, r.left, r.right - 1),
                    std::clamp(p.y, r.top, r.bottom - 1)};
            double dx = double(p.x) - q.x, dy = double(p.y) - q.y;
            double dist = dx*dx + dy*dy;
            if (dist < bestDist) { bestDist = dist; best = q; }
        }
        RECT clip{};
        if (bestDist < 1e100 && GetClipCursor(&clip) && PtInRect(&clip, best)) {
            POINT now{};
            if (GetCursorPos(&now) && PtInRect(&g_cursorTarget, now))
                SetCursorPos(best.x, best.y);
        }
    }
    return 0;
}
static void stopMouseLock() {
    if (g_cursorThread) {
        SetEvent(g_cursorStop);
        WaitForSingleObject(g_cursorThread, INFINITE);
        CloseHandle(g_cursorThread); CloseHandle(g_cursorStop);
        g_cursorThread = g_cursorStop = nullptr;
        g_cursorSources.clear();
    }
    if (g_mouseLocked) {
        RECT current{};
        if (GetClipCursor(&current) && EqualRect(&current, &g_lockRect))
            ClipCursor(nullptr);
        g_mouseLocked = false;
    }
}
static bool startMouseLock() {
    if (g_cursorThread) return true;
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT current{};
    if (!GetClipCursor(&current)) return false;
    if (g_mouseLocked) {
        if (EqualRect(&current, &g_lockRect)) return true;
        if (!EqualRect(&current, &desktop)) { g_mouseLocked=false; return true; } // respect another app's clip
        return ClipCursor(&g_lockRect) != FALSE;
    }
    int ti = ResolveMonitor(g_monitors, g_cfg.target);
    if (ti < 0) return false;
    RECT bounds{};
    bool first = true;
    g_cursorSources.clear();
    for (int i = 0; i < (int)g_monitors.size(); ++i) {
        if (i == ti) continue;
        const RECT& r = g_monitors[i].rect;
        g_cursorSources.push_back(r);
        if (first) { bounds = r; first = false; }
        else { RECT merged{}; UnionRect(&merged, &bounds, &r); bounds = merged; }
    }
    if (first) return false;
    RECT overlap{};
    if (!IntersectRect(&overlap, &bounds, &g_monitors[ti].rect)) {
        if (!EqualRect(&current, &desktop)) return true;
        if (!ClipCursor(&bounds)) return false;
        g_lockRect = bounds;
        g_mouseLocked = true;
        return true;
    }
    g_cursorTarget = g_monitors[ti].rect;
    g_cursorStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_cursorStop) return false;
    g_cursorThread = CreateThread(nullptr, 0, cursorWorker, nullptr, 0, nullptr);
    if (!g_cursorThread) { CloseHandle(g_cursorStop); g_cursorStop = nullptr; return false; }
    return true;
}

// The UI thread owns all HWNDs and the tray. The rendering thread exclusively
// owns D3D, configuration and cursor confinement. Only requests cross threads.
struct WindowSpec { DWORD style, ex; int x,y,w,h; const wchar_t* title; };
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) && wp != SIZE_MINIMIZED) {
            g_resize = (unsigned long)lp; wakeRenderer();
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp; mmi->ptMinTrackSize.x=160; mmi->ptMinTrackSize.y=120; return 0;
    }
    case WM_CLOSE: setEnabled(false); return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { g_running=false; wakeRenderer(); } return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static LRESULT CALLBACK ControllerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) { if (g_trayAdded) Shell_NotifyIconW(NIM_ADD,&g_nid); return 0; }
    switch (msg) {
    case WM_MAG_HOTKEY_CONFIG:
        UnregisterHotKey(hwnd,ID_MAG_HOTKEY);
        g_lensToggled=false;
        return lp ? RegisterHotKey(hwnd,ID_MAG_HOTKEY,(UINT)wp|MOD_NOREPEAT,(UINT)lp) : TRUE;
    case WM_HOTKEY:
        if(wp==ID_MAG_HOTKEY) {g_lensToggled=!g_lensToggled.load(); wakeRenderer(); return 0;}
        break;
    case WM_TRAY:
        if (LOWORD(lp)==WM_LBUTTONUP) setEnabled(!g_enabled);
        else if (LOWORD(lp)==WM_RBUTTONUP) showMenu(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp)==IDM_TOGGLE) setEnabled(!g_enabled);
        else if (LOWORD(wp)==IDM_EDIT) { g_showConfig=true; wakeRenderer(); }
        else if (LOWORD(wp)==IDM_DIAGNOSTICS) { g_diagnostics=true; wakeRenderer(); }
        else if (LOWORD(wp)==IDM_QUIT) { g_running=false; wakeRenderer(); }
        return 0;
    case WM_DISPLAYCHANGE:
        g_reconfigure=true; wakeRenderer(); return 0;
    case WM_POWERBROADCAST:
        if (wp==PBT_APMRESUMEAUTOMATIC || wp==PBT_APMRESUMESUSPEND) { g_reconfigure=true; wakeRenderer(); }
        return TRUE;
    case WM_CREATE_MIRROR: {
        auto& w = *reinterpret_cast<WindowSpec*>(lp);
        return (LRESULT)CreateWindowExW(w.ex,kClassName,w.title,w.style,w.x,w.y,w.w,w.h,nullptr,nullptr,g_hInst,nullptr);
    }
    case WM_DESTROY_MIRROR: DestroyWindow((HWND)lp); return 0;
    case WM_SHOW_MIRROR: ShowWindow((HWND)lp,(int)wp); return 0;
    case WM_TRAY_STATE: updateTray(wp != 0); return 0;
    case WM_DIAGNOSTICS: {
        std::wstring* message=reinterpret_cast<std::wstring*>(lp);
        // This runs on the UI thread, not the renderer.
        MessageBoxW(hwnd,message->c_str(),L"Mirror - Performance diagnostics",MB_OK|MB_ICONINFORMATION);
        delete message; g_diagnosticsOpen=false; wakeRenderer(); return 0;
    }
    case WM_NOTICE: {
        // Asynchronous tray notification; never block the renderer on a message box.
        std::wstring* message = reinterpret_cast<std::wstring*>(lp);
        g_nid.uFlags = NIF_INFO; g_nid.dwInfoFlags = NIIF_WARNING;
        lstrcpynW(g_nid.szInfoTitle,L"Active Monitor Mirror",64);
        lstrcpynW(g_nid.szInfo,message->c_str(),256);
        Shell_NotifyIconW(NIM_MODIFY,&g_nid);
        g_nid.uFlags = NIF_ICON|NIF_TIP|NIF_MESSAGE;
        delete message; return 0;
    }
    case WM_CLOSE: g_running=false; wakeRenderer(); return 0;
    case WM_RENDER_DONE: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
static void notice(const std::wstring& message) {
    auto* copy = new std::wstring(message);
    if (!PostMessageW(g_controller,WM_NOTICE,0,(LPARAM)copy)) delete copy;
}
static void showMirror(bool on) {
    if (g_hwnd) SendMessageW(g_controller,WM_SHOW_MIRROR,on ? SW_SHOWNA : SW_HIDE,(LPARAM)g_hwnd);
    PostMessageW(g_controller,WM_TRAY_STATE,on,0);
}
static void destroyPipeline(Mirror& m) {
    g_windowGuard.stop(); stopMouseLock(); m.reset();
    if (g_hwnd) { SendMessageW(g_controller,WM_DESTROY_MIRROR,0,(LPARAM)g_hwnd); g_hwnd=nullptr; }
    g_resize=0;
}
static bool buildPipeline(Mirror& m, const MirrorConfig& requested, std::wstring& err) {
    auto mons = EnumMonitors();
    err = ValidateConfig(requested,mons);
    if (!err.empty()) return false;
    MirrorConfig effective = requested;
    int ti=ResolveMonitor(mons,effective.target);
    const auto& tm=mons[ti];
    if (effective.followMouse) {
        int si=FollowSource(mons,ti);
        effective.source={mons[si].device,mons[si].hwid,0,0,mons[si].w(),mons[si].h()};
        effective.target.x=effective.target.y=0; effective.target.w=tm.w(); effective.target.h=tm.h();
        effective.windowed=false; effective.cover=true; effective.rotation=0; effective.flipH=effective.flipV=false;
    }
    WindowSpec w{};
    if (effective.windowed) {
        w.style=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN; w.ex=WS_EX_APPWINDOW;
        RECT rc{0,0,effective.target.w,effective.target.h}; AdjustWindowRectEx(&rc,w.style,FALSE,w.ex);
        w.x=tm.rect.left+effective.target.x+rc.left; w.y=tm.rect.top+effective.target.y+rc.top;
        w.w=rc.right-rc.left; w.h=rc.bottom-rc.top; w.title=L"Mirror";
    } else {
        w.style=WS_POPUP; w.ex=WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP; w.title=L"Mirror";
        w.x=tm.rect.left+(effective.cover?0:effective.target.x); w.y=tm.rect.top+(effective.cover?0:effective.target.y);
        w.w=effective.cover?tm.w():effective.target.w; w.h=effective.cover?tm.h():effective.target.h;
    }
    // DXGI allows only one duplication per output per process. Tear down the
    // old pipeline only after validation; caller rebuilds old settings on failure.
    destroyPipeline(m);
    g_monitors=std::move(mons);
    g_hwnd=(HWND)SendMessageW(g_controller,WM_CREATE_MIRROR,0,(LPARAM)&w);
    if (!g_hwnd) { err=L"The mirror window could not be created."; return false; }
    if (!m.init(effective,g_monitors,g_hwnd,err)) { destroyPipeline(m); return false; }
    g_resize=0;
    return true;
}
static void launchSetup(const std::wstring& banner=L"") {
    if (g_setupThread) return;
    stopMouseLock();
    g_liveSetup.edited=g_cfg; g_liveSetup.monitors=EnumMonitors(); g_liveSetup.path=g_cfgPath;
    g_liveSetup.instance=g_hInst; g_liveSetup.result=ConfigResult::Quit;
    g_liveSetup.banner=banner; g_liveSetup.live= !banner.empty() ? false : true;
    g_setupThread=CreateThread(nullptr,0,setupWorker,&g_liveSetup,0,nullptr);
    if (!g_setupThread) notice(L"Could not open settings.");
}
static DWORD WINAPI renderWorker(LPVOID) {
    HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Mirror m;
    bool configured=LoadConfig(g_cfg,g_cfgPath), ready=false, firstPrompt=!configured;
    bool appliedEnabled=false;
    openLog(g_cfg.log);
    std::wstring err;
    if (configured) ready=buildPipeline(m,g_cfg,err);
    if (!ready) { firstPrompt=true; launchSetup(configured ? err : L"No configuration found. Choose your source and target."); }
    ULONGLONG retryAt=GetTickCount64()+1000, reportAt=GetTickCount64()+5000;
    LARGE_INTEGER freq{}, last{}; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&last);
    bool fineTimer=false, lockWarned=false, guardWarned=false;
    int registeredKey=0, registeredMods=0;
    bool lensWarned=false;
    ULONGLONG sampleAt=GetTickCount64();
    unsigned long long prevPresents=0, prevCopies=0;
    double presentRate=0, copyRate=0;
    while (g_running) {
        if (g_showConfig.exchange(false)) launchSetup();
        if (g_setupThread && WaitForSingleObject(g_setupThread,0)==WAIT_OBJECT_0) {
            CloseHandle(g_setupThread); g_setupThread=nullptr;
            if (g_liveSetup.result==ConfigResult::Save) {
                MirrorConfig candidate=g_liveSetup.edited;
                auto current=EnumMonitors();
                std::wstring reason=ValidateConfig(candidate,current);
                bool rebuilt=false;
                bool ok=reason.empty();
                bool rebuild=ok && (!ready || !samePipeline(g_cfg,candidate));
                if (rebuild) { rebuilt=true; ok=buildPipeline(m,candidate,reason); }
                if (ok && !SaveConfig(candidate,g_cfgPath)) { ok=false; reason=L"Could not save configuration; previous settings retained."; }
                if (ok) {
                    g_cfg=candidate; configured=true; ready=true; firstPrompt=false;
                    m.applyOptions(g_cfg); openLog(g_cfg.log); stopMouseLock();
                    guardWarned=false;
                    appliedEnabled=ready && g_enabled; showMirror(appliedEnabled); lockWarned=false;
                    logf("configuration applied");
                } else {
                    if (rebuilt) {
                        std::wstring rollback;
                        ready=configured && buildPipeline(m,g_cfg,rollback);
                        if (!ready) { destroyPipeline(m); logf("rollback failed: %ls",rollback.c_str()); }
                        appliedEnabled=false;
                        if (!ready) showMirror(false);
                    }
                    notice(reason); logf("configuration rejected: %ls",reason.c_str());
                    if (!configured) launchSetup(reason);
                }
            } else if (firstPrompt && !configured) { g_running=false; break; }
            firstPrompt=!configured;
        }
        bool displayChanged=g_reconfigure.exchange(false);
        if (displayChanged && configured) { destroyPipeline(m); ready=false; appliedEnabled=false; showMirror(false); retryAt=GetTickCount64()+250; }
        if (!ready && configured && g_enabled && GetTickCount64()>=retryAt) {
            ready=buildPipeline(m,g_cfg,err); retryAt=GetTickCount64()+1000;
            if (ready) { appliedEnabled=false; lockWarned=false; logf("capture recovered"); }
            else { destroyPipeline(m); logf("waiting for display/capture: %ls",err.c_str()); }
        }
        bool active=ready && g_enabled;
        const auto& mag=g_cfg.magnifier;
        if(active && mag.enabled && !m.psLens && !lensWarned) {
            notice(L"Magnifier shader is unavailable; normal mirroring continues."); lensWarned=true;
        }
        if(!mag.enabled || (ready && m.psLens)) lensWarned=false;
        int wantKey=active && m.psLens && mag.enabled && mag.mode==0 ? mag.key : 0;
        int wantMods=wantKey ? mag.modifiers : 0;
        if(wantKey!=registeredKey || wantMods!=registeredMods) {
            bool ok=SendMessageW(g_controller,WM_MAG_HOTKEY_CONFIG,wantMods,wantKey)!=0;
            registeredKey=wantKey; registeredMods=wantMods;
            if(!ok) notice(L"Magnifier shortcut is already in use or unavailable. Choose a different shortcut, Hold hotkey, or Always on.");
        }
        ULONGLONG sampleNow=GetTickCount64();
        if (!ready || m.presents<prevPresents || m.captures<prevCopies) {
            sampleAt=sampleNow; prevPresents=prevCopies=0; presentRate=copyRate=0;
        } else if (sampleNow-sampleAt>=1000) {
            double seconds=(sampleNow-sampleAt)/1000.0;
            presentRate=(m.presents-prevPresents)/seconds; copyRate=(m.captures-prevCopies)/seconds;
            prevPresents=m.presents; prevCopies=m.captures; sampleAt=sampleNow;
        }
        if (g_diagnostics.exchange(false) && !g_diagnosticsOpen.exchange(true)) {
            stopMouseLock();
            wchar_t details[1024]{};
            std::wstring src=L"(none)", target=L"(none)";
            int hz=0, targetHz=0;
            if (ready && m.activeSource>=0 && m.activeSource<(int)g_monitors.size()) {
                src=g_monitors[m.activeSource].label();
                DEVMODEW dm{}; dm.dmSize=sizeof(dm);
                if (EnumDisplaySettingsW(g_monitors[m.activeSource].device.c_str(),ENUM_CURRENT_SETTINGS,&dm)) hz=dm.dmDisplayFrequency;
                int ti=ResolveMonitor(g_monitors,g_cfg.target);
                if (ti>=0) {
                    target=g_monitors[ti].label(); dm={}; dm.dmSize=sizeof(dm);
                    if (EnumDisplaySettingsW(g_monitors[ti].device.c_str(),ENUM_CURRENT_SETTINGS,&dm)) targetHz=dm.dmDisplayFrequency;
                }
            }
            swprintf(details,1024,L"Status: %ls\nSource: %ls (%d Hz)\nTarget: %ls (%d Hz)\nPresent submissions/s: %.1f\nDesktop copies/s: %.1f\nLast captured image age: %.2f ms\nLow latency: %ls\nV-Sync: %ls\nQueue limit: 1\n\nCapture age is measured before rendering; it is NOT display latency. Rates are sampled over roughly one second. Windows presentation mode and GPU time are not measured.",
                active?L"Running":ready?L"Paused":L"Waiting for capture",src.c_str(),hz,target.c_str(),targetHz,presentRate,copyRate,m.frameAgeMs,g_cfg.lowLatency?L"On":L"Off",g_cfg.vsync?L"On":L"Off");
            auto* copy=new std::wstring(details);
            if (!PostMessageW(g_controller,WM_DIAGNOSTICS,0,(LPARAM)copy)) { delete copy; g_diagnosticsOpen=false; }
        }
        if (active != appliedEnabled) { showMirror(active); appliedEnabled=active; m.redrawNeeded=true; if (!active && ready) m.releaseFrame(); }
        // Guard stays active during settings and diagnostics; floating mirrors do not reserve a monitor.
        if (active && g_cfg.guardWindows && (g_cfg.followMouse || !g_cfg.windowed)) {
            if (!g_windowGuard.start(g_hwnd, g_monitors, ResolveMonitor(g_monitors,g_cfg.target),
                                     ResolveMonitor(g_monitors,g_cfg.source)) && !guardWarned) {
                notice(L"Window guard is unavailable; mirroring continues."); guardWarned=true;
            }
        } else { g_windowGuard.stop(); guardWarned=false; }
        bool timerWanted=active && (!g_cfg.lowLatency && !g_cfg.followMouse && g_cfg.fps>0);
        if (timerWanted != fineTimer) {
            if (timerWanted) { fineTimer=timeBeginPeriod(1)==TIMERR_NOERROR; }
            else { timeEndPeriod(1); fineTimer=false; }
        }
        if (active && !g_setupThread && !g_diagnosticsOpen && g_cfg.lockCursor && (g_cfg.followMouse || !g_cfg.windowed)) {
            if (!startMouseLock() && !lockWarned) { notice(L"Cursor confinement is unavailable; mirroring continues."); lockWarned=true; }
        } else stopMouseLock();
        unsigned long size=g_resize.exchange(0);
        if (ready && m.windowed && LOWORD(size)>=16 && HIWORD(size)>=16 && !m.resize(LOWORD(size),HIWORD(size))) m.lastError=E_FAIL;
        if (active && SUCCEEDED(m.lastError)) {
            if (!g_cfg.lowLatency && !g_cfg.followMouse && g_cfg.fps>0) {
                LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
                double ms=1000.0/g_cfg.fps-1000.0*(now.QuadPart-last.QuadPart)/freq.QuadPart;
                if (ms>=1 && WaitForSingleObject(g_wake,(DWORD)ms)==WAIT_OBJECT_0) continue;
            }
            if (m.occluded) {
                HRESULT test=m.swap->Present(0,DXGI_PRESENT_TEST);
                if (FAILED(test)) m.lastError=test;
                else if (test!=DXGI_STATUS_OCCLUDED) { m.occluded=false; m.redrawNeeded=true; }
                else { WaitForSingleObject(g_wake,50); continue; }
            }
            HANDLE waits[]={g_wake,m.waitable};
            DWORD result=m.presentationReady ? WAIT_OBJECT_0+1 : WaitForMultipleObjects(2,waits,FALSE,50);
            if (result==WAIT_OBJECT_0) continue;
            if (result==WAIT_OBJECT_0+1) {
                m.presentationReady=true;
                if (g_cfg.followMouse) {
                    POINT pt{}; GetCursorPos(&pt);
                    int ti=ResolveMonitor(g_monitors,g_cfg.target);
                    for (int i=0;i<(int)g_monitors.size();++i)
                        if (i!=ti && PtInRect(&g_monitors[i].rect,pt)) { m.selectSource(i); break; }
                }
                if (SUCCEEDED(m.lastError)) m.renderFrame();
                if (!m.presentationReady) QueryPerformanceCounter(&last);
            } else if (result==WAIT_FAILED) m.lastError=HRESULT_FROM_WIN32(GetLastError());
            else if (m.dev) { HRESULT hr=m.dev->GetDeviceRemovedReason(); if (FAILED(hr)) m.lastError=hr; }
        } else if (!active) {
            HANDLE waits[]={g_wake,g_setupThread};
            WaitForMultipleObjects(g_setupThread?2:1,waits,FALSE,250);
        }
        if (ready && FAILED(m.lastError)) {
            logf("capture/presentation failed: 0x%08lx",(unsigned long)m.lastError);
            destroyPipeline(m); ready=false; appliedEnabled=false; showMirror(false); retryAt=GetTickCount64()+500;
        }
        if (ready && g_cfg.log && GetTickCount64()>=reportAt) {
            logf("diagnostics: presents=%llu desktop_copies=%llu accumulated_frames=%llu last_capture_age_ms=%.2f active_source=%d queue_limit=1",
                 m.presents,m.captures,m.accumulated,m.frameAgeMs,m.activeSource);
            reportAt=GetTickCount64()+5000;
        }
    }
    SendMessageW(g_controller,WM_MAG_HOTKEY_CONFIG,0,0);
    destroyPipeline(m);
    if (g_setupThread) { CloseHandle(g_setupThread); g_setupThread=nullptr; } // process exit closes live setup
    if (fineTimer) timeEndPeriod(1);
    openLog(false);
    if (SUCCEEDED(com)) CoUninitialize();
    PostMessageW(g_controller,WM_RENDER_DONE,0,0);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,PWSTR,int) {
    g_hInst=hInst;
    typedef BOOL (WINAPI* SetCtx)(HANDLE);
    if (auto p=(SetCtx)GetProcAddress(GetModuleHandleW(L"user32"),"SetProcessDpiAwarenessContext")) p((HANDLE)-4);
    HANDLE singleton=CreateMutexW(nullptr,FALSE,L"Local\\ActiveMonitorMirror.Instance");
    if (!singleton || GetLastError()==ERROR_ALREADY_EXISTS) { if (singleton) CloseHandle(singleton); return 0; }
    g_cfgPath=DefaultConfigPath();
    WNDCLASSW wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=kClassName;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hIcon=LoadIconW(hInst,MAKEINTRESOURCEW(1));
    if (!RegisterClassW(&wc)) return 1;
    wc.lpfnWndProc=ControllerProc; wc.lpszClassName=L"MirrorControllerWindow";
    if (!RegisterClassW(&wc)) return 1;
    g_controller=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"Mirror controller",WS_POPUP,0,0,0,0,nullptr,nullptr,hInst,nullptr);
    if (!g_controller) return 1;
    g_wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!g_wake) { DestroyWindow(g_controller); return 1; }
    addTray(g_controller); g_trayAdded=true; updateTray(false);
    HANDLE worker=CreateThread(nullptr,0,renderWorker,nullptr,0,nullptr);
    if (!worker) { removeTray(); CloseHandle(g_wake); DestroyWindow(g_controller); return 1; }
    MSG msg{};
    while (GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    // Worker posts WM_RENDER_DONE only after releasing D3D and windows.
    WaitForSingleObject(worker,INFINITE); CloseHandle(worker);
    removeTray(); DestroyWindow(g_controller); CloseHandle(g_wake); CloseHandle(singleton);
    ExitProcess(0); // no static teardown while an outstanding setup dialog may exist
    return 0;
}
