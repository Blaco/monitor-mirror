// Magnifier geometry in target-image pixels, independent of Win32 and D3D.
#pragma once
#include <algorithm>

struct LensGeometry {
    bool visible=false;
    float ax=0,ay=0,cx=0,cy=0,hw=0,hh=0;
};
inline LensGeometry MakeLensGeometry(float x,float y,int rotation,bool flipH,bool flipV,
                                    int dx,int dy,int dw,int dh,int width,int height,bool fit) {
    LensGeometry g;
    if(x<0 || x>=1 || y<0 || y>=1 || dw<=0 || dh<=0 || width<=0 || height<=0) return g;
    float u=x,v=y;
    switch(rotation) {case 90:u=1-y;v=x;break;case 180:u=1-x;v=1-y;break;case 270:u=y;v=1-x;break;}
    if(flipH) u=1-u;
    if(flipV) v=1-v;
    g.ax=g.cx=dx+u*dw; g.ay=g.cy=dy+v*dh;
    g.hw=std::min(width,dw)*0.5f; g.hh=std::min(height,dh)*0.5f;
    if(fit) {
        g.cx=std::clamp(g.cx,dx+g.hw,dx+dw-g.hw);
        g.cy=std::clamp(g.cy,dy+g.hh,dy+dh-g.hh);
    }
    g.visible=true;
    return g;
}
