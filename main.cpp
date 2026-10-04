#define _CRT_SECURE_NO_WARNINGS
#define GLEW_STATIC
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <climits>
#include <algorithm>
#include <windows.h>
#include <conio.h>
#include "config.h"
#include "grid.h"
#include "simulation.h"
#include "renderer.h"

// === Application state ===

static Config g_cfg;
static std::vector<Pixel> g_pixels;

// Authoritative colour of every cell, plus the list of cells that changed this frame.
// g_lastOverlay remembers pixels overdrawn by the selection rect / paste preview so they
// can be restored on the next frame (the old code rewrote every pixel, which hid this).
static std::vector<CellRGBA> g_colorBuf;
static std::vector<int> g_changed;
static std::vector<int> g_lastOverlay;
static bool g_canvasTexChanged = true;

static bool mouseL = false, mouseR = false;
static double msX = 0, msY = 0;
static double preMsX = 0, preMsY = 0;
static bool prevSpace = false, prevF = false;
static bool canvasDirty = true;
static bool stepFrame = false;
static bool skipUntilMove = false;

// === Non-blocking console input ===
static bool inputWait=false,inputSave=false;
static std::string inputBuf;

static void pollInput(){
  if(!inputWait)return;
  while(_kbhit()){
    int ch=_getch();
    if(ch=='\r'||ch=='\n'){
      if(!inputBuf.empty()){
        if(inputSave)saveGrid(inputBuf);
        else{loadGrid(inputBuf);g_pixels.resize((size_t)W*H);markCanvasFullDirty();}
        printf("[%s] %s\n",inputSave?"save":"load",inputBuf.c_str());
        canvasDirty=true;
      }
      inputWait=false;inputBuf.clear();printf("\n");
    }else if(ch=='\b'||ch==8){if(!inputBuf.empty())inputBuf.pop_back();printf("\r[%s] %s  ",inputSave?"SAVE":"LOAD",inputBuf.c_str());fflush(stdout);}
    else if(ch>=32&&ch<=126){inputBuf.push_back((char)ch);printf("\r[%s] %s",inputSave?"SAVE":"LOAD",inputBuf.c_str());fflush(stdout);}
  }
}

// === Pixel sync helpers ===

static void drawRectBorder(int x1, int y1, int x2, int y2,
                           float r, float g, float b, float a) {
    int cx1 = std::max(0, std::min(x1, x2));
    int cy1 = std::max(0, std::min(y1, y2));
    int cx2 = std::min(W - 1, std::max(x1, x2));
    int cy2 = std::min(H - 1, std::max(y1, y2));
    float lr = r / 3, lg = g / 3, lb = b / 3;
    for (int x = cx1; x <= cx2; x++) {
        g_pixels[cy1 * W + x] = {r, g, b, a, lr, lg, lb};
        g_pixels[cy2 * W + x] = {r, g, b, a, lr, lg, lb};
        g_lastOverlay.push_back(cy1 * W + x); g_lastOverlay.push_back(cy2 * W + x);
    }
    for (int y = cy1; y <= cy2; y++) {
        g_pixels[y * W + cx1] = {r, g, b, a, lr, lg, lb};
        g_pixels[y * W + cx2] = {r, g, b, a, lr, lg, lb};
        g_lastOverlay.push_back(y * W + cx1); g_lastOverlay.push_back(y * W + cx2);
    }
}

// g_colorBuf holds the authoritative colour of every cell; g_pixels is refreshed only
// for cells that changed (see syncGridColors). Pixels overdrawn by the selection
// rectangles / paste preview are recorded so they can be restored the next frame.
static void expandPixel(int i) {
    const CellRGBA& c = g_colorBuf[i];
    g_pixels[i] = {c.r, c.g, c.b, c.a, c.r, c.g, c.b};
}

static void syncCanvas() {
    CoordI mc;
    scr2mem(msX, msY, mc.x, mc.y, W, H);

    if ((int)g_colorBuf.size() != W * H) {
        g_colorBuf.assign((size_t)W * H, CellRGBA());
        g_changed.clear();
        markCanvasFullDirty();
    }
    syncGridColors(g_colorBuf.data(), g_changed, W, H, g_cfg.wireAlpha);

    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    auto touch = [&](int i) {
        int x = i % W, y = i / W;
        if (x < x0) x0 = x; if (y < y0) y0 = y;
        if (x > x1) x1 = x; if (y > y1) y1 = y;
    };

    // undo last frame's overlay pixels, then write the cells that actually changed
    for (size_t k = 0; k < g_lastOverlay.size(); k++) { int i = g_lastOverlay[k]; expandPixel(i); touch(i); }
    g_lastOverlay.clear();
    for (size_t k = 0; k < g_changed.size(); k++) { int i = g_changed[k]; expandPixel(i); touch(i); }

    // Paste preview
    if (flag_paste == 1) {
        for (int y = 0; y < clipH && y < H; y++) {
            for (int x = 0; x < clipW && x < W; x++) {
                int gx = mc.x - clipW / 2 + x;
                int gy = mc.y - clipH / 2 + y;
                if (gx >= 0 && gx < W && gy >= 0 && gy < H && clip_cv[x][y]) {
                    uint32_t argb = magic_c2col(clip_fi[x][y]);
                    float fr = ((argb >> 16) & 0xFF) / 255.0f;
                    float fg = ((argb >> 8) & 0xFF) / 255.0f;
                    float fb = (argb & 0xFF) / 255.0f;
                    int idx = gy * W + gx;
                    g_pixels[idx] = {fr, fg, fb, g_cfg.wireAlpha, fr, fg, fb};
                    g_lastOverlay.push_back(idx);
                    touch(idx);
                }
            }
        }
        drawRectBorder(mc.x - clipW / 2, mc.y - clipH / 2,
                       mc.x + (clipW - 1) / 2, mc.y + (clipH - 1) / 2,
                       0, 1, 0, 1);
    }

    // Copy selection rectangle
    if (flag_copy == 2) {
        drawRectBorder(clip_c.x, clip_c.y, mc.x, mc.y, 0, 1, 0, 0.6f);
    }
    if (flag_copyx == 2) {
        drawRectBorder(clip_c.x, clip_c.y, mc.x, mc.y, 1, 0, 0, 0.6f);
    }

    // Hand the touched region to the texture upload; nothing touched -> no upload at all.
    g_texX0 = x0; g_texY0 = y0; g_texX1 = x1; g_texY1 = y1;
    g_canvasTexChanged = g_texAll || (x1 >= x0);
    if (g_canvasTexChanged) g_texDirty = true;
    g_needRender = true;
}

// === Overlay text update ===

static void updateOverlayText() {
    CoordI mc;
    scr2mem(msX, msY, mc.x, mc.y, W, H);
    if (cv[mc.x][mc.y] == 0) {
        g_overlayText = "(" + std::to_string(mc.x) + ", " + std::to_string(mc.y) + ") = empty";
    } else {
        char buf[128];
        double re = fi[mc.x][mc.y].real();
        double im = fi[mc.x][mc.y].imag();
        snprintf(buf, sizeof(buf), "(%d, %d) = %.3f%+.3fi", mc.x, mc.y, re, im);
        g_overlayText = buf;
    }
}

// === GLFW callbacks ===

static void mouseButtonCb(GLFWwindow* w, int btn, int act, int mod) {
    (void)w; (void)mod;
    if (btn == GLFW_MOUSE_BUTTON_LEFT) {
        mouseL = (act == GLFW_PRESS);
        if (mouseL) { preMsX = msX; preMsY = msY; }
    }
    if (btn == GLFW_MOUSE_BUTTON_RIGHT) {
        mouseR = (act == GLFW_PRESS);
        if (mouseR) { preMsX = msX; preMsY = msY; }
    }
}

static void cursorPosCb(GLFWwindow* w, double x, double y) {
    (void)w; msX = x; msY = y;
}

static void keyCb(GLFWwindow* w, int key, int sc, int act, int mod) {
    (void)w; (void)sc;
    if (act != GLFW_PRESS && act != GLFW_REPEAT) return;

    if (mod & GLFW_MOD_CONTROL) {
        switch (key) {
        case GLFW_KEY_UP:    g_cam.sk /= 1.03f; break;
        case GLFW_KEY_DOWN:  g_cam.sk *= 1.03f; break;
        case GLFW_KEY_0:     g_cam = Camera(); break;
        case GLFW_KEY_S: inputWait=true;inputSave=true;inputBuf.clear();printf("\r[SAVE] ");fflush(stdout);break;
        case GLFW_KEY_R: inputWait=true;inputSave=false;inputBuf.clear();printf("\r[LOAD] ");fflush(stdout);break;
        case GLFW_KEY_C:
            if (flag_copy == 0 && flag_copyx == 0) flag_copy = 1;
            break;
        case GLFW_KEY_V:
            if (flag_paste == 0) flag_paste = 1;
            break;
        case GLFW_KEY_X:
            if (flag_copy == 0 && flag_copyx == 0) flag_copyx = 1;
            break;
        case GLFW_KEY_P:
            screenshot();
            break;
        }
    } else {
        switch (key) {
        case GLFW_KEY_LEFT:  g_cam.x -= 0.02 * g_cam.sk; break;
        case GLFW_KEY_UP:    g_cam.y += 0.02 * g_cam.sk; break;
        case GLFW_KEY_RIGHT: g_cam.x += 0.02 * g_cam.sk; break;
        case GLFW_KEY_DOWN:  g_cam.y -= 0.02 * g_cam.sk; break;
        case GLFW_KEY_F11: {
            static int prevX = 100, prevY = 100, prevW = WinW, prevH = WinH;
            if (glfwGetWindowMonitor(g_window)) {
                glfwSetWindowMonitor(g_window, nullptr, prevX, prevY, prevW, prevH, GLFW_DONT_CARE);
                WinW=prevW;WinH=prevH;
            } else {
                glfwGetWindowPos(g_window, &prevX, &prevY);
                glfwGetWindowSize(g_window, &prevW, &prevH);
                GLFWmonitor* mon = glfwGetPrimaryMonitor();
                const GLFWvidmode* mode = glfwGetVideoMode(mon);
                glfwSetWindowMonitor(g_window, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
                WinW=mode->width;WinH=mode->height;
            }
            break;
        }
        case GLFW_KEY_N: g_useMip=!g_useMip;printf("[mip] %d\n",(int)g_useMip);break;
        case GLFW_KEY_M: g_useAcc=!g_useAcc;g_noise=g_useAcc?0.1f:0.0f;printf("[acc] %d noise=%.2f\n",(int)g_useAcc,g_noise);break;
        case GLFW_KEY_B: g_batchN=(g_batchN>=64)?1:g_batchN*2;printf("[batch] %d\n",g_batchN);break;
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(g_window, true);
            break;
        }
    }
}

// === Mouse interaction (draw/erase/copy/paste) ===

static void handleMouse() {
    if (!mouseL && !mouseR) return;

    CoordI mc, pmc;
    scr2mem(msX, msY, mc.x, mc.y, W, H);
    scr2mem(preMsX, preMsY, pmc.x, pmc.y, W, H);

    if (mouseL) {
        if (flag_copy == 1) {
            clip_c = mc;
            flag_copy = 2;
            skipUntilMove = true;
            preMsX = msX; preMsY = msY;
            canvasDirty = true;
        } else if (flag_copy == 2 && mc != clip_c) {
            copy_cv(clip_c, mc);
            flag_copy = 0;
            skipUntilMove = true;
            preMsX = msX; preMsY = msY;
            canvasDirty = true;
        } else if (flag_copyx == 1) {
            clip_c = mc;
            flag_copyx = 2;
            skipUntilMove = true;
            preMsX = msX; preMsY = msY;
            canvasDirty = true;
        } else if (flag_copyx == 2 && mc != clip_c) {
            copy_cv(clip_c, mc);
            CoordI c1 = {std::min(clip_c.x, mc.x), std::min(clip_c.y, mc.y)};
            CoordI c2 = {std::max(clip_c.x, mc.x), std::max(clip_c.y, mc.y)};
            for (int y = c1.y; y < c2.y; y++)
                for (int x = c1.x; x < c2.x; x++) {
                    cv[x][y] = 0;
                    fi[x][y] = Complex(0, 0);
                }
            flag_copyx = 0;
            skipUntilMove = true;
            preMsX = msX; preMsY = msY;
            canvasDirty = true;
            rebuildPatterns();
        } else if (flag_paste == 1) {
            paste_cv(mc);
            rebuildPatterns();
            flag_paste = 0;
            skipUntilMove = true;
            preMsX = msX; preMsY = msY;
            canvasDirty = true;
        } else if (flag_copy != 2 && flag_copyx != 2) {
            if (!skipUntilMove || mc != pmc) {
                line_cvs(pmc, mc, 1);
                rebuildPatternsAround(std::min(pmc.x,mc.x), std::min(pmc.y,mc.y),
                                      std::max(pmc.x,mc.x), std::max(pmc.y,mc.y));
                preMsX = msX; preMsY = msY;
                canvasDirty = true;
            }
            if (mc != pmc) skipUntilMove = false;
        }
    }

    if (mouseR) {
        line_cvs(pmc, mc, 0);
        rebuildPatternsAround(std::min(pmc.x,mc.x), std::min(pmc.y,mc.y),
                              std::max(pmc.x,mc.x), std::max(pmc.y,mc.y));
        preMsX = msX; preMsY = msY;
        canvasDirty = true;
    }
}

// === Main ===

int main() {
    SetProcessDPIAware();

    // Load config
    loadConfig("cfg.txt", g_cfg);
    W = g_cfg.gridW;
    H = g_cfg.gridH;
    WinW = g_cfg.winW;
    WinH = g_cfg.winH;
    // Force window aspect ratio to match canvas (square pixels)
    if (WinW * H > WinH * W) {
        WinW = WinH * W / H;  // window too wide, shrink width
    } else if (WinW * H < WinH * W) {
        WinH = WinW * H / W;  // window too tall, shrink height
    }
    g_circleR = g_cfg.rayRadius;
    g_wireAlpha = g_cfg.wireAlpha;

    g_textClr[0] = g_cfg.textR; g_textClr[1] = g_cfg.textG;
    g_textClr[2] = g_cfg.textB; g_textClr[3] = g_cfg.textA;
    g_sunClr[0]  = g_cfg.sunR;  g_sunClr[1]  = g_cfg.sunG;  g_sunClr[2]  = g_cfg.sunB;
    g_wallClr[0] = g_cfg.wallR; g_wallClr[1] = g_cfg.wallG; g_wallClr[2] = g_cfg.wallB;
    g_emit = g_cfg.emitBrightness;

    // Init pixel buffer
    g_pixels.resize((size_t)W * H);

    // Init renderer
    if (!initRenderer(W, H)) {
        fprintf(stderr, "Renderer initialization failed\n");
        return 1;
    }
    rebuildPatterns();

    // Set callbacks
    glfwSetMouseButtonCallback(g_window, mouseButtonCb);
    glfwSetCursorPosCallback(g_window, cursorPosCb);
    glfwSetKeyCallback(g_window, keyCb);

    // FPS counter
    double lastFpsTime = glfwGetTime();
    int frameCount = 0;

    // Main loop
    while (!glfwWindowShouldClose(g_window)) {
        glfwPollEvents();

        // Mouse drawing
        handleMouse();

        // Pause / step
        bool curSpace = glfwGetKey(g_window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (curSpace && !prevSpace) flag_pause ^= 1;
        prevSpace = curSpace;

        bool curF = glfwGetKey(g_window, GLFW_KEY_F) == GLFW_PRESS;
        if (curF && !prevF) stepFrame = true;
        prevF = curF;

        // Simulation step
      double t0=glfwGetTime(),t_parse=0,t_conduct=0,t_sync=0,t_upload=0,t_render=0,t_disp=0;
      if(!flag_pause||stepFrame){
        parsing();t_parse=glfwGetTime()-t0;
        conduct();t_conduct=glfwGetTime()-t0-t_parse;
        stepFrame=false;canvasDirty=true;
      }
      if(canvasDirty){
        syncCanvas();t_sync=glfwGetTime()-t0-t_parse-t_conduct;
        canvasDirty=false;
      }
      uploadCanvasTexture(g_pixels.data(),W,H);
      t_upload=glfwGetTime()-t0-t_parse-t_conduct-t_sync;
      // Re-run the light pass only when the canvas texture actually changed, or when
      // temporal accumulation / noise dithering needs a fresh frame every time.
      bool needsEveryFrame=(g_useAcc||g_noise>0.001f);
      if(g_needRender&&(g_canvasTexChanged||needsEveryFrame)){
        renderScanline(W,H);
        t_render=glfwGetTime()-t0-t_parse-t_conduct-t_sync-t_upload;
      }
      g_needRender=false;g_canvasTexChanged=false;

        // Overlay
        updateOverlayText();
        pollInput();

        // Display
        display(W, H);
        t_disp=glfwGetTime()-t0-t_parse-t_conduct-t_sync-t_upload-t_render;

        // FPS counter + pixel info
        frameCount++;
        double now = glfwGetTime();
        if(now-lastFpsTime>=1.0){
        double fps=frameCount/(now-lastFpsTime);
        if(!inputWait)fprintf(stderr,"[fps] %.0f | p:%.1f c:%.1f s:%.1f u:%.1f r:%.1f d:%.1f ms\n",
          fps,t_parse*1e3,t_conduct*1e3,t_sync*1e3,t_upload*1e3,t_render*1e3,t_disp*1e3);
        frameCount=0;lastFpsTime=now;
      }
      // Update window title with pixel info every frame
      CoordI mc;scr2mem(msX,msY,mc.x,mc.y,W,H);
      char title[256];
      if(cv[mc.x][mc.y]==0)
        snprintf(title,sizeof(title),"(%d,%d)=empty | fps:%.0f",mc.x,mc.y,frameCount/(glfwGetTime()-lastFpsTime+0.001));
      else{
        double re=fi[mc.x][mc.y].real(),im=fi[mc.x][mc.y].imag();
        snprintf(title,sizeof(title),"(%d,%d)=%.3f%+.3fi",mc.x,mc.y,re,im);
      }
      glfwSetWindowTitle(g_window,title);
    }

    glfwTerminate();
    return 0;
}
