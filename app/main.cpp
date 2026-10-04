// app/main.cpp — 像素画板（P1：可用的绘图工具集）
//
// P1 阶段还没有 UI 框架（Dear ImGui 在 P3 接入），所以交互靠键盘 + 鼠标，反馈靠三处：
//   1. 画面底部的色板条（鼠标可点选）与光标处的笔刷方框
//   2. 窗口标题栏的实时状态（工具 / 颜色 / 笔刷 / 调色板 / 撤销步数 / fps）
//   3. 控制台日志（切换工具、模式时的确认信息）
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <windows.h>
#endif

#include "core/canvas.h"
#include "core/console.h"
#include "core/history.h"
#include "core/image.h"
#include "core/palette.h"
#include "core/tool.h"
#include "core/types.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/hud.h"
#include "gfx/light_sweep.h"
#include "gfx/viewport.h"

#include <GLFW/glfw3.h>

using namespace pb;

namespace {

// ---------------------------------------------------------------------------
// 应用状态
// ---------------------------------------------------------------------------

struct App {
    // 文档与编辑
    Canvas canvas;
    History history;
    Palette palette;
    ToolOptions opts;
    ToolSession session;

    // 渲染对象（由 main 填充）
    gfx::GLContext* ctx = nullptr;
    gfx::LightSweep* sweep = nullptr;
    gfx::Viewport* vp = nullptr;
    gfx::Hud* hud = nullptr;

    bool lightOn = true;
    bool lightDirty = true;

    // 鼠标与拖拽状态（窗口坐标）
    double mx = 0.0, my = 0.0;
    bool leftDown = false, rightDown = false, middleDown = false;
    bool stroking = false;
    bool panning = false;
    bool spaceDown = false;
    ToolId savedTool = ToolId::Pencil;  // 右键临时擦除时保存原工具

    int saveCounter = 0;
    double fps = 0.0;
};

App* g = nullptr;

// ---------------------------------------------------------------------------
// HUD 布局（色板条）
// ---------------------------------------------------------------------------
//
// 元素尺寸以"窗口坐标"为基准，绘制时乘 DPI 缩放换到帧缓冲像素，
// 这样在 125%/150% 缩放下色板条的视觉大小一致。

struct HudLayout {
    double scale = 1.0;   // 帧缓冲像素 / 窗口像素
    double pad = 6.0;
    double swatch = 22.0;
    double gap = 2.0;
    double barH = 34.0;   // = swatch + 2*pad
};

// 色块尺寸自适应：色板最多可能有 55 色（NES），固定 22px 会超出窗口宽度。
HudLayout computeHudLayout(int winW, int winH, int fbW, int fbH, int paletteCount) {
    HudLayout L;
    if (winW <= 0 || winH <= 0 || fbW <= 0 || fbH <= 0) return L;
    L.scale = (double)fbH / (double)winH;
    if (paletteCount > 0) {
        const double avail = (double)winW - 2 * L.pad - (paletteCount - 1) * L.gap;
        const double fit = avail / (double)paletteCount;
        L.swatch = std::clamp(fit, 6.0, 22.0);
    }
    L.barH = L.swatch + 2 * L.pad;
    return L;
}

// 第 i 个色块在**窗口坐标**中的矩形
void swatchRect(const HudLayout& L, int i, int winH, double& x, double& y, double& w,
                double& h) {
    x = L.pad + i * (L.swatch + L.gap);
    w = L.swatch;
    h = L.swatch;
    y = (double)winH - L.pad - L.swatch;
}

// 鼠标是否落在色板条上；是则返回色块下标，否则 -1
int paletteHitTest(const HudLayout& L, int winH, int paletteCount, double x, double y) {
    if (y < (double)winH - L.barH) return -1;
    for (int i = 0; i < paletteCount; i++) {
        double sx, sy, sw, sh;
        swatchRect(L, i, winH, sx, sy, sw, sh);
        if (x >= sx && x <= sx + sw && y >= sy && y <= sy + sh) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

void refreshTitle(const App& a) {
    if (!a.ctx) return;
    char col[16];
    snprintf(col, sizeof(col), "#%02X%02X%02X", a.opts.color.r, a.opts.color.g,
             a.opts.color.b);

    char buf[640];
    snprintf(buf, sizeof(buf),
             "paintboard P1 | 工具:%s | 色:%s a=%d 自发光=%d | 笔刷:%d%s%s | 色板:%s[%d/%d] "
             "| 撤销:%zu 重做:%zu | 光:%s | %.0f fps | 画布 %dx%d",
             toolName(a.opts.tool), col, a.opts.color.a, a.opts.material.emission,
             a.opts.brushSize, a.opts.brushRound ? "圆" : "方",
             a.opts.shapeFilled ? " 填充" : "", a.palette.presetName(),
             a.palette.currentIndex() + 1, a.palette.size(), a.history.undoSteps(),
             a.history.redoSteps(), a.lightOn ? "开" : "关", a.fps, a.canvas.width(),
             a.canvas.height());
    a.ctx->setTitle(buf);
}

void logTool(const App& a) {
    printf("[工具] %s | 笔刷 %d%s | %s | 颜色 #%02X%02X%02X a=%d 自发光=%d\n",
           toolName(a.opts.tool), a.opts.brushSize, a.opts.brushRound ? "圆" : "方",
           a.opts.shapeFilled ? "填充" : "描边", a.opts.color.r, a.opts.color.g,
           a.opts.color.b, a.opts.color.a, a.opts.material.emission);
}

void logPaletteColor(const App& a) {
    printf("[色板] %s[%d] #%02X%02X%02X\n", a.palette.presetName(),
           a.palette.currentIndex() + 1, a.opts.color.r, a.opts.color.g, a.opts.color.b);
}

void ensureOutDir() {
#ifdef _WIN32
    _mkdir("out");
#endif
}

void saveCanvas(App& a, bool lit) {
    ensureOutDir();
    char path[256];
    snprintf(path, sizeof(path), "out/paintboard_%03d%s.png", a.saveCounter,
             lit ? "_lit" : "");
    a.saveCounter++;

    if (!lit) {
        // 原始画布（保留 alpha = 遮挡程度）：这是"干净的原图"，取色和再编辑都以它为准
        if (writePNG(path, a.canvas.image()))
            printf("[保存] %s （原始画布 %dx%d，含 alpha）\n", path, a.canvas.width(),
                   a.canvas.height());
        return;
    }

    if (!a.sweep) return;
    // 受光结果：需要先确保光照是最新的
    if (a.lightOn) {
        a.sweep->render();
        a.lightDirty = false;
    }
    Image litImg(a.canvas.width(), a.canvas.height());
    if (!gfx::readbackTexture(a.sweep->resultTexture(), a.canvas.width(),
                              a.canvas.height(), litImg.data())) {
        printf("[保存] 回读受光结果失败\n");
        return;
    }
    for (size_t i = 0; i < litImg.pixelCount(); i++) litImg.data()[i].a = 255;
    if (writePNG(path, litImg)) printf("[保存] %s （受光结果）\n", path);
}

// 光标位置 -> 画布坐标
void cursorToCanvas(const App& a, double sx, double sy, int& cx, int& cy) {
    cx = cy = 0;
    if (!a.ctx || !a.vp) return;
    int winW = 0, winH = 0, fbW = 0, fbH = 0;
    a.ctx->windowSize(winW, winH);
    a.ctx->framebufferSize(fbW, fbH);
    a.vp->screenToCanvas(sx, sy, winW, winH, fbW, fbH, cx, cy);
}

// 以光标为锚点缩放：缩放前后光标下的画布像素保持不变
void zoomAtCursor(App& a, double factor) {
    if (!a.ctx || !a.vp) return;
    int winW = 0, winH = 0, fbW = 0, fbH = 0;
    a.ctx->windowSize(winW, winH);
    a.ctx->framebufferSize(fbW, fbH);
    if (winW <= 0 || winH <= 0) return;

    double cx = 0.0, cy = 0.0;
    a.vp->screenToCanvasF(a.mx, a.my, winW, winH, fbW, fbH, cx, cy);

    a.vp->zoomBy(factor);

    float du = 1.0f, dv = 1.0f;
    a.vp->viewUV(fbW, fbH, du, dv);
    const double u = a.mx / (double)winW;
    const double v = 1.0 - a.my / (double)winH;

    // 由 abx = du*(u-0.5) + cam.x 反解 cam，使光标下的画布点保持不动
    a.vp->camera().x = cx / (double)a.vp->canvasWidth() - (double)du * (u - 0.5);
    a.vp->camera().y = cy / (double)a.vp->canvasHeight() - (double)dv * (v - 0.5);
}

// ---------------------------------------------------------------------------
// 笔画
// ---------------------------------------------------------------------------

void beginStrokeAt(App& a, int cx, int cy) {
    if (a.opts.tool == ToolId::Picker) {
        RGBA8 c;
        Material m;
        if (pickPixel(a.canvas, cx, cy, c, m)) {
            a.opts.color = c;
            a.opts.material = m;
            printf("[取色] #%02X%02X%02X a=%d 自发光=%d\n", c.r, c.g, c.b, c.a,
                   m.emission);
        }
        return;
    }
    a.history.beginStroke(a.canvas);
    a.session.begin(a.canvas, a.opts, cx, cy);
    a.stroking = true;
}

void endStroke(App& a) {
    if (!a.stroking) return;
    a.session.end();
    const bool changed = a.history.endStroke(a.canvas, toolName(a.opts.tool));
    a.stroking = false;
    if (changed) a.lightDirty = true;
}

// ---------------------------------------------------------------------------
// HUD 绘制
// ---------------------------------------------------------------------------

void drawHud(const App& a, gfx::Hud& hud, int winW, int winH, int fbW, int fbH) {
    const int n = a.palette.size();
    if (n <= 0) return;
    const HudLayout L = computeHudLayout(winW, winH, fbW, fbH, n);

    const double s = L.scale;

    // 底部条背景（半透明深色，让色块显得清楚）
    const double barTopFb = (double)fbH - L.barH * s;
    hud.fillRect(0.0f, (float)barTopFb, (float)fbW, (float)(L.barH * s),
                 RGBA8{14, 14, 18, 205});

    for (int i = 0; i < n; i++) {
        double wx, wy, ww, wh;
        swatchRect(L, i, winH, wx, wy, ww, wh);
        const double x = wx * s;
        const double y = (double)fbH - ((double)winH - wy) * s;  // 窗口 y(向下) -> fb y
        const double w = ww * s;
        const double h = wh * s;

        RGBA8 c = a.palette.color(i);
        c.a = 255;
        hud.fillRect((float)x, (float)y, (float)w, (float)h, c);

        if (i == a.palette.currentIndex()) {
            hud.rectOutline((float)(x - 2 * s), (float)(y - 2 * s),
                            (float)(w + 4 * s), (float)(h + 4 * s), (float)(2 * s),
                            RGBA8{255, 255, 255, 255});
        } else {
            hud.rectOutline((float)x, (float)y, (float)w, (float)h, (float)s,
                            RGBA8{0, 0, 0, 170});
        }
    }

    // 光标处的笔刷方框
    if (a.vp) {
        int cx = 0, cy = 0;
        cursorToCanvas(a, a.mx, a.my, cx, cy);
        const int size = std::max(1, a.opts.brushSize);
        const int half = (size - 1) / 2;

        double fx = 0.0, fy = 0.0;
        a.vp->canvasToFramebuffer((double)(cx - half), (double)(cy - half), fbW, fbH, fx,
                                  fy);
        const double ppc = a.vp->pixelsPerCell(fbW, fbH);

        RGBA8 outline = a.opts.color;
        outline.a = 230;
        hud.rectOutline((float)fx, (float)fy, (float)(ppc * size), (float)(ppc * size),
                        (float)std::max(1.0, ppc * 0.12), outline);
    }
}

// ---------------------------------------------------------------------------
// 截图与演示场景
// ---------------------------------------------------------------------------

// 从默认帧缓冲抓图。注意 GL 的原点在左下角，写 PNG 前要上下翻转。
bool captureFramebuffer(int fbW, int fbH, Image& out) {
    if (fbW <= 0 || fbH <= 0) return false;
    out.resize(fbW, fbH);
    std::vector<uint8_t> raw((size_t)fbW * (size_t)fbH * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

    for (int y = 0; y < fbH; y++) {
        const uint8_t* src = raw.data() + (size_t)(fbH - 1 - y) * (size_t)fbW * 4;
        RGBA8* dst = out.data() + (size_t)y * (size_t)fbW;
        for (int x = 0; x < fbW; x++) {
            dst[x] = RGBA8{src[x * 4 + 0], src[x * 4 + 1], src[x * 4 + 2], 255};
        }
    }
    return true;
}

// 用工具管线画一张演示图，顺便把直线/矩形/椭圆/抖动/自发光都走一遍。
// `--demo` 模式用它，既能在无人操作时看出渲染是否正确，也能当作视觉回归的固定输入。
void buildDemoScene(App& a) {
    const int W = a.canvas.width();
    const int H = a.canvas.height();

    const ToolOptions saved = a.opts;
    ToolOptions o = a.opts;
    ToolSession s;

    auto stroke = [&](ToolId t, int x0, int y0, int x1, int y1, RGBA8 col, int brush,
                      bool filled, Material mat) {
        o.tool = t;
        o.color = col;
        o.brushSize = brush;
        o.shapeFilled = filled;
        o.material = mat;
        a.history.beginStroke(a.canvas);
        s.begin(a.canvas, o, x0, y0);
        s.update(a.canvas, o, x1, y1);
        s.end();
        a.history.endStroke(a.canvas, toolName(t));
    };

    Material glow;
    glow.emission = 220;
    Material none;

    // 三根霓虹管：半透明 + 自发光，会在周围投出彩色光晕
    stroke(ToolId::Line, W / 8, H / 4, W - W / 8, H / 4, RGBA8{90, 200, 255, 110}, 3,
           false, glow);
    stroke(ToolId::Line, W / 8, H - H / 4, W - W / 8, H - H / 4, RGBA8{255, 130, 80, 110},
           3, false, glow);
    // 实心块（alpha=255：挡光，画面直接显示自身颜色）
    stroke(ToolId::Rect, W / 2 - 26, H / 2 - 20, W / 2 + 26, H / 2 + 20,
           RGBA8{44, 44, 54, 255}, 1, true, none);
    // 椭圆描边
    stroke(ToolId::Ellipse, W / 8, H / 2 - 30, W / 4, H / 2 + 30, RGBA8{220, 190, 90, 160},
           1, false, glow);
    // 抖动渐变：用抖动笔铺一块，验证 Bayer 图案
    for (int i = 0; i < 6; i++) {
        const int level = 100 - i * 16;
        o.tool = ToolId::Dither;
        o.color = RGBA8{(uint8_t)(60 + i * 30), 90, (uint8_t)(200 - i * 25), 130};
        o.brushSize = 12;
        o.ditherLevel = level;
        o.material = glow;
        a.history.beginStroke(a.canvas);
        s.begin(a.canvas, o, W - W / 6, H / 2 - 30 + i * 10);
        s.end();
        a.history.endStroke(a.canvas, "抖动");
    }

    a.opts = saved;
    a.lightDirty = true;
}

// ---------------------------------------------------------------------------
// 输入回调
// ---------------------------------------------------------------------------

void mouseButtonCb(GLFWwindow* w, int button, int action, int mods) {
    (void)mods;
    App& a = *g;
    const bool press = (action == GLFW_PRESS);

    int winW = 0, winH = 0, fbW = 0, fbH = 0;
    glfwGetWindowSize(w, &winW, &winH);
    glfwGetFramebufferSize(w, &fbW, &fbH);

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        a.leftDown = press;
        if (press) {
            glfwGetCursorPos(w, &a.mx, &a.my);

            // 点在色板条上就是选色，不落笔
            const HudLayout L =
                computeHudLayout(winW, winH, fbW, fbH, a.palette.size());
            const int hit = paletteHitTest(L, winH, a.palette.size(), a.mx, a.my);
            if (hit >= 0) {
                a.palette.setCurrent(hit);
                a.opts.color = a.palette.current();
                logPaletteColor(a);
                return;
            }

            if (a.spaceDown) {
                a.panning = true;
            } else {
                int cx = 0, cy = 0;
                cursorToCanvas(a, a.mx, a.my, cx, cy);
                beginStrokeAt(a, cx, cy);
            }
        } else {
            if (a.panning) a.panning = false;
            else endStroke(a);
        }
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        a.rightDown = press;
        if (press) {
            glfwGetCursorPos(w, &a.mx, &a.my);
            // 右键 = 临时擦除（松手后恢复原来的工具）
            a.savedTool = a.opts.tool;
            a.opts.tool = ToolId::Eraser;
            int cx = 0, cy = 0;
            cursorToCanvas(a, a.mx, a.my, cx, cy);
            beginStrokeAt(a, cx, cy);
        } else {
            endStroke(a);
            a.opts.tool = a.savedTool;
        }
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        a.middleDown = press;
        a.panning = press;
        if (press) glfwGetCursorPos(w, &a.mx, &a.my);
    }
}

void cursorPosCb(GLFWwindow* w, double x, double y) {
    App& a = *g;

    if (a.panning && a.vp && a.ctx) {
        int winW = 0, winH = 0, fbW = 0, fbH = 0;
        a.ctx->windowSize(winW, winH);
        a.ctx->framebufferSize(fbW, fbH);
        if (winW > 0 && winH > 0) {
            float du = 1.0f, dv = 1.0f;
            a.vp->viewUV(fbW, fbH, du, dv);
            a.vp->panByUV(-(x - a.mx) / winW * du, (y - a.my) / winH * dv);
        }
    }

    a.mx = x;
    a.my = y;

    if (a.stroking) {
        int cx = 0, cy = 0;
        cursorToCanvas(a, x, y, cx, cy);
        a.session.update(a.canvas, a.opts, cx, cy);
    }
    (void)w;
}

void scrollCb(GLFWwindow* w, double dx, double dy) {
    (void)w;
    (void)dx;
    App& a = *g;
    if (dy > 0.0) zoomAtCursor(a, 0.88);
    else if (dy < 0.0) zoomAtCursor(a, 1.0 / 0.88);
}

void keyCb(GLFWwindow* w, int key, int scancode, int action, int mods) {
    (void)scancode;
    App& a = *g;

    if (action != GLFW_PRESS && action != GLFW_REPEAT) {
        if (key == GLFW_KEY_SPACE) a.spaceDown = false;
        return;
    }

    if (key == GLFW_KEY_SPACE) {
        a.spaceDown = true;
        return;
    }

    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    const bool shift = (mods & GLFW_MOD_SHIFT) != 0;

    if (ctrl) {
        switch (key) {
            case GLFW_KEY_Z:
                if (shift) {
                    if (a.history.redo(a.canvas)) {
                        a.lightDirty = true;
                        printf("[重做] 剩余可撤销 %zu\n", a.history.undoSteps());
                    }
                } else if (a.history.undo(a.canvas)) {
                    a.lightDirty = true;
                    printf("[撤销] 剩余可撤销 %zu\n", a.history.undoSteps());
                }
                return;
            case GLFW_KEY_Y:
                if (a.history.redo(a.canvas)) {
                    a.lightDirty = true;
                    printf("[重做] 剩余可撤销 %zu\n", a.history.undoSteps());
                }
                return;
            case GLFW_KEY_S:
                saveCanvas(a, shift);
                return;
            case GLFW_KEY_N:
                a.history.beginStroke(a.canvas);
                a.canvas.clear(RGBA8{0, 0, 0, 0}, Material{});
                a.history.endStroke(a.canvas, "清空");
                a.lightDirty = true;
                printf("[清空] 画布已清空（可撤销）\n");
                return;
            case GLFW_KEY_0:
                if (a.vp) a.vp->resetCamera();
                return;
            case GLFW_KEY_EQUAL:
                if (a.vp) a.vp->zoomBy(0.9);
                return;
            case GLFW_KEY_MINUS:
                if (a.vp) a.vp->zoomBy(1.0 / 0.9);
                return;
            default:
                return;
        }
    }

    switch (key) {
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(w, GLFW_TRUE);
            return;

        case GLFW_KEY_TAB:
            a.lightOn = !a.lightOn;
            if (a.lightOn) a.lightDirty = true;
            printf("[光照] %s\n", a.lightOn ? "开" : "关（显示原始画布）");
            return;

        case GLFW_KEY_P: a.opts.tool = ToolId::Pencil; logTool(a); return;
        case GLFW_KEY_L: a.opts.tool = ToolId::Line; logTool(a); return;
        case GLFW_KEY_R: a.opts.tool = ToolId::Rect; logTool(a); return;
        case GLFW_KEY_E: a.opts.tool = ToolId::Ellipse; logTool(a); return;
        case GLFW_KEY_F: a.opts.tool = ToolId::Fill; logTool(a); return;
        case GLFW_KEY_D: a.opts.tool = ToolId::Dither; logTool(a); return;
        case GLFW_KEY_K: a.opts.tool = ToolId::Picker; logTool(a); return;
        case GLFW_KEY_X: a.opts.tool = ToolId::Eraser; logTool(a); return;
        case GLFW_KEY_I: a.opts.tool = ToolId::Lighten; logTool(a); return;
        case GLFW_KEY_O: a.opts.tool = ToolId::Darken; logTool(a); return;

        case GLFW_KEY_LEFT_BRACKET:
            a.opts.brushSize = std::max(1, a.opts.brushSize - 1);
            logTool(a);
            return;
        case GLFW_KEY_RIGHT_BRACKET:
            a.opts.brushSize = std::min(64, a.opts.brushSize + 1);
            logTool(a);
            return;
        case GLFW_KEY_B:
            a.opts.brushRound = !a.opts.brushRound;
            logTool(a);
            return;
        case GLFW_KEY_G:
            a.opts.shapeFilled = !a.opts.shapeFilled;
            logTool(a);
            return;

        case GLFW_KEY_COMMA:
            a.palette.loadPreset((a.palette.presetIndex() - 1 + builtinPaletteCount()) %
                                 builtinPaletteCount());
            a.opts.color = a.palette.current();
            printf("[色板] 切换到 %s（%d 色）\n", a.palette.presetName(), a.palette.size());
            return;
        case GLFW_KEY_PERIOD:
            a.palette.loadPreset((a.palette.presetIndex() + 1) % builtinPaletteCount());
            a.opts.color = a.palette.current();
            printf("[色板] 切换到 %s（%d 色）\n", a.palette.presetName(), a.palette.size());
            return;

        default:
            break;
    }

    // 数字键 1..9 选色，0 选第 10 个
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) {
        a.palette.setCurrent(key - GLFW_KEY_1);
        a.opts.color = a.palette.current();
        logPaletteColor(a);
    } else if (key == GLFW_KEY_0) {
        a.palette.setCurrent(9);
        a.opts.color = a.palette.current();
        logPaletteColor(a);
    }
}

}  // namespace

int main(int argc, char** argv) {
    setupConsoleUtf8();
#ifdef _WIN32
    SetProcessDPIAware();
#endif

    int cw = 320, ch = 180;
    bool demo = false;
    bool forceVisible = false;
    bool forceHidden = false;
    int framesToRun = 0;
    std::string outFile;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                cw = w;
                ch = h;
            }
            i++;
        } else if (strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            framesToRun = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outFile = argv[++i];
        } else if (strcmp(argv[i], "--visible") == 0) {
            forceVisible = true;
        } else if (strcmp(argv[i], "--hidden") == 0) {
            forceHidden = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf(
                "用法: paintboard [选项]\n"
                "  --size WxH        画布尺寸（默认 320x180）\n"
                "  --demo            用工具管线画一张演示图\n"
                "  --frames N        渲染 N 帧后截图退出（配合 --out）\n"
                "  --out FILE.png    截图输出路径\n"
                "  --hidden/--visible  强制窗口是否可见\n"
                "  无 --frames 时进入正常交互模式。\n");
            return 0;
        }
    }

    // --demo/--frames 是自动化用途，默认不开可见窗口，免得干扰使用者
    const bool automated = (framesToRun > 0) || demo;
    bool visible = !automated;
    if (forceVisible) visible = true;
    if (forceHidden) visible = false;

    App app;
    g = &app;
    app.canvas.resize(cw, ch);
    app.canvas.clear(RGBA8{0, 0, 0, 0}, Material{});
    app.history.reset();
    app.opts.color = app.palette.current();

    gfx::ContextDesc desc;
    desc.width = 1280;
    desc.height = 720;
    desc.visible = visible;
    desc.title = "paintboard P1";

    gfx::GLContext ctx;
    if (!ctx.create(desc)) {
        fprintf(stderr, "无法创建 OpenGL 上下文\n");
        return 1;
    }
    glfwSwapInterval(1);
    app.ctx = &ctx;

    gfx::LightSweep sweep;
    if (!sweep.init(cw, ch)) {
        fprintf(stderr, "光照管线初始化失败\n");
        return 1;
    }
    sweep.settings().rayRadius = 128;
    sweep.settings().ambient[0] = 0.08f;
    sweep.settings().ambient[1] = 0.08f;
    sweep.settings().ambient[2] = 0.10f;
    app.sweep = &sweep;

    gfx::Viewport vp;
    if (!vp.init()) {
        fprintf(stderr, "视口初始化失败\n");
        return 1;
    }
    vp.setCanvasSize(cw, ch);
    app.vp = &vp;

    gfx::Hud hud;
    if (!hud.init()) {
        fprintf(stderr, "HUD 初始化失败\n");
        return 1;
    }
    app.hud = &hud;

    glfwSetMouseButtonCallback(ctx.window(), mouseButtonCb);
    glfwSetCursorPosCallback(ctx.window(), cursorPosCb);
    glfwSetScrollCallback(ctx.window(), scrollCb);
    glfwSetKeyCallback(ctx.window(), keyCb);

    if (demo) {
        buildDemoScene(app);
        printf("[演示] 已用工具管线绘制演示图，撤销栈 %zu 步\n", app.history.undoSteps());
    }

    printf("=== paintboard P1 ===\n");
    printf("画布 %dx%d\n", cw, ch);
    printf(
        "工具: P铅笔 L直线 R矩形 E椭圆 F油漆桶 D抖动 K取色 X橡皮 I加光 O减光\n"
        "笔刷: [ ] 改大小   B 方圆切换   G 填充/描边\n"
        "色板: 1-9,0 选色   , . 切换色板（也可以直接点画面底部的色条）\n"
        "视图: 滚轮缩放   中键 或 空格+左键 拖拽平移   Ctrl+0 复位\n"
        "其它: Ctrl+Z 撤销   Ctrl+Y 重做   Tab 开关光照   Ctrl+S 存原始图   "
        "Ctrl+Shift+S 存受光图   Ctrl+N 清空   ESC 退出\n");
    refreshTitle(app);

    double lastTime = glfwGetTime();
    int frames = 0;
    int frameIndex = 0;

    while (!ctx.shouldClose()) {
        ctx.pollEvents();

        // --- 上传画布（只传脏矩形）---
        const Rect dirty = app.canvas.takeDirty();
        if (!dirty.empty()) {
            sweep.upload(app.canvas.image(), dirty);
            app.lightDirty = true;
        }

        // --- 光照（只在画布变了、或需要时才重算）---
        if (app.lightOn && app.lightDirty) {
            sweep.render();
            app.lightDirty = false;
        }

        // --- 显示 ---
        int fbW = 0, fbH = 0;
        ctx.framebufferSize(fbW, fbH);
        const GLuint shown = app.lightOn ? sweep.resultTexture() : sweep.canvasTexture();
        vp.draw(shown, fbW, fbH);

        // --- HUD ---
        int winW = 0, winH = 0;
        ctx.windowSize(winW, winH);
        hud.begin(fbW, fbH);
        drawHud(app, hud, winW, winH, fbW, fbH);
        hud.end();

        // 截图必须在 swapBuffers 之前读——交换之后后台缓冲的内容就不确定了
        const bool lastFrame = (framesToRun > 0 && frameIndex == framesToRun - 1);
        if (lastFrame && !outFile.empty()) {
            Image shot;
            if (captureFramebuffer(fbW, fbH, shot)) {
                ensureOutDir();
                if (writePNG(outFile, shot))
                    printf("[截图] %s （%dx%d，含 HUD）\n", outFile.c_str(), fbW, fbH);
            }
        }

        ctx.swapBuffers();

        frameIndex++;
        if (framesToRun > 0 && frameIndex >= framesToRun) ctx.requestClose();

        // --- fps 与标题 ---
        frames++;
        const double now = glfwGetTime();
        if (now - lastTime >= 0.5) {
            app.fps = frames / (now - lastTime);
            frames = 0;
            lastTime = now;
            refreshTitle(app);
        }
    }

    hud.shutdown();
    vp.shutdown();
    sweep.shutdown();
    ctx.destroy();
    g = nullptr;
    return 0;
}
