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
#include <array>
#include <chrono>
#include <cstdio>
#include <numeric>
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
#include "app/bench_scene.h"
#include "app/input.h"
#include "app/light_settings_io.h"
#include "app/panels.h"
#include "core/image.h"
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
    int benchLayers = 0;          // > 0 = 启用 --bench-scene，值为图层数
bool benchLayersGiven = false; // 用户是否显式给了层数（未给则用场景推荐值）
bool sceneGiven = false;       // 是否显式给了 --scene（决定要不要套用场景推荐值）
std::string projectToOpen;     // --project FILE：启动时打开这个工程文件
std::string projectToSave;     // --save-project FILE：退出前存到这个路径
bool openProjectDialogAtStart = false;   // --project-dialog：启动即拉起工程对话框
double zoomArg = 0.0;                    // --zoom F：覆盖自动适配的缩放（0 = 自动）
// --bench-scene 用的场景（见 app/bench_scene.h）。默认 neon-city：
// 它同时是最有诊断价值的（1px 霓虹管 = 锐度探针）和最好看的。
bench::SceneId benchScene = bench::SceneId::NeonCity;
    bool sizeGiven = false;       // 用户显式给了 --size，基准就不覆盖尺寸
    int radiusArg = -1;           // --radius N：方向表半径（-1 = 未给，用场景推荐/默认 128）
// 方向扩散（--diff / --diffw F / --diffk N）。给一次就锁定，防止预设档覆盖。
bool sweepDiffOn = false;
bool sweepDiffGiven = false;
float sweepDiffW = 0.1f;
int sweepDiffK = 8;
    bool hideUi = false;          // --hide-ui：隐藏面板，只测画布与光照
    bool forceVisible = false, forceHidden = false;
    int framesToRun = 0;
    int paletteIndex = -1;
    // --field：光场照亮下层的强度。默认与 App::fieldStrength 一致（1.0，开着）；
    // 0 是合成着色器里"关掉光场模型"的哨兵值，也就是旧的加法混合路径，
    // 所以不要在不开参数时踩到它。
    float startField = 1.0f;
    // 渐进哈希采样的启动参数（原来只能在面板里点，自动化截图/回归时没法复现）
    bool startDenoise = false;
    float startDenoiseStrength = 3.5f;
    bool startProgressive = false;
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
                sizeGiven = true;
            }
            i++;
        } else if (arg == "--bench-scene") {
            // 可选带图层数：--bench-scene 或 --bench-scene 8
            benchLayers = 5;
            benchLayersGiven = false;
            if (i + 1 < argCount && args[(size_t)i + 1][0] != '-') {
                benchLayers = std::atoi(args[(size_t)++i].c_str());
                benchLayersGiven = true;
            }
            if (benchLayers <= 0) { benchLayers = 5; benchLayersGiven = false; }
            // 基准默认 384x216。必须在**光照初始化之前**定下来：
            // 光照的 GPU 纹理尺寸在 sweep.init(cw,ch) 时就固定了，
            // 之后再改文档尺寸就会左右不一致（已踩过）。
            if (!sizeGiven) { cw = 384; ch = 216; }
        } else if (arg == "--scene") {
            // 选 --bench-scene 用哪套场景（名字见 app/bench_scene.h 的 kScenes）
            if (i + 1 < argCount) {
                if (!bench::sceneByName(args[(size_t)++i], benchScene)) {
                    std::fprintf(stderr, "未知场景；可用：");
                    for (int k = 0; k < (int)bench::SceneId::Count; k++)
                        std::fprintf(stderr, "%s%s", k ? ", " : "", bench::kScenes[k].name);
                    std::fprintf(stderr, "\n");
                    return 1;
                }
            }
            // 场景的推荐值**不在这里套用** —— 参数顺序不定，命令行可能在
            // --scene 之后才给 --radius/--diffw，那时"是否显式给过"还没置位。
            // 统一挪到解析循环结束之后（见 sceneGiven 处理）。
            sceneGiven = true;
        } else if (arg == "--radius" && i + 1 < argCount) {
            radiusArg = std::atoi(args[(size_t)++i].c_str());
        } else if (arg == "--diff") {
            sweepDiffOn = true; sweepDiffGiven = true;
        } else if (arg == "--diffw" && i + 1 < argCount) {
            sweepDiffW = (float)std::atof(args[(size_t)++i].c_str());
            sweepDiffOn = true; sweepDiffGiven = true;
        } else if (arg == "--diffk" && i + 1 < argCount) {
            sweepDiffK = std::max(1, std::atoi(args[(size_t)++i].c_str()));
            sweepDiffOn = true; sweepDiffGiven = true;
        } else if (arg == "--zoom" && i + 1 < argCount) {
            zoomArg = std::atof(args[(size_t)++i].c_str());
        } else if (arg == "--project-dialog") {
            // 自动化用：启动时就像点了菜单一样拉起工程对话框，
            // 配合 --frames --out 可以截图验证"对话框真的画出来了"。
            openProjectDialogAtStart = true;
        } else if (arg == "--save-project" && i + 1 < argCount) {
            // 退出前把当前状态（含 --project 载入的）存到这个路径。
            // 给自动化验证用：没有它就没法在无人值守下测"存了再读"。
            projectToSave = args[(size_t)++i];
        } else if (arg == "--project" && i + 1 < argCount) {
            // 启动时直接打开一个工程文件（双击 .pbproj / 脚本调用都走这条）
            projectToOpen = args[(size_t)++i];
        } else if (arg == "--hide-ui") {
            hideUi = true;
        } else if (arg == "--demo") {
            demo = true;

        } else if (arg == "--frames" && i + 1 < argCount) {
            framesToRun = std::atoi(args[(size_t)++i].c_str());
        } else if (arg == "--out" && i + 1 < argCount) {
            outFile = args[(size_t)++i];
        } else if (arg == "--palette" && i + 1 < argCount) {
            paletteIndex = std::atoi(args[(size_t)++i].c_str());
        } else if (arg == "--field" && i + 1 < argCount) {
            startField = (float)std::atof(args[(size_t)++i].c_str());
        } else if (arg == "--mode" && i + 1 < argCount) {
            // 只剩扫描 GI（路径追踪已于 2026-10-19 移除）。
            // 未知模式明确报错，不再静默回落到 sweep。
            const std::string& m = args[(size_t)++i];
            if (m != "sweep") {
                std::fprintf(stderr, "未知光照模式 '%s'（只有 sweep）\n", m.c_str());
                return 1;
            }
        } else if (arg == "--progressive") {
            startProgressive = true;
        } else if (arg == "--denoise") {
            startDenoise = true;
            // 可选强度：下一个参数不是选项才当强度吃
            if (i + 1 < argCount && !args[(size_t)i + 1].empty() &&
                args[(size_t)i + 1][0] != '-')
                startDenoiseStrength = (float)std::atof(args[(size_t)++i].c_str());
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
                "  --mode sweep       光照模式（只剩扫描 GI；路径追踪已移除）\n"
                "  --progressive      扫描 GI 的渐进哈希采样（每帧每像素一个方向）\n"
                "  --denoise [强度]   渐进模式去噪（à-trous 联合双边，默认强度 3.5）\n"
                "  --demo             用工具管线画一张演示图（含两个图层）\n"
                "  --bench-scene [N]  实时性能基准：N 层逐帧动画（默认 5），384x216\n"
                "                     每帧改动所有图层，量的是最坏情况端到端帧率\n"
                "  --hide-ui          配合 --bench-scene：隐藏面板，只测画布\n"
                "  --radius N         方向表半径（默认 128 = 728 个方向；越小越快、光越硬）\n"
                "  --scene NAME       基准场景：neon-city / lantern-fog / stained-glass /\n"
                "                     chromatic / grid-lab / legacy / lowend\n"
                "  --diff / --diffw F / --diffk N   方向扩散（抹掉放射状辐条）\n"
                "                     低配档示例：--bench-scene --scene lowend\n"
                "                     （单层 + R=32 + 扩散 w=0.1/K=8，集显可玩）\n"
                "  --project FILE     启动时打开工程文件（.pbproj）\n"
                "  --zoom F           覆盖自动适配的缩放（验证画布外背景时用）\n"
                "  --frames N         渲染 N 帧后截图退出（配合 --out）\n"

                "  --out FILE.png     截图输出路径\n"
                "  --open NAME        强制展开右侧栏分区（名字子串，如「图层」）\n"
                "  --close NAME       强制折叠右侧栏分区\n"
                "  --hidden/--visible 强制窗口是否可见\n"
                "  无 --frames 时进入正常交互模式。\n");
            return 0;
        }
    }

    // ------------------------------------------------------------------
    // 参数解析**之后**才套用场景推荐值 / 预设档
    // ------------------------------------------------------------------
    //
    // 为什么必须放在这里：命令行参数顺序不定。`--scene lowend --radius 64`
    // 与 `--radius 64 --scene lowend` 必须得到同一结果，所以"用户是否显式
    // 给过某个参数"要等整条命令行看完才知道。
    // 规则：**显式参数永远优先**，没给过的才用场景推荐值。
    if (sceneGiven || benchLayers > 0) {
        const bench::SceneDesc& sd = bench::kScenes[(int)benchScene];
        if (!benchLayersGiven) benchLayers = sd.layers;
        if (radiusArg < 0) radiusArg = sd.radius;
        if (!sweepDiffGiven) {
            // 预设档（lowend）连扩散一起定；普通场景有自己的 recW
            const bool useDiff = sd.preset ? true : (sd.recW > 0.0f);
            sweepDiffOn = useDiff;
            if (useDiff) { sweepDiffW = sd.recW; sweepDiffK = sd.recK; }
        }
        if (benchLayers > 0) {
            printf("[bench] 档位 %s：%d 层  R=%d  扩散 %s\n", sd.name, benchLayers,
                   (radiusArg > 0) ? radiusArg : 128,
                   sweepDiffOn ? "开" : "关");
        }
    }

    const bool automated = (framesToRun > 0) || demo;
    bool visible = !automated;
    if (forceVisible) visible = true;
    if (forceHidden) visible = false;

    app::App a;
    a.doc.resize(cw, ch);
    // 普通启动（非演示）时底层铺纯白：它相当于整幅画的"底纸"。
    // 光场是"照亮下层"的，最下面那层不能是空的，否则光没东西可打。
    if (!demo) {
        a.doc.layers()[0].image().fill(RGBA8{255, 255, 255, 255});
        a.doc.layers()[0].markAllDirty();
    }
    a.history.reset();
    a.opts.color = a.palette.current();
    a.demo = demo;
    a.fieldStrength = startField;
    a.progressive = startProgressive;
    a.denoise = startDenoise;
    a.denoiseStrength = startDenoiseStrength;

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
    // 方向表半径默认 128（728 个方向）。可用 --radius 覆盖 —— 这是
    // "画质 vs 性能"最有效的单一旋钮，实机测试不同档位时必用。
    sweep.settings().rayRadius = (radiusArg > 0) ? radiusArg : 128;
    sweep.settings().ambient[0] = 0.08f;
    sweep.settings().ambient[1] = 0.08f;
    sweep.settings().ambient[2] = 0.10f;
    // 方向扩散（--diff / --diffw / --diffk，或被预设档如 lowend 设置）
    sweep.settings().diffusionOn = sweepDiffOn;
    sweep.settings().diffusion = sweepDiffW;
    sweep.settings().diffusionStep = sweepDiffK;
    // 从 paintboard_light.ini 恢复用户上次调好的光照参数。
    // ⚠️ 只在**没给命令行参数**时恢复，否则会出现"我明明传了 --radius 32
    // 结果还是 128"这种极难查的问题（命令行意图永远优先）。
    if (!sceneGiven && radiusArg < 0 && !sweepDiffGiven) {
        app::loadLightSettings(sweep.settings());
    }
    a.sweep = &sweep;

    // --project FILE：启动时打开工程。
    // 放在这里是因为它要写 sweep.settings()，而 sweep 刚刚初始化好。
    // ⚠️ 必须在光照纹理分配之后 —— 工程文件可能带来不同的画布尺寸，
    // 但 sweep.init(cw,ch) 已经把纹理尺寸定死了，所以尺寸不符时
    // loadProject 会明确拒绝（见 core/project.cpp 的尺寸校验），
    // 而不是留下"文档和 GPU 纹理不一致"这种更糟的状态。
    if (!projectToOpen.empty()) {
        a.projectPath = projectToOpen;
        actLoadProject(a, projectToOpen);
    }
    if (openProjectDialogAtStart) a.requestProjectSaveDialog = true;

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
    // 基准场景：图层在这里建好（光照的 GPU 侧图层数在 initLighting 时确定，
    // 所以必须在它之前）。尺寸已在参数解析阶段定好，这里不再改。
    if (benchLayers > 0) {
        bench::buildDoc(a.doc, benchScene, benchLayers);
        a.layerPanelDirty = true;
    }
    a.hidePanels = hideUi;

    printf("=== paintboard ===\n");
    printf("画布 %dx%d，界面 Dear ImGui %s\n", cw, ch, IMGUI_VERSION);

    double lastTime = glfwGetTime();
    int frames = 0;
    int frameIndex = 0;

    while (!ctx.shouldClose() && !a.requestQuit) {
        ctx.pollEvents();

        // --- 1.5 基准场景动画 ---
        // 必须在光照之前：step() 会把**所有**图层的 revision 顶上去，
        // 于是 lighting 里的脏层判断全部命中，每帧重算全部图层。
        // 这正是基准要的最坏情况（也是它和"静止看画"帧率差很多的原因）。
        if (benchLayers > 0) {
            bench::stepDoc(a.doc, benchScene, a.benchFrame);
            a.benchFrame++;
        }

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
            // --zoom F：压过自动适配。用来把画布缩到比面板小，
            // 好看到"画布以外"那块背景（自动化截图验证用）。
            if (zoomArg > 0.0) {
                gfx::Camera& c = vp.camera();
                c.zoom = std::clamp(zoomArg, 0.02, 64.0);
                c.x = 0.5;
                c.y = 0.5;
            }
            a.cameraInitialized = true;
        }

        // --- 5. 逐层光照 + 合成 ---
        //
        // 只重算**变了的那几层**（syncLightDirty 判定），每层各自跑一遍独立的
        // 平面内光照并留下自己的光场，最后由 GPU 把各层的光场乘到下面各层上。
        // 详见 app/lighting.cpp。
        app::syncLightDirty(a);
        if (a.lightOn && a.canvasVisible) {
            {
                lightTimer.begin();
                const bool didWork = app::runLighting(a);
                lightTimer.end();
                // 注意：这里**不能**在 !didWork 时把 lightMs 清 0。
                // 基准场景里 UI 是在光照之前构建的（面板要先定画布矩形），
                // 所以 HUD 读到的是这一帧稍后才会被写入的值 —— 每帧都清 0
                // 会让读数永远停在 0.00 ms（已实测踩过）。
                // 计时器结果本身也总是滞后一帧，不差这一次。
                (void)didWork;
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
            GLuint shown = 0;
            if (!a.lightOn) {
                // 关灯：把各层原始 albedo 按图层不透明度叠出来（数组纹理的 layer 0
                // 只是最底层，直接显示它会漏掉上面的图层）。
                GLuint layers = sweep.canvasTexture();
                GLuint opacity = sweep.opacityTexture();
                const int n = sweep.layerCount();
                // 原始 albedo：alpha 是"有多不透明"，空气(0)要变成 mask=1（以完整
                // 颜色显示），半透明像素也以**完整颜色**显示、只用 alpha 决定透出
                // 下面多少。所以两个开关是 (false, true) —— 详见 layer_composite.h。
                //
                // 这里**刻意不传材质**：关灯是"看干净的原图"，而自发光属于光照结果。
                // 传了的话发光像素就会在原图上留下一层光晕，取色和导出都会被污染 ——
                // 那正是这条路径本来要避免的事。
                albedoComp.composite(layers, opacity, n, cw, ch, albedoFlat, true);
                glMemoryBarrier(GL_ALL_BARRIER_BITS);
                shown = albedoFlat;
            } else {
                shown = sweep.resultTexture();
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

        // --- 基准场景的实机读数：每 120 帧往控制台打一行 ---
        // 为什么要打控制台：实机测试时用户要**抄下数字**去比对，
        // 而屏幕上的 HUD 截不了那么多次图。控制台一行就能直接复制。
        if (benchLayers > 0 && a.benchFrame > 0 && a.benchFrame % 120 == 0) {
            printf("[bench] 帧 %6d | %6.1f fps | 光照 %6.2f ms | %d 层 | %dx%d | %s | "
                   "半径 %d / %d 方向\n",
                   a.benchFrame, a.fps, (double)a.lightMs, a.doc.layerCount(), a.doc.width(),
                   a.doc.height(), lightModeName(a.lightMode),
                   a.sweep ? a.sweep->settings().rayRadius : 0,
                   a.sweep ? a.sweep->directionCount() : 0);
            fflush(stdout);
        }
    }

    // --save-project：退出前存一份完整工程（自动化验证"存了再读"用）
    if (!projectToSave.empty()) {
        actSaveProject(a, projectToSave);
    }

    // 退出前把光照参数存回去（下次启动直接恢复上次调好的档位）。
    // 存的是交互后的最终状态 —— 面板里改的、快捷键改的都算。
    app::saveLightSettings(sweep.settings());

    glDeleteTextures(1, &albedoFlat);
    albedoComp.shutdown();
    vp.shutdown();
    sweep.shutdown();
    lightTimer.shutdown();
    uiCtx.shutdown();
    ctx.destroy();
    return 0;
}
