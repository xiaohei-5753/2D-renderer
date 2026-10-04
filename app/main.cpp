// app/main.cpp — 像素画板入口
//
// 一帧的顺序（这个顺序是必须的，理由写在下面）：
//   1. glfwPollEvents       —— 事件回调里更新鼠标/笔画状态（用上一帧的悬停结果）
//   2. ui.beginFrame        —— ImGui::NewFrame，此后才能构建 UI
//   3. handleShortcuts/Nav  —— 键盘（有文本焦点时自动让路）
//   4. drawUi               —— 构建所有面板；画布面板顺带算出画布的绘制矩形
//   5. 上传画布 + 跑光照    —— 只上传脏矩形，光照只在需要时重算
//   6. viewport.draw        —— 用裸 GL 把结果画进中央矩形（scissor 限制在矩形内）
//   7. ui.render            —— ImGui 在后，于是面板与叠加层盖在画布之上
//   8. swapBuffers
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

#include "app/app.h"
#include "app/input.h"
#include "app/panels.h"
#include "core/console.h"
#include "core/image.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/gl_timer.h"
#include "gfx/light_sweep.h"
#include "gfx/viewport.h"
#include "ui/ui_context.h"

#include "imgui.h"

#include <GLFW/glfw3.h>

using namespace pb;

namespace {

// 演示/自动化模式：用工具管线画一张图，渲染若干帧后截图退出。
// 同时充当这个项目的视觉回归工具——不需要人操作就能看出 UI 与渲染是否正常。
void buildDemoScene(app::App& a) {
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

    stroke(ToolId::Line, W / 8, H / 4, W - W / 8, H / 4, RGBA8{90, 200, 255, 110}, 3, false,
           glow);
    stroke(ToolId::Line, W / 8, H - H / 4, W - W / 8, H - H / 4, RGBA8{255, 130, 80, 110}, 3,
           false, glow);
    stroke(ToolId::Rect, W / 2 - 26, H / 2 - 20, W / 2 + 26, H / 2 + 20,
           RGBA8{44, 44, 54, 255}, 1, true, none);
    stroke(ToolId::Ellipse, W / 8, H / 2 - 30, W / 4, H / 2 + 30, RGBA8{220, 190, 90, 160}, 1,
           false, glow);
    for (int i = 0; i < 6; i++) {
        o.tool = ToolId::Dither;
        o.color = RGBA8{(uint8_t)(60 + i * 30), 90, (uint8_t)(200 - i * 25), 130};
        o.brushSize = 12;
        o.ditherLevel = 100 - i * 16;
        o.material = glow;
        a.history.beginStroke(a.canvas);
        s.begin(a.canvas, o, W - W / 6, H / 2 - 30 + i * 10);
        s.end();
        a.history.endStroke(a.canvas, "抖动");
    }

    a.opts = saved;
    a.lightDirty = true;
}

// 把默认帧缓冲抓成 PNG（GL 原点在左下，写文件前要翻转）
bool captureFramebuffer(int fbW, int fbH, Image& out) {
    if (fbW <= 0 || fbH <= 0) return false;
    out.resize(fbW, fbH);
    std::vector<uint8_t> raw((size_t)fbW * (size_t)fbH * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    for (int y = 0; y < fbH; y++) {
        const uint8_t* src = raw.data() + (size_t)(fbH - 1 - y) * (size_t)fbW * 4;
        RGBA8* dst = out.data() + (size_t)y * (size_t)fbW;
        for (int x = 0; x < fbW; x++)
            dst[x] = RGBA8{src[x * 4 + 0], src[x * 4 + 1], src[x * 4 + 2], 255};
    }
    return true;
}

void ensureOutDir() {
#ifdef _WIN32
    _mkdir("out");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    setupConsoleUtf8();
#ifdef _WIN32
    SetProcessDPIAware();
#endif

    int cw = 320, ch = 180;
    bool demo = false;
    bool forceVisible = false, forceHidden = false;
    int framesToRun = 0;
    int paletteIndex = -1;
    std::string outFile;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                cw = w;
                ch = h;
            }
            i++;
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            demo = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            framesToRun = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outFile = argv[++i];
        } else if (std::strcmp(argv[i], "--palette") == 0 && i + 1 < argc) {
            paletteIndex = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--visible") == 0) {
            forceVisible = true;
        } else if (std::strcmp(argv[i], "--hidden") == 0) {
            forceHidden = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            printf(
                "用法: paintboard [选项]\n"
                "  --size WxH         画布尺寸（默认 320x180）\n"
                "  --demo             用工具管线画一张演示图\n"
                "  --frames N         渲染 N 帧后截图退出（配合 --out）\n"
                "  --out FILE.png     截图输出路径\n"
                "  --hidden/--visible 强制窗口是否可见\n"
                "  无 --frames 时进入正常交互模式。\n");
            return 0;
        }
    }

    const bool automated = (framesToRun > 0) || demo;
    bool visible = !automated;
    if (forceVisible) visible = true;
    if (forceHidden) visible = false;

    app::App a;
    a.canvas.resize(cw, ch);
    a.canvas.clear(RGBA8{0, 0, 0, 0}, Material{});
    a.history.reset();
    a.opts.color = a.palette.current();
    a.demo = demo;

    // 自动化/回归用：直接切到某一套色板（例如 NES 55 色，验证色块网格的自动换行）
    if (paletteIndex >= 0) {
        a.palette.loadPreset(paletteIndex);
        app::actSetPaletteColor(a, 0);
    }

    gfx::ContextDesc desc;
    desc.width = 1600;
    desc.height = 900;
    desc.visible = visible;
    desc.title = "paintboard";

    gfx::GLContext ctx;
    if (!ctx.create(desc)) {
        fprintf(stderr, "无法创建 OpenGL 上下文\n");
        return 1;
    }
    glfwSwapInterval(1);
    a.ctx = &ctx;

    // ⚠️ 顺序关键：宿主的 GLFW 回调必须先装，ImGui 才能链式调用到它们
    app::installCallbacks(ctx.window(), a);

    ui::UiContext uiCtx;
    ui::UiConfig uiCfg;
    if (!uiCtx.init(ctx.window(), uiCfg)) {
        fprintf(stderr, "ImGui 初始化失败\n");
        return 1;
    }
    // 字体没加载上就得在界面上报出来（双击启动看不到控制台）
    a.fontWarning = uiCtx.fontWarning();

    gfx::LightSweep sweep;
    if (!sweep.init(cw, ch)) {
        fprintf(stderr, "光照管线初始化失败\n");
        return 1;
    }
    sweep.settings().rayRadius = 128;
    sweep.settings().ambient[0] = 0.08f;
    sweep.settings().ambient[1] = 0.08f;
    sweep.settings().ambient[2] = 0.10f;
    a.sweep = &sweep;

    gfx::Viewport vp;
    if (!vp.init()) {
        fprintf(stderr, "视口初始化失败\n");
        return 1;
    }
    vp.setCanvasSize(cw, ch);
    a.vp = &vp;

    // GPU 计时：光照耗时必须用计时器查询测，CPU 侧计时只会得到 0.0 ms
    gfx::GpuTimer lightTimer;
    lightTimer.init();

    // 面板尺寸按 DPI 缩放：ImGui 坐标是物理像素，字体与 style 已经放大过
    a.uiScale = uiCtx.dpiScale();
    a.leftWidth = a.ui(236.0f);
    a.rightWidth = a.ui(322.0f);
    a.statusHeight = a.ui(26.0f);

    if (demo) {
        buildDemoScene(a);
        printf("[演示] 已用工具管线绘制，撤销栈 %zu 步\n", a.history.undoSteps());
    }

    printf("=== paintboard ===\n");
    printf("画布 %dx%d，界面 Dear ImGui %s\n", cw, ch, IMGUI_VERSION);

    double lastTime = glfwGetTime();
    int frames = 0;
    int frameIndex = 0;

    while (!ctx.shouldClose() && !a.requestQuit) {
        ctx.pollEvents();

        // --- 2. ImGui 帧开始 ---
        uiCtx.beginFrame();

        // --- 3. 键盘 ---
        app::handleShortcuts(a);
        app::handleNavigation(a);

        // --- 4. 构建 UI（顺带确定画布矩形）---
        app::drawUi(a);

        // --- 5. 上传画布（只传脏矩形）+ 光照 ---
        const Rect dirty = a.canvas.takeDirty();
        if (!dirty.empty()) {
            sweep.upload(a.canvas.image(), dirty);
            a.lightDirty = true;
        }
        if (a.lightOn && a.lightDirty && a.canvasVisible) {
            lightTimer.begin();
            sweep.render();
            lightTimer.end();
            a.lightDirty = false;
        }
        // 计时器结果要下一帧才可用（不阻塞管线）
        {
            float ms = 0.0f;
            if (lightTimer.poll(ms)) a.lightMs = ms;
        }

        // --- 6. 画布（裸 GL，scissor 限制在中央矩形内）---
        int fbW = 0, fbH = 0;
        ctx.framebufferSize(fbW, fbH);
        int winW = 0, winH = 0;
        ctx.windowSize(winW, winH);
        const double fbScale = (winW > 0) ? (double)fbW / (double)winW : 1.0;
        vp.setFramebuffer(fbScale, fbH);
        // 整块帧缓冲先清成暗色：工作区窗口是透明的，面板之间的空隙由这里兜底
        glViewport(0, 0, fbW, fbH);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.031f, 0.031f, 0.039f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (a.canvasVisible) {
            const GLuint shown = a.lightOn ? sweep.resultTexture() : sweep.canvasTexture();
            vp.draw(shown);
        }

        // --- 7. ImGui 渲染（盖在画布之上）---
        uiCtx.render();

        // --- 截图（必须在 swapBuffers 之前）---
        const bool lastFrame = (framesToRun > 0 && frameIndex == framesToRun - 1);
        if (lastFrame && !outFile.empty()) {
            Image shot;
            if (captureFramebuffer(fbW, fbH, shot)) {
                ensureOutDir();
                if (writePNG(outFile, shot))
                    printf("[截图] %s (%dx%d)\n", outFile.c_str(), fbW, fbH);
            }
        }

        ctx.swapBuffers();

        frameIndex++;
        if (framesToRun > 0 && frameIndex >= framesToRun) ctx.requestClose();

        // --- fps ---
        // 用逐帧指数滑动平均，而不是"每 0.5 秒统计一次"：
        // 后者在短时运行（例如自动化只跑几十帧）下永远显示 0。
        frames++;
        const double now = glfwGetTime();
        const double dt = now - lastTime;
        lastTime = now;
        if (dt > 1e-6) {
            const double inst = 1.0 / dt;
            a.fps = (a.fps > 1.0) ? (a.fps * 0.9 + inst * 0.1) : inst;
        }
        (void)frames;
    }

    vp.shutdown();
    sweep.shutdown();
    lightTimer.shutdown();
    uiCtx.shutdown();
    ctx.destroy();
    return 0;
}
