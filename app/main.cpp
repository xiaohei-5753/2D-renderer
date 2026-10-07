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
#include "app/voxel_build.h"
#include "render/voxel_renderer.h"
#include "core/composite.h"
#include "gfx/layer_composite.h"
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

// 把命令行参数重新按 UTF-8 取一遍。
//
// ⚠️ 为什么不能直接用 main 的 argv：在中文 Windows 上，CRT 是按 **ANSI 代码页**
// （CP936/GBK）解码命令行参数的，而本程序全程按 UTF-8 处理文本。于是
// `--open 图层` 拿到的是一串 GBK 字节，和源码里的 UTF-8 字面量比较永远不相等 ——
// 表现就是"这个选项完全没反应"，而且不会有任何报错。我自己就踩了这个坑。
//
// 所以这里绕开 CRT，直接用 GetCommandLineW 拿原始的 UTF-16 命令行，按 Windows
// 自己的分词规则拆开（双引号成组、反斜杠转义），再转成 UTF-8。
// 顺带把 `--out 中文名.png` 这类路径也修好了。
//
// 注：不用 CommandLineToArgvW 是因为它声明在 <shellapi.h>，而该头文件在
// WIN32_LEAN_AND_MEAN 下不完整（而且还要多链接一个 shell32）。自己拆更省事。
bool isWindowsSpace(wchar_t c) { return c == L' ' || c == L'\t'; }

std::vector<std::string> utf8Args() {
    std::vector<std::string> out;
#ifdef _WIN32
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return out;

    auto append = [&out](const std::wstring& w) {
        if (w.empty()) {
            out.emplace_back();
            return;
        }
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr,
                                            0, nullptr, nullptr);
        std::string s;
        if (len > 0) {
            s.resize((size_t)len);
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len,
                                nullptr, nullptr);
        }
        out.push_back(std::move(s));
    };

    size_t i = 0;
    while (cmd[i] != L'\0') {
        while (cmd[i] != L'\0' && isWindowsSpace(cmd[i])) i++;
        if (cmd[i] == L'\0') break;

        std::wstring arg;
        bool inQuotes = false;
        while (cmd[i] != L'\0') {
            if (!inQuotes && isWindowsSpace(cmd[i])) break;
            if (cmd[i] == L'\\') {
                // 反斜杠要数一串：只有紧跟在双引号前的才转义
                size_t slashes = 0;
                while (cmd[i] == L'\\') { slashes++; i++; }
                if (cmd[i] == L'"') {
                    arg.append(slashes / 2, L'\\');
                    if (slashes % 2 == 1) {
                        arg.push_back(L'"');   // 转义掉的引号，算普通字符
                        i++;
                    } else {
                        inQuotes = !inQuotes;  // 偶数个反斜杠：这个引号是分隔符
                        i++;
                    }
                } else {
                    arg.append(slashes, L'\\');
                }
                continue;
            }
            if (cmd[i] == L'"') {
                inQuotes = !inQuotes;
                i++;
                continue;
            }
            arg.push_back(cmd[i]);
            i++;
        }
        append(arg);
    }
#endif
    return out;
}

// 演示/自动化模式：用工具管线画一张图，渲染若干帧后截图退出。
// 同时充当这个项目的视觉回归工具——不需要人操作就能看出 UI 与渲染是否正常。
//
// 演示图刻意画在**两个图层**上，这样自动化截图就能同时验证：
// 图层合成、逐层光照的层间影响、以及图层面板的缩略图。
void buildDemoScene(app::App& a) {
    const int W = a.doc.width();
    const int H = a.doc.height();

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
        a.history.beginStroke(a.doc);
        s.begin(a.doc.active(), o, x0, y0);
        s.update(a.doc.active(), o, x1, y1);
        s.end();
        a.history.commit(a.doc, toolName(t));
    };

    // 自发光是**独立的 RGB**，与 albedo 无关。
    // 这个辅助函数让自发光取某个颜色的色相（"这个颜色自己发光"）——
    // 这是最常用的组合，但不是唯一可能：也完全可以红表面发蓝光。
    auto glowOf = [](RGBA8 c, float level) {
        Material m;
        const float k = level / 255.0f;
        m.setEmissionFrom((uint8_t)std::lround(c.r * k), (uint8_t)std::lround(c.g * k),
                          (uint8_t)std::lround(c.b * k));
        return m;
    };
    Material none;

    const RGBA8 neonA{90, 200, 255, 110};
    const RGBA8 neonB{255, 130, 80, 110};
    const RGBA8 glassC{220, 190, 90, 160};

    // --- 底层：两条霓虹灯管 + 中间一块实心墙 ---
    stroke(ToolId::Line, W / 8, H / 4, W - W / 8, H / 4, neonA, 3, false, glowOf(neonA, 175));
    stroke(ToolId::Line, W / 8, H - H / 4, W - W / 8, H - H / 4, neonB, 3, false,
           glowOf(neonB, 175));
    stroke(ToolId::Rect, W / 2 - 26, H / 2 - 20, W / 2 + 26, H / 2 + 20,
           RGBA8{44, 44, 54, 255}, 1, true, none);

    // --- 上层：玻璃椭圆 + 一列抖动色块。它们会压住下面的灯管 ---
    a.history.beginAll(a.doc);
    a.doc.addLayer("前景");
    a.history.commit(a.doc, "新建图层");

    stroke(ToolId::Ellipse, W / 8, H / 2 - 30, W / 4, H / 2 + 30, glassC, 1, false,
           glowOf(glassC, 150));
    for (int i = 0; i < 6; i++) {
        o.tool = ToolId::Dither;
        o.color = RGBA8{(uint8_t)(60 + i * 30), 90, (uint8_t)(200 - i * 25), 130};
        o.brushSize = 12;
        o.ditherLevel = 100 - i * 16;
        o.material = glowOf(o.color, 120);
        a.history.beginStroke(a.doc);
        s.begin(a.doc.active(), o, W - W / 6, H / 2 - 30 + i * 10);
        s.end();
        a.history.commit(a.doc, "抖动");
    }

    a.opts = saved;
    a.lightDirty = true;
    a.layerPanelDirty = true;
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

    // 用 UTF-8 的参数表（理由见上面 utf8Args 的注释）。
    // 非 Windows 平台或取不到时回落到 CRT 给的 argv。
    std::vector<std::string> args = utf8Args();
    if (args.empty()) {
        for (int i = 0; i < argc; i++) args.push_back(argv[i] ? argv[i] : "");
    }

    int cw = 320, ch = 180;
    bool demo = false;
    bool forceVisible = false, forceHidden = false;
    int framesToRun = 0;
    int paletteIndex = -1;
    app::LightMode startMode = app::LightMode::Sweep;
    float startDzEl = 45.0f;       // --dz-el：太阳仰角
    float startDzAz = 135.0f;      // --dz-az：太阳方位角
    float startDzSunI = 1.0f;      // --dz-sun：阳光强度
    bool startDzEmit = true;       // --dz-emit：自发光当光源
    float startDzEmitI = 1.0f;     // --dz-ei：自发光强度
    std::string outFile;
    std::string forceOpen, forceClosed;
    std::string clickSection;   // --click-section NAME：自动点这个分区（验证折叠交互）
    int strokeAtFrame = -1;     // --stroke-at N：在第 N 帧往画布上画一笔（验证绘制->光照）
    int clickFrame = 10;   // 等布局稳定（右侧栏宽度会自适应，前几帧的矩形还在变）

    const int argCount = (int)args.size();
    for (int i = 1; i < argCount; i++) {
        const std::string& arg = args[(size_t)i];
        if (arg == "--size" && i + 1 < argCount) {
            int w = 0, h = 0;
            if (std::sscanf(args[(size_t)i + 1].c_str(), "%dx%d", &w, &h) == 2 && w > 0 &&
                h > 0) {
                cw = w;
                ch = h;
            }
            i++;
        } else if (arg == "--demo") {
            demo = true;
        } else if (arg == "--frames" && i + 1 < argCount) {
            framesToRun = std::atoi(args[(size_t)++i].c_str());
        } else if (arg == "--out" && i + 1 < argCount) {
            outFile = args[(size_t)++i];
        } else if (arg == "--palette" && i + 1 < argCount) {
            paletteIndex = std::atoi(args[(size_t)++i].c_str());
        } else if (arg == "--mode" && i + 1 < argCount) {
            const std::string& m = args[(size_t)++i];
            if (m == "pathtrace" || m == "pt") startMode = app::LightMode::PathTrace;
            else if (m == "25d" || m == "dz") startMode = app::LightMode::Trace25D;
            else startMode = app::LightMode::Sweep;
        } else if (arg == "--dz-el" && i + 1 < argCount) {
            startDzEl = (float)std::atof(args[(size_t)++i].c_str());
        } else if (arg == "--dz-az" && i + 1 < argCount) {
            startDzAz = (float)std::atof(args[(size_t)++i].c_str());
        } else if (arg == "--dz-sun" && i + 1 < argCount) {
            startDzSunI = (float)std::atof(args[(size_t)++i].c_str());
        } else if (arg == "--dz-emit" && i + 1 < argCount) {
            startDzEmit = (std::atoi(args[(size_t)++i].c_str()) != 0);
        } else if (arg == "--dz-ei" && i + 1 < argCount) {
            startDzEmitI = (float)std::atof(args[(size_t)++i].c_str());
        } else if (arg == "--visible") {
            forceVisible = true;
        } else if (arg == "--hidden") {
            forceHidden = true;
        } else if (arg == "--stroke-at" && i + 1 < argCount) {
            strokeAtFrame = std::atoi(args[(size_t)++i].c_str());
        } else if (arg == "--click-section" && i + 1 < argCount) {
            clickSection = args[(size_t)++i];
        } else if (arg == "--open" && i + 1 < argCount) {
            forceOpen = args[(size_t)++i];
        } else if (arg == "--close" && i + 1 < argCount) {
            forceClosed = args[(size_t)++i];
        } else if (arg == "--help") {
            printf(
                "用法: paintboard [选项]\n"
                "  --size WxH         画布尺寸（默认 320x180）\n"
                "  --demo             用工具管线画一张演示图（含两个图层）\n"
                "  --frames N         渲染 N 帧后截图退出（配合 --out）\n"
                "  --out FILE.png     截图输出路径\n"
                "  --open NAME        强制展开右侧栏分区（名字子串，如「图层」）\n"
                "  --close NAME       强制折叠右侧栏分区\n"
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
    a.doc.resize(cw, ch);
    a.history.reset();
    a.opts.color = a.palette.current();
    a.demo = demo;
    a.lightMode = startMode;
    a.vzSunEl = startDzEl;
    a.vzSunAz = startDzAz;
    a.vzSunIntensity = startDzSunI;
    a.vzEmitLights = startDzEmit;
    a.vzEmitIntensity = startDzEmitI;

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

    // 自动化截图用：强制右侧栏分区的展开状态
    app::setUiSections(forceOpen, forceClosed);

    gfx::LightSweep sweep;
    if (!sweep.init(cw, ch)) {
        fprintf(stderr, "光照管线初始化失败\n");
        return 1;
    }
    // 扫描 GI 默认环境光压得比较低（它靠方向累积出亮度）
    sweep.settings().rayRadius = 128;
    sweep.settings().ambient[0] = 0.08f;
    sweep.settings().ambient[1] = 0.08f;
    sweep.settings().ambient[2] = 0.10f;
    a.sweep = &sweep;

    gfx::PathTrace pt;
    if (!pt.init(cw, ch)) {
        fprintf(stderr, "路径追踪初始化失败\n");
        return 1;
    }
    // 路径追踪是"收集"式的：空白处的亮度就等于环境光，所以这一项决定背景明暗
    pt.settings().ambient[0] = 0.12f;
    pt.settings().ambient[1] = 0.12f;
    pt.settings().ambient[2] = 0.14f;
    a.pt = &pt;

    // 体素光追：GUI 只通过 render/voxel_renderer.h 这个接口用它。
    // 渲染端内部（加速结构、线程、GPU 资源）GUI 一概不知道。
    auto voxRenderer = render::createVoxelRenderer();
    const GLuint voxTex = gfx::createTexture(GL_RGBA8, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE);
    render::VoxelVolume voxVol;
    std::vector<RGBA8> voxPx;
    bool voxUploaded = false;

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

    // 关灯看原图时用来把各层 albedo 叠起来
    gfx::LayerComposite albedoComp;
    albedoComp.init();
    const GLuint albedoFlat =
        gfx::createTexture(GL_RGBA8, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE);

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
        //
        // 自动点击验证：`--click-section NAME` 会用**真实鼠标事件**点一下那个分区，
        // 再打印它的展开状态。于是"点一下能不能折叠/展开"就成了可自动判定的事实
        // —— 折叠交互靠截图是看不出来的（点过和没点长得一样）。
        //
        // 三个必须注意的时序问题（都踩过，逐一说明）：
        //   1. 位置必须在 beginFrame **之后**注入：GLFW 后端在 NewFrame 里发现鼠标
        //      不在窗口上（自动化跑的是隐藏窗口），会用真实光标位置把它冲掉。
        //   2. 按键事件排到**下一帧**才生效（ImGui 在帧首统一处理事件队列）。
        //   3. 取点必须在**按下前一帧**、并且在 drawUi **之后**取，而且那一刻要
        //      先把鼠标移上去。否则取到的是布局还没稳定时的旧矩形 —— 我因此
        //      点了半天都点不到：坐标落在分区的内容区而不是标题栏上
        //      （表现为 hovered=0、anyHovered=1）。
        //
        // 时间线：clickFrame 悬停取点 -> +1 按下 -> +2 抬起 -> +3 读结果。
        float pointX = 0.0f, pointY = 0.0f;
        bool havePoint = false;
        if (!clickSection.empty() && frameIndex >= clickFrame) {
            havePoint = app::panelSectionPoint(clickSection.c_str(), pointX, pointY);
        }
        if (havePoint && frameIndex <= clickFrame + 2) {
            // 这几帧都把鼠标摁在这个点上，别被后端的真实光标位置带跑
            ImGui::GetIO().AddMousePosEvent(pointX, pointY);
            if (frameIndex == clickFrame + 1) ImGui::GetIO().AddMouseButtonEvent(0, true);
            if (frameIndex == clickFrame + 2) ImGui::GetIO().AddMouseButtonEvent(0, false);
        }

        // 自动化验证：在主循环里真的画一笔（走和鼠标一样的路径）
        if (strokeAtFrame >= 0 && frameIndex == strokeAtFrame) {
            a.opts.color = RGBA8{255, 80, 40, 255};
            a.opts.brushSize = 5;
            a.opts.material.setEmissionFrom(255, 120, 60);
            app::simulateStroke(a, cw / 4, ch / 3, cw * 3 / 4, ch * 2 / 3);
            printf("[sim] 已在第 %d 帧画了一笔\n", frameIndex);
        }

        app::beginPanelHeaderCapture();
        app::drawUi(a);

        // 取点：在 drawUi 之后读这一帧的矩形，得到的是布局稳定后的位置
        if (havePoint && frameIndex == clickFrame) {
            app::panelSectionPoint(clickSection.c_str(), pointX, pointY);
        }
        if (!clickSection.empty() && frameIndex == clickFrame + 3) {
            printf("[ui] 点击「%s」后，展开状态 = %d\n", clickSection.c_str(),
                   app::panelSectionOpen(clickSection.c_str()) ? 1 : 0);
        }
        // 图层面板的缩略图：等 UI 跑完再重建，这样这一帧显示的就是最新内容，
        // 而且不必让面板函数关心 GPU 资源
        app::uploadLayerThumbnails(a);

        // 第一次拿到画布矩形后做一次整数倍缩放并居中（像素画必须整数倍，
        // 否则 NEAREST 采样会让像素列宽不均，表现为竖条纹）。之后不再干预用户。
        if (!a.cameraInitialized && a.canvasVisible) {
            vp.fitIntegerZoom();
            a.cameraInitialized = true;
        }

        // --- 5. 逐层光照 + 合成 ---
        //
        // 这里是模型 B 的落地：只重算**变了的那几层**（syncLightDirty 判定），
        // 每层先算 occlAbove / emitFor 两张通道再照亮，最后由 GPU 把各层叠起来。
        // 详见 app/lighting.cpp。
        app::syncLightDirty(a);
        if (a.lightOn && a.canvasVisible) {
            if (a.lightMode == app::LightMode::Trace25D) {
                // ---- 体素光追（+z 面）----
                //
                // GUI 只做两件事：把图层翻成体素空间、把参数摆好。
                // 光线怎么走、怎么加速，全在渲染端里面。
                const bool geomChanged = !voxUploaded || app::anyLayerDirty(a);
                if (geomChanged) {
                    app::buildVoxelVolume(a.doc, voxVol, a.vzLayerStride);
                    voxRenderer->setVolume(voxVol);   // 内部会重置累积
                    voxUploaded = true;
                    a.vzDirty = true;
                    // 消费掉"脏"状态。syncLightDirty() 每帧按图层签名重算，
                    // 但 lightAllLayers 只有消费方会清（别的模式是 runLighting 清的）。
                    // 不清的话 anyLayerDirty() 永远为真 -> 体素每帧重建 ->
                    // 渐进累积被反复重置，永远收敛不了。
                    a.lightAllLayers = false;
                    for (auto& v : a.lightDirtyLayers) v = 0;
                }
                if (a.vzDirty || geomChanged) {
                    render::RenderSettings rs;
                    // 方位角 / 仰角 -> 指向光源的单位向量（z 向上）
                    const float az = a.vzSunAz * 3.14159265f / 180.0f;
                    const float el = a.vzSunEl * 3.14159265f / 180.0f;
                    const float ce = std::cos(el);
                    rs.sunDir[0] = std::cos(az) * ce;
                    rs.sunDir[1] = std::sin(az) * ce;
                    rs.sunDir[2] = std::sin(el);
                    rs.sunColor[0] = 1.0f; rs.sunColor[1] = 0.96f; rs.sunColor[2] = 0.88f;
                    rs.sunIntensity = a.vzSunIntensity;
                    rs.skyColor[0] = rs.skyColor[1] = rs.skyColor[2] = a.vzSky;
                    rs.skyColor[2] *= 1.25f;                 // 天光偏冷
                    rs.emitScale = a.vzEmitScale;
                    rs.shadows = a.vzShadows;
                    rs.emissiveLights = a.vzEmitLights;
                    rs.emissiveIntensity = a.vzEmitIntensity;
                    rs.lightSamples = a.vzLightSamples;
                    rs.progressive = true;
                    rs.maxAccumFrames = a.vzMaxFrames;
                    voxRenderer->setSettings(rs);   // 参数变了自己会重置累积
                    a.vzDirty = false;
                }
                // 渐进累积：每帧推进一步，UI 不失响应；收敛后停手
                if (!voxRenderer->converged()) {
                    lightTimer.begin();
                    voxRenderer->render(voxPx);
                    lightTimer.end();
                    glBindTexture(GL_TEXTURE_2D, voxTex);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch, GL_RGBA,
                                    GL_UNSIGNED_BYTE, voxPx.data());
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
                a.vzFrames = voxRenderer->accumFrames();
                a.vzMs = (float)voxRenderer->lastMs();
                if (getenv("PB_VOXDUMP") && voxRenderer->converged()) {
                    Image di(cw, ch);
                    for (int i = 0; i < cw * ch; i++) di.data()[i] = voxPx[(size_t)i];
                    writePNG("out/vox_app_out.png", di);
                    fprintf(stderr, "[app] 体素光追输出已写出 out/vox_app_out.png\\n");
                }
            } else {
                // 画布一变，路径追踪的旧累积样本就和新几何对不上了（会拖影），必须重置累积
                if (app::anyLayerDirty(a)) pt.resetAccumulation();
                lightTimer.begin();
                const bool didWork = app::runLighting(a);
                lightTimer.end();
                if (!didWork) a.lightMs = 0.0f;   // 本帧没有光照要做，别显示上一帧的数字
            }
        }
        // 计时器结果要下一帧才可用（不阻塞管线）
        {
            float ms = 0.0f;
            if (lightTimer.poll(ms) && ms > 0.0f) a.lightMs = ms;
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
            const bool ptMode = (a.lightMode == app::LightMode::PathTrace);
            GLuint shown = 0;
            if (!a.lightOn) {
                // 关灯：把各层原始 albedo 按图层不透明度叠出来（数组纹理的 layer 0
                // 只是最底层，直接显示它会漏掉上面的图层）。
                gfx::LightSweep* ls = &sweep;
                gfx::PathTrace* ptx = &pt;
                GLuint layers = ptMode ? ptx->albedoTexture() : ls->canvasTexture();
                GLuint opacity = ptMode ? ptx->opacityTexture() : ls->opacityTexture();
                const int n = ptMode ? ptx->layerCount() : ls->layerCount();
                // 原始 albedo：alpha 是"有多不透明"，空气(0)要变成 mask=1（以完整
                // 颜色显示），半透明像素也以**完整颜色**显示、只用 alpha 决定透出
                // 下面多少。所以两个开关是 (false, true) —— 详见 layer_composite.h。
                albedoComp.composite(layers, opacity, n, cw, ch, albedoFlat, true);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                shown = albedoFlat;
            } else {
                if (a.lightMode == app::LightMode::Trace25D) shown = voxTex;
                else shown = ptMode ? pt.resultTexture() : sweep.resultTexture();
            }
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

    glDeleteTextures(1, &albedoFlat);
    albedoComp.shutdown();
    vp.shutdown();
    sweep.shutdown();
    pt.shutdown();
    glDeleteTextures(1, &voxTex);
    lightTimer.shutdown();
    uiCtx.shutdown();
    ctx.destroy();
    return 0;
}
