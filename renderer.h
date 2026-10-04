#ifndef RENDERER_H
#define RENDERER_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string>

struct Pixel {
    float r=0,g=0,b=0,a=0;
    float lr=0,lg=0,lb=0;
};
struct Camera{double x=0.5,y=0.5,sk=1.0;};

extern int WinW,WinH;
extern Camera g_cam;
extern GLFWwindow* g_window;
extern bool g_texDirty;
extern bool g_needRender;
extern std::string g_overlayText;
extern float g_wireAlpha;
extern float g_textClr[4];
extern float g_sunClr[3];
extern float g_wallClr[3];
extern int g_circleR;
extern float g_noise;
extern int g_batchN;
extern float g_emit;
extern bool g_useMip,g_useAcc;

// Dirty rectangle of the canvas texture, set by syncCanvas() before uploadCanvasTexture().
// Inclusive pixel bounds; x1<x0 means "nothing to upload". g_texAll forces a full upload.
extern bool g_texAll;
extern int g_texX0,g_texY0,g_texX1,g_texY1;
void markCanvasFullDirty();      // call after any wholesale change of g_pixels
void dirtyRectAdd(int x0,int y0,int x1,int y1);   // inlined helper, see below

inline void dirtyRectAdd(int x0,int y0,int x1,int y1){
    if(g_texAll)return;
    if(g_texX1<g_texX0){g_texX0=x0;g_texY0=y0;g_texX1=x1;g_texY1=y1;return;}
    if(x0<g_texX0)g_texX0=x0; if(y0<g_texY0)g_texY0=y0;
    if(x1>g_texX1)g_texX1=x1; if(y1>g_texY1)g_texY1=y1;
}

bool initRenderer(int,int);
void uploadCanvasTexture(const Pixel*,int,int);
void renderScanline(int,int);
void display(int,int);
void scr2mem(double,double,int&,int&,int,int);
void screenshot();

#endif
