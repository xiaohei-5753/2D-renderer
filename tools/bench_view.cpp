// tools/bench_view.cpp — 基准场景的**纯显示**程序
//
// ## 为什么要有它
//
// paintboard 里的 `--bench-scene` 能测，但它长着一身编辑器界面（工具栏、
// 色板、图层列表…），而且读数要低头看控制台。实机测试时那两样都碍事：
// 界面会干扰"直观感受"，控制台在别的设备上可能根本看不到。
//
// 所以这里做一个**只有一个画面 + 一块半透明读数**的窗口：
// 一条命令就能起来，看着它跑，屏幕上直接读帧率。
//
// 场景代码与 paintboard **完全共用**（app/bench_scene.h 的 buildDoc/stepDoc），
// 所以两边负载一致，数字可以互相比。
//
// ## 操作
//   ESC / Q   退出
//   V         切换垂直同步（关掉才能看到真实上限帧率）
//   SPACE     暂停/继续动画
//   H         隐藏/显示读数
//   F         切换全屏
//   [ / ]     方向表半径减/加（画质 vs 性能最有效的旋钮）
//
// 命令行：
//   bench_view [--layers N] [--radius N] [--size WxH] [--fullscreen] [--no-vsync]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/bench_scene.h"
#include "core/console.h"
#include "core/image.h"
#include "core/layer.h"
#include "gfx/gl_common.h"

// ⚠️ GLFW 必须在 GLEW **之后**包含：glew.h 里有
//    `#if defined(GL_H) ... #error gl.h included before glew.h`
//    反过来的话整个编译就崩（已踩过）。
#include <GLFW/glfw3.h>

#include "gfx/gl_context.h"
#include "gfx/gl_timer.h"
#include "gfx/light_sweep.h"
#include "gfx/shader.h"
#include "imgui.h"
#include "ui/ui_context.h"

using namespace pb;

namespace {

int g_w = 384, g_h = 216;
int g_layers = 5;
int g_radius = 128;
bool g_fullscreen = false;
bool g_vsync = true;
int g_frames = 0;              // > 0 时跑完就退出（自动化用；默认一直跑）
std::string g_out;             // 非空则退出前存一张 PNG
// 方向扩散（见 gfx/light_sweep.h）：默认关闭，按键/参数可开
bool g_diffOn = false;
float g_diffW = 0.1f;
int g_diffK = 8;
// 场景选择。默认 neon-city —— 它同时是最好看的和最有诊断价值的
// （1px 霓虹管 = 锐度探针，前景雾让垂直自发光传输可见）。
bench::SceneId g_scene = bench::SceneId::NeonCity;
bool g_sceneGiven = false;   // 用户显式给了 --scene 时，不自动套用推荐参数
bool g_hideHud = false;      // --hide-hud：不画读数面板（自动化对比画布时用）
// 用户是否显式给过这些参数。给过就别用场景推荐值覆盖 —— 否则
// `--scene neon-city --radius 128` 会被静默改回 32，非常难查。
bool g_radiusGiven = false;
bool g_layersGiven = false;
bool g_diffGiven = false;

// 显示用：把**合成好的最终画面**（LightSweep::resultTexture）画到窗口。
//
// 为什么用 resultTexture 而不是逐层叠 litTexture：
//   resultTexture 就是"完整管线跑完的样子"（各层光场累加 + 垂直自发光传输 +
//   按图层不透明度合成），paintboard 里也是显示它。逐层自己叠会让基准程序
//   和真正的应用出现画面差异 —— 那样两边测的就不是同一件事了。
//
// 顶点着色器按目标矩形缩放，从而**保持画布长宽比**（不然 384x216 会被拉成
// 窗口的比例，画面完全变形）。
const char* kBlitVS = R"(#version 430 core
layout(location=0) in vec2 a_pos;   // 全屏三角形
uniform vec4 u_rect;                // xy=左上(NDC), zw=尺寸(NDC)
out vec2 v_uv;
void main(){
  vec2 p = a_pos * 0.5 + 0.5;       // 0..1
  v_uv = p;
  gl_Position = vec4(u_rect.xy + p * u_rect.zw, 0.0, 1.0);
})";

const char* kBlitFS = R"(#version 430 core
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 o_color;
void main(){
  // 画布的 y 轴朝下、GL 的纹理坐标朝上，所以要翻一下
  o_color = vec4(texture(u_tex, vec2(v_uv.x, 1.0 - v_uv.y)).rgb, 1.0);
})";

struct Blit {
    GLuint prog = 0, vao = 0, vbo = 0;
    GLint uTex = -1, uRect = -1;

    bool init() {
        prog = gfx::createGraphicsProgram("bench_blit", kBlitVS, kBlitFS);
        if (!prog) return false;
        uTex = glGetUniformLocation(prog, "u_tex");
        uRect = glGetUniformLocation(prog, "u_rect");
        const float quad[] = {-1, -1, 3, -1, -1, 3};   // 放大三角形覆盖全屏
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        glBindVertexArray(0);
        return true;
    }
    void shutdown() {
        if (vbo) glDeleteBuffers(1, &vbo);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (prog) glDeleteProgram(prog);
        vbo = vao = prog = 0;
    }
    // 把 imgW x imgH 的画面按"等比放大到刚好放进 winW x winH"的方式绘制
    void drawFit(GLuint tex, int imgW, int imgH, int winW, int winH) {
        if (imgW <= 0 || imgH <= 0 || winW <= 0 || winH <= 0) return;
        const double scale = std::min((double)winW / imgW, (double)winH / imgH);
        const double dw = (double)imgW * scale;    // 设备像素
        const double dh = (double)imgH * scale;
        const float sx = (float)(dw / winW);       // 占窗口的比例
        const float sy = (float)(dh / winH);
        // NDC：整个窗口是 [-1,1]，所以比例 -> NDC 尺寸就是 *2
        const float rx = (float)((1.0 - sx) * 1.0);   // 居中留边（NDC 单位 = 1.0 半宽）
        const float ry = (float)((1.0 - sy) * 1.0);
        glUseProgram(prog);
        glUniform1i(uTex, 0);
        glUniform4f(uRect, -sx, -sy, sx * 2.0f, sy * 2.0f);   // (宽高各占 2*sx)
        (void)rx;
        (void)ry;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }
};

// --- 按键边沿检测（GLFW 只给"当前是否按下"，要自己转成"刚按下"）---
struct KeyEdge {
    bool prev[512] = {false};
    bool pressed(GLFWwindow* w, int key) {
        if (key < 0 || key >= 512) return false;
        const bool now = glfwGetKey(w, key) == GLFW_PRESS;
        const bool edge = now && !prev[key];
        prev[key] = now;
        return edge;
    }
};

void usage() {
    printf(
        "bench_view — 渲染器视觉测试集 / 性能基准（画面 + 实时读数）\n"
        "\n"
        "场景（--scene NAME，默认 neon-city；每个场景都是一幅完整画面，\n"
        "      同时针对性地测某一类视觉特征）：\n"
        "  neon-city       1px 霓虹锐度 + 大面积泛光 + 垂直自发光传输\n"
        "  lantern-fog     垂直光路：光要穿过上方多层半透明雾\n"
        "  stained-glass   半透明染色 + 硬阴影 + 彩色光互染\n"
        "  chromatic       自发光与 albedo 解耦（暗表面发绿光）\n"
        "  grid-lab        定量诊断：点/线/网格/方块（静止，供 dir_probe）\n"
        "  legacy          旧的 5 层动画基准（历史数字可复现）\n"
        "  lowend          低配档：单层 + R=32 + 扩散（集显可玩，测帧率）\n"
        "\n"
        "  --layers N      图层数（默认用场景推荐值）\n"
        "  --radius N      方向表半径（默认用场景推荐值 = 32）\n"
        "  --size WxH      画布尺寸（默认 384x216）\n"
        "  --fullscreen    全屏启动\n"
        "  --frames N      跑 N 帧后自动退出（默认一直跑，按键退出）\n"
        "  --out FILE.png  退出前存一张截图（自动化验证用）\n"
        "  --no-vsync      关闭垂直同步（才能看到真实上限帧率）\n"
        "  --hide-hud      不画读数面板（自动化逐像素对比画布时必须加）\n"
        "  --scene NAME    选场景（会自动套用该场景推荐的 radius/diffusion）\n"
        "\n"
        "方向扩散（抹掉离散方向造成的放射状辐条，见 gfx/light_sweep.h）：\n"
        "  --diff          启动时就开启扩散\n"
        "  --diffw F       交换比例 w（默认 0.1）\n"
        "  --diffk N       每 K 步交换一次（默认 8）\n"
        "\n"
        "  运行中：ESC/Q 退出，V 切垂直同步，SPACE 暂停，H 切读数，F 全屏，\n"
        "          [ ] 调半径，D 开关扩散，- = 调 w，, . 调 K\n");
}

bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--help" || a == "-h") { usage(); exit(0); }
        else if (a == "--layers") { const char* v = next(); if (!v) return false; g_layers = std::max(1, atoi(v)); g_layersGiven = true; }
        else if (a == "--radius") { const char* v = next(); if (!v) return false; g_radius = std::max(4, atoi(v)); g_radiusGiven = true; }
        else if (a == "--size") {
            const char* v = next(); if (!v) return false;
            int w = 0, h = 0;
            if (sscanf(v, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                fprintf(stderr, "[bench] --size 格式应为 WxH\n");
                return false;
            }
            g_w = w; g_h = h;
        }
        else if (a == "--frames") { const char* v = next(); if (!v) return false; g_frames = atoi(v); }
        else if (a == "--out") { const char* v = next(); if (!v) return false; g_out = v; }
        else if (a == "--fullscreen") { g_fullscreen = true; }
        else if (a == "--no-vsync") { g_vsync = false; }
        else if (a == "--hide-hud") { g_hideHud = true; }
        else if (a == "--diff") { g_diffOn = true; g_diffGiven = true; }
        else if (a == "--diffw") { const char* v = next(); if (!v) return false; g_diffW = (float)atof(v); g_diffGiven = true; g_diffOn = true; }
        else if (a == "--diffk") { const char* v = next(); if (!v) return false; g_diffK = std::max(1, atoi(v)); g_diffGiven = true; g_diffOn = true; }
        else if (a == "--scene") {
            const char* v = next(); if (!v) return false;
            if (!bench::sceneByName(v, g_scene)) {
                fprintf(stderr, "[bench] 未知场景 '%s'（可用：", v);
                for (int k = 0; k < (int)bench::SceneId::Count; k++)
                    fprintf(stderr, "%s%s", k ? ", " : "", bench::kScenes[k].name);
                fprintf(stderr, "）\n");
                return false;
            }
            g_sceneGiven = true;
        }
        else { fprintf(stderr, "[bench] 未知参数: %s\n", a.c_str()); return false; }
    }
    // ------------------------------------------------------------------
    // 参数解析**结束之后**才套用场景推荐值 / 预设档
    // ------------------------------------------------------------------
    //
    // 为什么不能放在 `--scene` 分支里当场套用：命令行顺序不定。
    // `--scene lowend --radius 128` 与 `--radius 128 --scene lowend`
    // 必须得到同一结果 —— 当场套用会让前者被预设值覆盖掉。
    //
    // 规则：**显式参数永远优先**，只有没给过的才用场景推荐值。
    {
        const bench::SceneDesc& sd = bench::kScenes[(int)g_scene];
        if (!g_layersGiven) g_layers = sd.layers;
        if (!g_radiusGiven) g_radius = sd.radius;
        if (!g_diffGiven) {
            // 预设档（lowend）连扩散一起定；普通场景用各自的 recW
            const bool useDiff = sd.preset ? true : (sd.recW > 0.0f);
            g_diffOn = useDiff;
            if (useDiff) { g_diffW = sd.recW; g_diffK = sd.recK; }
        }
        printf("[bench] 档位 %s：%d 层  R=%d  扩散 %s（w=%.3f K=%d）\n", sd.name, g_layers,
               g_radius, g_diffOn ? "开" : "关", (double)g_diffW, g_diffK);
    }
    return true;
}

// 从默认帧缓冲读回一张图（与 paintboard 里的 captureFramebuffer 同做法）
//
// ⚠️ 两个必须守住的点，否则读回来的图**不确定**（我在这上面栽过）：
//
//  1. 必须在 **glfwSwapBuffers 之前**调用。交换之后 back buffer 的内容是
//     未定义的 —— 拿到的图会掺进别的帧，表现为"同一命令跑两次结果不同"。
//     这类错误很隐蔽：图看起来是对的，只有逐像素对比才暴露。
//  2. 必须先 glFinish()。glReadPixels 虽然会隐式同步，但显式等一次更稳，
//     也让"读到半成品"这种问题彻底没有余地。
bool captureFramebuffer(int fbW, int fbH, Image& out) {
    if (fbW <= 0 || fbH <= 0) return false;
    glFinish();
    out.resize(fbW, fbH);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    // GL 的原点在左下，PNG 在左上 —— 上下翻转
    std::vector<RGBA8> row((size_t)fbW);
    for (int y = 0; y < fbH / 2; y++) {
        RGBA8* a = out.data() + (size_t)y * fbW;
        RGBA8* b = out.data() + (size_t)(fbH - 1 - y) * fbW;
        std::memcpy(row.data(), a, (size_t)fbW * sizeof(RGBA8));
        std::memcpy(a, b, (size_t)fbW * sizeof(RGBA8));
        std::memcpy(b, row.data(), (size_t)fbW * sizeof(RGBA8));
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setupConsoleUtf8();
    if (!parseArgs(argc, argv)) return 2;

    gfx::ContextDesc desc;
    desc.width = 1280;
    desc.height = 720;
    desc.visible = true;
    desc.title = "paintboard — 性能基准";

    gfx::GLContext ctx;
    if (!ctx.create(desc)) {
        fprintf(stderr, "[bench] 无法创建 OpenGL 上下文\n");
        return 1;
    }
    glfwSwapInterval(g_vsync ? 1 : 0);
    printf("[bench] GPU: %s\n", (const char*)glGetString(GL_RENDERER));
    printf("[bench] 画布 %dx%d，%d 层，方向半径 %d\n", g_w, g_h, g_layers, g_radius);

    // --- 文档与场景 ---
    Document doc;
    doc.resize(g_w, g_h);
    // 场景代码与 paintboard 共用同一份（app/bench_scene.cpp），
    // 所以两边的画面与负载必然一致、数字可比。
    bench::buildDoc(doc, g_scene, g_layers);

    // --- 光照管线 ---
    gfx::LightSweep sweep;
    if (!sweep.init(g_w, g_h)) {
        fprintf(stderr, "[bench] 光照管线初始化失败（着色器编译错？见上面 [gl] 报错）\n");
        return 1;
    }
    sweep.settings().rayRadius = g_radius;
    sweep.settings().ambient[0] = 0.08f;
    sweep.settings().ambient[1] = 0.08f;
    sweep.settings().ambient[2] = 0.10f;
    // 方向扩散：参数放在 settings 里，renderLayer 每帧同步（见 light_sweep.h）
    sweep.settings().diffusionOn = g_diffOn;
    sweep.settings().diffusion = g_diffW;
    sweep.settings().diffusionStep = g_diffK;

    // --- ImGui（只用来画读数）---
    ui::UiContext uiCtx;
    ui::UiConfig uiCfg;
    const bool uiOk = uiCtx.init(ctx.window(), uiCfg);
    if (!uiOk) fprintf(stderr, "[bench] ImGui 初始化失败，读数不可用（画面仍会显示）\\n");

    // --- 显示 ---
    Blit blit;
    if (!blit.init()) {
        fprintf(stderr, "[bench] 显示着色器编译失败\n");
        return 1;
    }

    gfx::GpuTimer timer;
    timer.init();

    int frame = 0;
    bool paused = false;
    bool showHud = !g_hideHud;
    double fps = 0.0;
    double lastTime = glfwGetTime();
    float lightMs = 0.0f;
    float frameMs = 0.0f;
    const Rect all{0, 0, g_w - 1, g_h - 1};
    KeyEdge keys;
    const auto t0 = std::chrono::steady_clock::now();

    // 首帧把方向表建起来，好立刻报出方向数
    sweep.setLayerCount(doc.layerCount());
    for (int i = 0; i < doc.layerCount(); i++) {
        sweep.uploadLayer(i, doc.layer(i).albedo, doc.layer(i).material.data(), all);
        sweep.uploadLayerOpacity(i, 1.0f);
        sweep.renderLayer(i);
    }
    sweep.composite();
    printf("[bench] 方向数 %d（半径 %d）\n", sweep.directionCount(),
           sweep.settings().rayRadius);

    while (!ctx.shouldClose()) {
        ctx.pollEvents();
        const auto fStart = std::chrono::steady_clock::now();
        const bool vsync = g_vsync;

        // ---------- 动画（每帧改动所有图层 = 最坏情况）----------
        // diffscene 是静止场景：不动画，只让光照每帧重算（仍然是全层重算，
        // 所以性能开销与动画场景同量级，但画面稳定、辐条看得清）。
        if (!paused) {
            bench::stepDoc(doc, g_scene, frame);
            frame++;
        }

        // ---------- 光照：每层每帧都重算 ----------
        const int n = std::min(doc.layerCount(), gfx::LightSweep::kMaxLayers);
        // 把按键改过的扩散参数同步回 settings（renderLayer 每帧会读它）
        gfx::LightSettings& ls = sweep.settings();
        ls.diffusionOn = g_diffOn;
        ls.diffusion = g_diffW;
        ls.diffusionStep = g_diffK;
        timer.begin();
        sweep.setLayerCount(n);
        for (int i = 0; i < n; i++) {
            const Layer& L = doc.layer(i);
            sweep.uploadLayerOpacity(i, L.visible ? (float)L.opacity / 255.0f : 0.0f);
            sweep.uploadLayer(i, L.albedo, L.material.data(), all);
            sweep.renderLayer(i);
        }
        sweep.setFieldStrength(1.0f);
        sweep.composite();
        timer.end();
        {
            float ms = 0.0f;
            if (timer.poll(ms)) lightMs = ms;
        }

        // ---------- 画到窗口 ----------
        int fbW = 0, fbH = 0;
        ctx.framebufferSize(fbW, fbH);
        glViewport(0, 0, fbW, fbH);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // 画**合成好的最终画面**，等比放大到刚好放进窗口（像素风格用 NEAREST，
        // 放大后是硬边方块，不会糊）。
        blit.drawFit(sweep.resultTexture(), g_w, g_h, fbW, fbH);

        // ---------- 读数叠加层 ----------
        if (uiOk) {
            uiCtx.beginFrame();
            if (showHud) {
                const ImGuiViewport* vp = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 14.0f, vp->WorkPos.y + 14.0f));
                ImGui::SetNextWindowBgAlpha(0.74f);
                ImGui::Begin("##benchhud", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoMove);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 1.0f, 0.60f, 1.0f));
                ImGui::Text("场景  %s", bench::sceneName(g_scene));
                ImGui::TextDisabled("%s", bench::kScenes[(int)g_scene].purpose);
                ImGui::Text("画布  %d 层   %dx%d", n, g_w, g_h);
                ImGui::PopStyleColor();
                ImGui::Separator();
                // 大号帧率：实机测试时一眼可读
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.30f, 1.0f));
                ImGui::SetWindowFontScale(2.6f);
                ImGui::Text("%.0f fps", fps);
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleColor();
                ImGui::Separator();
                ImGui::Text("光照      %6.2f ms", (double)lightMs);
                ImGui::Text("帧耗时    %6.2f ms", (double)frameMs);
                ImGui::Text("方向      %d 个（半径 %d）", sweep.directionCount(),
                            sweep.settings().rayRadius);
                // 方向扩散状态：这是"抹掉放射状辐条"的开关，测试时要一眼看到
                if (g_diffOn) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 1.0f, 0.75f, 1.0f));
                    ImGui::Text("扩散      开  w=%.3f  K=%d", (double)g_diffW, g_diffK);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("扩散      关");
                }
                ImGui::Text("垂直同步  %s", vsync ? "开" : "关");
                ImGui::Text("动画      %s（帧 %d）", paused ? "暂停" : "运行", frame);
                ImGui::Separator();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.64f, 0.70f, 1.0f));
                ImGui::Text("V 垂直同步   SPACE 暂停   D 扩散开关");
                ImGui::Text("[ ] 方向半径  - = 调 w   , . 调 K");
                ImGui::Text("H 隐藏读数   F 全屏      ESC 退出");
                ImGui::PopStyleColor();
                ImGui::End();
            }
            uiCtx.render();
        }
        // 自动化：跑够帧数 -> 在 **swap 之前** 读回 back buffer（此时内容才有效）
        if (g_frames > 0 && frame >= g_frames) {
            if (!g_out.empty()) {
                Image shot;
                if (captureFramebuffer(fbW, fbH, shot) && writePNG(g_out, shot))
                    printf("[bench] 截图 %s (%dx%d)\n", g_out.c_str(), fbW, fbH);
            }
            ctx.requestClose();
        }
        ctx.swapBuffers();

        // ---------- 计时 ----------
        const auto fEnd = std::chrono::steady_clock::now();
        frameMs = (float)std::chrono::duration<double, std::milli>(fEnd - fStart).count();
        const double now = glfwGetTime();
        const double dt = now - lastTime;
        lastTime = now;
        if (dt > 1e-6) {
            const double inst = 1.0 / dt;
            fps = (fps > 1.0) ? (fps * 0.9 + inst * 0.1) : inst;
        }

        // ---------- 按键 ----------
        GLFWwindow* w = ctx.window();
        if (keys.pressed(w, GLFW_KEY_ESCAPE) || keys.pressed(w, GLFW_KEY_Q)) ctx.requestClose();
        if (keys.pressed(w, GLFW_KEY_V)) {
            g_vsync = !g_vsync;
            glfwSwapInterval(g_vsync ? 1 : 0);
            printf("[bench] 垂直同步 -> %s\n", g_vsync ? "开" : "关");
        }
        if (keys.pressed(w, GLFW_KEY_SPACE)) paused = !paused;
        if (keys.pressed(w, GLFW_KEY_H)) showHud = !showHud;
        // --- 方向扩散：本体开关 + 两个参数 ---
        if (keys.pressed(w, GLFW_KEY_D)) {
            g_diffOn = !g_diffOn;
            printf("[bench] 方向扩散 -> %s（w=%.3f K=%d）\n", g_diffOn ? "开" : "关",
                   (double)g_diffW, g_diffK);
        }
        if (keys.pressed(w, GLFW_KEY_MINUS) || keys.pressed(w, GLFW_KEY_EQUAL)) {
            const float d = keys.pressed(w, GLFW_KEY_MINUS) ? -0.01f : 0.01f;
            g_diffW = std::max(0.005f, std::min(0.5f, g_diffW + d));
            printf("[bench] 扩散 w -> %.3f\n", (double)g_diffW);
        }
        if (keys.pressed(w, GLFW_KEY_COMMA) || keys.pressed(w, GLFW_KEY_PERIOD)) {
            const int d = keys.pressed(w, GLFW_KEY_COMMA) ? -1 : 1;
            g_diffK = std::max(1, std::min(64, g_diffK + d));
            printf("[bench] 扩散 K -> %d\n", g_diffK);
        }
        if (keys.pressed(w, GLFW_KEY_F)) {
            g_fullscreen = !g_fullscreen;
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = mon ? glfwGetVideoMode(mon) : nullptr;
            if (g_fullscreen && mode)
                glfwSetWindowMonitor(w, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
            else
                glfwSetWindowMonitor(w, nullptr, 80, 80, 1280, 720, 0);
        }
        const bool leftKey = keys.pressed(w, GLFW_KEY_LEFT_BRACKET);
        const bool rightKey = keys.pressed(w, GLFW_KEY_RIGHT_BRACKET);
        if (leftKey || rightKey) {
            const int step = leftKey ? -16 : 16;
            const int r = std::max(4, std::min(512, sweep.settings().rayRadius + step));
            if (r != sweep.settings().rayRadius) {
                sweep.settings().rayRadius = r;
                printf("[bench] 方向半径 -> %d\n", r);
            }
        }

        // 注：这里**不再**有第二个 swapBuffers —— 帧末的那次已经在上面
        // （截图必须在它之前完成，见 captureFramebuffer 的注释）。
    }

    blit.shutdown();
    timer.shutdown();
    sweep.shutdown();
    uiCtx.shutdown();
    ctx.destroy();
    printf("[bench] 退出（共 %d 帧）\n", frame);
    return 0;
}
