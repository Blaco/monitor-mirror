#pragma once
// Small native modal RGB picker: hue/saturation wheel, brightness and RGB input.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace ColorPicker {
constexpr int Left=16, Top=16, Size=224, Radius=112, Brightness=10, Red=11, Green=12, Blue=13;
constexpr double Pi=3.14159265358979323846;
struct State {
    COLORREF color=RGB(255,255,255);
    double hue=0, sat=0, val=1;
    bool syncing=false, dragging=false, accepted=false, done=false;
    HWND value=nullptr;
    HWND fields[3]{};
};
inline COLORREF hsv(double h,double s,double v) {
    h=std::fmod(h+360.0,360.0); s=std::clamp(s,0.0,1.0); v=std::clamp(v,0.0,1.0);
    double c=v*s, x=c*(1-std::abs(std::fmod(h/60,2.0)-1)), m=v-c;
    double r=0,g=0,b=0;
    switch(int(h/60)) {case 0:r=c;g=x;break;case 1:r=x;g=c;break;case 2:g=c;b=x;break;
      case 3:g=x;b=c;break;case 4:r=x;b=c;break;default:r=c;b=x;break;}
    return RGB(int(std::lround((r+m)*255)),int(std::lround((g+m)*255)),int(std::lround((b+m)*255)));
}
inline void fromRGB(State& s) {
    double r=GetRValue(s.color)/255.0,g=GetGValue(s.color)/255.0,b=GetBValue(s.color)/255.0;
    double mx=std::max({r,g,b}),mn=std::min({r,g,b}),d=mx-mn;
    s.val=mx; s.sat=mx>0?d/mx:0;
    if(d>0) {double h=mx==r?(g-b)/d:mx==g?2+(b-r)/d:4+(r-g)/d; s.hue=std::fmod(h*60+360,360.0);}
}
inline void update(HWND hwnd,State& s,bool writeFields=true) {
    s.syncing=true;
    SendMessageW(s.value,TBM_SETPOS,TRUE,int(std::lround(s.val*255)));
    if(writeFields) {
        SetDlgItemInt(hwnd,Red,GetRValue(s.color),FALSE);
        SetDlgItemInt(hwnd,Green,GetGValue(s.color),FALSE);
        SetDlgItemInt(hwnd,Blue,GetBValue(s.color),FALSE);
    }
    s.syncing=false; InvalidateRect(hwnd,nullptr,FALSE);
}
inline void pick(HWND hwnd,State& s,int x,int y) {
    double dx=x-(Left+Radius),dy=y-(Top+Radius);
    s.sat=std::min(1.0,std::hypot(dx,dy)/Radius);
    if(s.sat>0) s.hue=std::fmod(std::atan2(dy,dx)*180/Pi+360,360.0);
    s.color=hsv(s.hue,s.sat,s.val); update(hwnd,s);
}
inline LRESULT CALLBACK Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    State* s=reinterpret_cast<State*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {s=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
    if(!s) return DefWindowProcW(hwnd,msg,wp,lp);
    switch(msg) {
    case WM_CREATE: {
        HINSTANCE hi=GetModuleHandleW(nullptr);
        auto child=[&](LPCWSTR cls,LPCWSTR text,DWORD style,int x,int y,int w,int h,int id=0,DWORD ex=0) {
            HWND c=CreateWindowExW(ex,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,hwnd,(HMENU)(INT_PTR)id,hi,nullptr);
            SendMessageW(c,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE); return c;
        };
        child(L"STATIC",L"Brightness",0,256,16,140,20);
        s->value=child(TRACKBAR_CLASSW,L"",WS_TABSTOP|TBS_HORZ|TBS_NOTICKS,252,40,144,30,Brightness);
        SendMessageW(s->value,TBM_SETRANGE,TRUE,MAKELPARAM(0,255));
        const wchar_t* names[]={L"Red",L"Green",L"Blue"};
        for(int i=0;i<3;++i) {child(L"STATIC",names[i],0,256,84+i*32,65,24); s->fields[i]=child(L"EDIT",L"",WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,326,82+i*32,65,25,Red+i,WS_EX_CLIENTEDGE); SendMessageW(s->fields[i],EM_SETLIMITTEXT,3,0);}
        child(L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON,224,260,80,28,IDOK);
        child(L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON,312,260,80,28,IDCANCEL);
        fromRGB(*s); update(hwnd,*s); return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd,&ps); RECT cr{}; GetClientRect(hwnd,&cr);
        FillRect(dc,&cr,GetSysColorBrush(COLOR_BTNFACE));
        std::vector<DWORD> pixels(Size*Size); COLORREF bg=GetSysColor(COLOR_BTNFACE);
        for(int y=0;y<Size;++y) for(int x=0;x<Size;++x) {
            double dx=x+0.5-Radius,dy=y+0.5-Radius,rad=std::hypot(dx,dy)/Radius;
            COLORREF c=rad<=1?hsv(std::atan2(dy,dx)*180/Pi,rad,s->val):bg;
            pixels[y*Size+x]=(DWORD(GetRValue(c))<<16)|(DWORD(GetGValue(c))<<8)|GetBValue(c);
        }
        BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=Size; bi.bmiHeader.biHeight=-Size; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
        SetDIBitsToDevice(dc,Left,Top,Size,Size,0,0,0,Size,pixels.data(),&bi,DIB_RGB_COLORS);
        int x=Left+Radius+int(std::lround(std::cos(s->hue*Pi/180)*s->sat*(Radius-1)));
        int y=Top+Radius+int(std::lround(std::sin(s->hue*Pi/180)*s->sat*(Radius-1)));
        HGDIOBJ ob=SelectObject(dc,GetStockObject(NULL_BRUSH)); HPEN black=CreatePen(PS_SOLID,3,RGB(0,0,0)),white=CreatePen(PS_SOLID,1,RGB(255,255,255));
        HGDIOBJ op=SelectObject(dc,black); Ellipse(dc,x-5,y-5,x+6,y+6); SelectObject(dc,white); Ellipse(dc,x-5,y-5,x+6,y+6); SelectObject(dc,op); SelectObject(dc,ob); DeleteObject(black); DeleteObject(white);
        RECT sw{256,182,392,238}; HBRUSH b=CreateSolidBrush(s->color); FillRect(dc,&sw,b); DeleteObject(b); FrameRect(dc,&sw,GetSysColorBrush(COLOR_WINDOWFRAME));
        EndPaint(hwnd,&ps); return 0;
    }
    case WM_LBUTTONDOWN: {
        int x=GET_X_LPARAM(lp),y=GET_Y_LPARAM(lp);
        if(std::hypot(double(x-Left-Radius),double(y-Top-Radius))<=Radius) {s->dragging=true; SetCapture(hwnd); pick(hwnd,*s,x,y);} return 0;
    }
    case WM_MOUSEMOVE: if(s->dragging) pick(hwnd,*s,GET_X_LPARAM(lp),GET_Y_LPARAM(lp)); return 0;
    case WM_LBUTTONUP: if(s->dragging){s->dragging=false; ReleaseCapture();} return 0;
    case WM_CAPTURECHANGED: s->dragging=false; return 0;
    case WM_HSCROLL: if(!s->syncing && (HWND)lp==s->value){s->val=SendMessageW(s->value,TBM_GETPOS,0,0)/255.0; s->color=hsv(s->hue,s->sat,s->val); update(hwnd,*s);} return 0;
    case WM_COMMAND: {
        int id=LOWORD(wp);
        if(id==IDOK) {
            BOOL ok[3]{}; UINT v[3]{};
            for(int i=0;i<3;++i) v[i]=GetDlgItemInt(hwnd,Red+i,&ok[i],FALSE);
            for(int i=0;i<3;++i) if(!ok[i] || v[i]>255) {
                MessageBeep(MB_ICONWARNING); SetFocus(s->fields[i]); SendMessageW(s->fields[i],EM_SETSEL,0,-1); return 0;
            }
            s->color=RGB(v[0],v[1],v[2]); s->accepted=true; DestroyWindow(hwnd); return 0;
        }
        if(id==IDCANCEL){DestroyWindow(hwnd); return 0;}
        if(!s->syncing && id>=Red && id<=Blue && HIWORD(wp)==EN_CHANGE) {
            BOOL ok[3]{}; UINT v[3]{}; for(int i=0;i<3;++i) v[i]=GetDlgItemInt(hwnd,Red+i,&ok[i],FALSE);
            if(ok[0]&&ok[1]&&ok[2]&&v[0]<=255&&v[1]<=255&&v[2]<=255){s->color=RGB(v[0],v[1],v[2]); fromRGB(*s); update(hwnd,*s,false);}
        } return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: s->done=true; return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
inline bool Choose(HWND owner,COLORREF& color) {
    HINSTANCE hi=GetModuleHandleW(nullptr); WNDCLASSW wc{}; wc.lpfnWndProc=Proc; wc.hInstance=hi; wc.lpszClassName=L"MirrorRGBWheel"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);
    if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    State s; s.color=color; RECT r{0,0,408,304}; AdjustWindowRectEx(&r,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT);
    RECT parent{}; GetWindowRect(owner,&parent); int w=r.right-r.left,h=r.bottom-r.top;
    RECT work=parent; MONITORINFO mi{}; mi.cbSize=sizeof(mi); if(GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&mi)) work=mi.rcWork;
    int x=std::clamp(int((parent.left+parent.right-w)/2),int(work.left),std::max(int(work.left),int(work.right)-w));
    int y=std::clamp(int((parent.top+parent.bottom-h)/2),int(work.top),std::max(int(work.top),int(work.bottom)-h));
    HWND hwnd=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,wc.lpszClassName,L"Border color",WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN,x,y,w,h,owner,nullptr,hi,&s);
    if(!hwnd) return false;
    EnableWindow(owner,FALSE); ShowWindow(hwnd,SW_SHOW); SetFocus(s.value);
    MSG m{}; int result=1;
    while(!s.done && (result=GetMessageW(&m,nullptr,0,0))>0) {
        if(m.message==WM_KEYDOWN && m.wParam==VK_ESCAPE){DestroyWindow(hwnd); continue;}
        if(m.message==WM_KEYDOWN && m.wParam==VK_RETURN){
            if(GetFocus()==GetDlgItem(hwnd,IDCANCEL)) SendMessageW(hwnd,WM_COMMAND,IDCANCEL,0);
            else SendMessageW(hwnd,WM_COMMAND,IDOK,0);
            continue;
        }
        if(!IsDialogMessageW(hwnd,&m)){TranslateMessage(&m); DispatchMessageW(&m);}
    }
    if(!s.done) DestroyWindow(hwnd);
    if(IsWindow(owner)){EnableWindow(owner,TRUE); SetActiveWindow(owner);}
    if(result==0) PostQuitMessage((int)m.wParam);
    if(s.accepted) color=s.color;
    return s.accepted;
}
}
