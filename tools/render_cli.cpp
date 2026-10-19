// tools/render_cli.cpp — 离线渲染器
//
// 不打开可见窗口（GLFW 隐藏窗口 + OpenGL 4.3 离屏），把画布跑一遍光照管线后
// 直接写出 PNG。用途有三：
//   1. 渲染效果的验证回路：不需要人眼看屏幕就能确认光照对不对
//   2. 游戏侧的素材烘焙工具：把画板文档离线渲成成品图
//   3. 性能与回归测试：固定输入 -> 固定输出，可做像素级对比
//
// 用法：
//   paintboard_render --scene neon --size 320x180 --radius 128 --out out.png
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "core/canvas.h"
#include "core/console.h"
#include "core/image.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/light_sweep.h"

using namespace pb;
using namespace pb::tools;

namespace {

struct Options {
    std::string scene = "neon";
    int w = 320, h = 180;
    int radius = 128;
    float emit = 1.0f;
    float ambient[3] = {0.10f, 0.10f, 0.12f};
    std::string out = "out.png";
    std::string dumpCanvas;   // 非空则额外导出未受光的画布原图
    // 只剩扫描 GI。--mode 仍接受，但只有 "sweep" 合法（路径追踪已于 2026-10-19 移除）。
    std::string mode = "sweep";
    bool denoise = false;          // 渐进模式的去噪（à-trous 联合双边）
    float denoiseStrength = 3.5f;  // σr = c·√(Var/N)，实测甜点 c≈3.5
    // 渐进哈希采样（扫描 GI 的可选路径）：每帧每像素采一个方向，跨帧累积
    bool progressive = false;
    int progFrames = 184;          // 默认一个完整周期（R=32 时 D=184）
    // 角度可压缩性实验：只用前 N 个方向渲染，然后按 K/N 放大以便与全量对比。
    // 误差越小 -> 有效角度自由度越少 -> 低阶角度基/降方向数越有希望。
};

void usage() {
    printf(
        "paintboard_render — 离线渲染画布并输出 PNG\n"
        "\n"
        "  --scene <name>   场景: neon | cornell | checker | empty   (默认 neon)\n"
        "  --size <WxH>     画布尺寸                                (默认 320x180)\n"
        "  --radius <N>     扫描方向表半径，越大光越平滑越慢          (默认 128)\n"
        "  --emit <F>       自发光倍率                              (默认 1.0)\n"
        "  --ambient <r,g,b>  环境光 0..1                           (默认 0.10,0.10,0.12)\n"
        "  --out <file.png> 输出文件                                (默认 out.png)\n"
        "  --mode <m>       只有 sweep（路径追踪已移除）            (默认 sweep)\n"
        "  --progressive [N] 渐进哈希采样：跨帧累积 N 帧            (默认 184)\n"
        "  --denoise [强度]  渐进模式开启 à-trous 联合双边去噪（默认强度 3.5）\n"
        "  --dump-canvas <file.png>  额外导出未受光的画布原图\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "[cli] %s 需要一个参数\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            usage();
            exit(0);
        } else if (a == "--scene") {
            const char* v = next("--scene"); if (!v) return false; o.scene = v;
        } else if (a == "--size") {
            const char* v = next("--size"); if (!v) return false;
            if (!parseSize(v, o.w, o.h)) { fprintf(stderr, "[cli] 尺寸格式应为 WxH\n"); return false; }
        } else if (a == "--radius") {
            const char* v = next("--radius"); if (!v) return false; o.radius = atoi(v);
        } else if (a == "--emit") {
            const char* v = next("--emit"); if (!v) return false; o.emit = (float)atof(v);
        } else if (a == "--ambient") {
            const char* v = next("--ambient"); if (!v) return false;
            if (sscanf(v, "%f,%f,%f", &o.ambient[0], &o.ambient[1], &o.ambient[2]) != 3) {
                fprintf(stderr, "[cli] 环境光格式应为 r,g,b\n");
                return false;
            }
        } else if (a == "--out") {
            const char* v = next("--out"); if (!v) return false; o.out = v;
        } else if (a == "--dump-canvas") {
            const char* v = next("--dump-canvas"); if (!v) return false; o.dumpCanvas = v;
        } else if (a == "--mode") {
            const char* v = next("--mode"); if (!v) return false; o.mode = v;
            // 未知模式必须明确报错，不能静默回落到 sweep（那样脚本会以为跑的是 pt）
            if (o.mode != "sweep") {
                fprintf(stderr, "[cli] 未知模式 '%s'（只剩 sweep；路径追踪已移除）\n",
                        o.mode.c_str());
                return false;
            }
        } else if (a == "--denoise") {
            o.denoise = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.denoiseStrength = (float)atof(argv[++i]);
        } else if (a == "--progressive") {
            o.progressive = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o.progFrames = atoi(argv[++i]);
        } else {
            fprintf(stderr, "[cli] 未知参数: %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

RGBA8 rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return RGBA8{r, g, b, a}; }

// 自发光材质。传入强度 0..255。
// 自发光材质：注意自发光是**独立的 RGB**，与 albedo 无关。
// emissive()     给中性（灰白）自发光
// emissiveFrom() 让自发光取某个颜色的色相（"这个颜色自己发光"）
Material emissive(uint8_t e) {
    Material m;
    m.setEmissionFrom(e, e, e);
    return m;
}
Material emissiveFrom(RGBA8 c, uint8_t level) {
    Material m;
    const float k = level / 255.0f;
    m.setEmissionFrom((uint8_t)std::lround(c.r * k), (uint8_t)std::lround(c.g * k),
                      (uint8_t)std::lround(c.b * k));
    return m;
}

// --- 内置示例场景 ---
//
// 光照模型现在有**两个独立的量**：
//   alpha   = 光路上的通透程度（0=空气，255=完全遮挡）
//   emission= 自发光强度（与 alpha 无关）
// 于是可以表达两类原来表达不了的像素：
//   不透明但发光（自发光屏）= a=255 + emission 高
//   半透明但不发光（彩色玻璃）= a 中等 + emission 0
//
// 注意：emission 为 0 的像素**不发光**，场景里必须有材质才会亮起来。

void sceneNeon(Canvas& c) {
    c.clear(RGBA8{0, 0, 0, 0});
    const uint8_t A = 90;
    // 让每根管子发**自己颜色**的光（自发光是独立 RGB，这里显式取该颜色的色相）
    const RGBA8 c1 = rgb(80, 200, 255, A), c2 = rgb(255, 90, 140, A),
                c3 = rgb(255, 200, 90, A);
    // 三根不同颜色的霓虹管
    c.drawLine(40, 30, 120, 30, c1, emissiveFrom(c1, 175));
    c.drawLine(200, 40, 280, 40, c2, emissiveFrom(c2, 175));
    c.drawLine(60, 150, 260, 150, c3, emissiveFrom(c3, 175));
    // 一个实心色块：用来对比"实心像素不受光"
    c.fillRect(140, 70, 180, 110, rgb(40, 40, 48, 255));
    // 一个"不透明但发白光"的像素 —— 表面颜色与发光颜色完全独立
    c.setPixel(159, 55, rgb(255, 255, 255, 255), emissive(255));
    // 一块"半透明但不发光"的彩色玻璃
    c.fillRect(30, 100, 60, 140, rgb(120, 220, 160, 110));
}

void sceneCornell(Canvas& c) {
    c.clear(RGBA8{0, 0, 0, 0});
    const int W = c.width(), H = c.height();
    // 房间：四面实心墙（a=255 且不自发光 -> 挡光，画面显示自身颜色）
    const RGBA8 wall = rgb(70, 70, 76, 255);
    c.fillRect(0, 0, W - 1, 4, wall);
    c.fillRect(0, H - 5, W - 1, H - 1, wall);
    c.fillRect(0, 0, 4, H - 1, wall);
    c.fillRect(W - 5, 0, W - 1, H - 1, wall);
    // 天花板上的灯：半透明 + 强自发光
    c.fillRect(W / 2 - 40, 6, W / 2 + 40, 12, rgb(255, 240, 210, 140), emissive(235));
    // 两个实心盒子
    c.fillRect(70, H - 70, 120, H - 10, rgb(90, 80, 70, 255));
    c.fillRect(180, H - 100, 240, H - 10, rgb(70, 80, 95, 255));
}

void sceneChecker(Canvas& c) {
    c.clear(RGBA8{0, 0, 0, 0});
    const int W = c.width(), H = c.height();
    const Material glow = emissive(110);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const int cell = (x / 8 + y / 8) & 1;
            if (cell) c.setPixelRaw(x, y, rgb(60, 90, 160, 100), glow);
            else c.setPixelRaw(x, y, rgb(160, 70, 60, 100));
        }
    }
    c.markAllDirty();
}

// 诊断"方向性伪影"的最小场景：**纯白实心底 + 上方一个小自发光块**。
//
// 为什么必须是"白底 + 上方发光"这个结构（而不是透明底上放发光点）：
// 受光结果 lit 是**把收集到的光乘到 albedo 上**。底下没有东西可乘，
// 画面就是黑的 —— 我最初用透明底测，什么都看不见，白白折腾了几轮。
// 真实用法里最底下永远有一层实心底纸。
void scenePoint(Canvas& c) {
    const int W = c.width(), H = c.height();
    // 纯白实心底：alpha=255 才是不透明
    c.clear(RGBA8{255, 255, 255, 255});
    Material m;
    m.setEmissionFrom(255, 255, 255);
    // 上方一个小而强的自发光块
    const int cx = W / 2, cy = H / 2;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            c.setPixelRaw(cx + dx, cy + dy, rgb(255, 255, 255, 255), m);
    c.markAllDirty();
}


// 最小对照：纯白不透明底，**完全不自发光**。
// 用来确认"白底能不能正常显示" —— 如果这个都黑，问题就在合成/上传而不是光照。
void sceneWhite(Canvas& c) {
    c.clear(RGBA8{255, 255, 255, 255});
    c.markAllDirty();
}

// 朝向测试场景：**上亮下暗**，一眼就能判出 PNG 有没有上下颠倒。
//   * 顶部：一条满强度自发光的白条
//   * 底部：一条纯黑（不发光的暗条）
// 导出的图里白条必须在上方。若跑到下方 = 导出翻转了。
//
// 为什么需要它：这个错在界面上看不见（显示走另一条路径、自带翻转），
// 只有导出的 PNG 会颠倒，所以必须有一个**朝向可判**的场景来固化验证。
void sceneOrient(Canvas& c) {
    const int W = c.width(), H = c.height();
    c.clear(RGBA8{0, 0, 0, 255});
    for (auto& m : c.materials()) m = Material{};
    for (int y = 0; y < H / 6; y++)                    // 顶部白条（自发光）
        for (int x = 0; x < W; x++) {
            Material em;
            em.setEmissionFrom(255, 255, 255);
            c.image().at(x, y) = RGBA8{255, 255, 255, 255};
            c.materials()[(size_t)y * W + x] = em;
        }
    for (int y = H - H / 6; y < H; y++)                // 底部暗条
        for (int x = 0; x < W; x++)
            c.image().at(x, y) = RGBA8{20, 20, 24, 255};
    c.markAllDirty();
}

// 白底 + 中心自发光点（不出光也该看得见那个点本身）
void sceneWhiteDot(Canvas& c) {
    const int W = c.width(), H = c.height();
    c.clear(RGBA8{255, 255, 255, 255});
    Material m;
    m.setEmissionFrom(255, 255, 255);
    c.fillRect(W / 2 - 3, H / 2 - 3, W / 2 + 3, H / 2 + 3, rgb(255, 255, 255, 255), m);
    c.markAllDirty();
}


// 如果亮度把颜色推向白（旧实现的错误行为），右列会变成白色 —— 这个场景一眼就能看出来。
void sceneHue(Canvas& c) {
    c.clear(RGBA8{0, 0, 0, 0});
    const int W = c.width(), H = c.height();
    const RGBA8 hues[3] = {rgb(220, 40, 40), rgb(40, 200, 60), rgb(50, 90, 230)};
    const uint8_t ems[3] = {0, 120, 255};

    const int barW = W / 5;
    const int barH = H / 5;
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) {
            const int x0 = W / 12 + k * (barW + W / 24);
            const int y0 = H / 8 + r * (barH + H / 12);
            c.fillRect(x0, y0, x0 + barW, y0 + barH, hues[r],
                   emissiveFrom(hues[r], ems[k]));
        }
    }
    // 亮度对照的标签块：无彩色，用来检查自发光强度本身
    for (int k = 0; k < 3; k++) {
        const int x0 = W / 12 + k * (barW + W / 24);
        c.fillRect(x0, H - H / 5, x0 + barW, H - H / 6, rgb(255, 255, 255),
                   emissive(ems[k]));
    }
    c.markAllDirty();
}

bool buildScene(const std::string& name, Canvas& c) {
    if (name == "empty") { c.clear(RGBA8{0, 0, 0, 0}); return true; }
    if (name == "neon") { sceneNeon(c); return true; }
    if (name == "cornell") { sceneCornell(c); return true; }
    if (name == "point") { scenePoint(c); return true; }
    if (name == "white") { sceneWhite(c); return true; }
    if (name == "orient") { sceneOrient(c); return true; }
    if (name == "whitedot") { sceneWhiteDot(c); return true; }
    if (name == "checker") { sceneChecker(c); return true; }
    if (name == "hue") { sceneHue(c); return true; }
    fprintf(stderr, "[cli] 未知场景: %s\n", name.c_str());
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    setupConsoleUtf8();
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;

    // --- 1. 用纯 CPU 逻辑构建画布（core 层完全不依赖 GL） ---
    Canvas canvas(opt.w, opt.h);
    if (!buildScene(opt.scene, canvas)) return 2;
    printf("[cli] 场景 '%s'  %dx%d\n", opt.scene.c_str(), opt.w, opt.h);

    if (!opt.dumpCanvas.empty()) {
        if (!writePNG(opt.dumpCanvas, canvas.image())) return 1;
        printf("[cli] 已导出画布原图: %s\n", opt.dumpCanvas.c_str());
    }

    // --- 2. 建隐藏窗口的 GL 上下文 ---
    gfx::GLContext ctx;
    if (!createOffscreenContext(ctx, opt.w, opt.h, "paintboard offline")) {
        fprintf(stderr, "[cli] 无法创建 OpenGL 上下文（离线渲染需要 GL 4.3）\n");
        return 1;
    }

    // 诊断：打印场景本身的内容（确认是场景还是渲染的问题）
    {
        const Image& im = canvas.image();
        int nz = 0, maxA = 0, maxR = 0;
        for (size_t i = 0; i < im.pixelCount(); i++) {
            const RGBA8& p = im.data()[i];
            if (p.r || p.g || p.b || p.a) nz++;
            if (p.a > maxA) maxA = p.a;
            if (p.r > maxR) maxR = p.r;
        }
        printf("[cli] 场景内容: 非空 %d/%zu  最大alpha %d  最大R %d\\n", nz,
               im.pixelCount(), maxA, maxR);
    }

    // --- 3. 跑光照 ---
    // 只剩扫描 GI（路径追踪已于 2026-10-19 移除）；模式合法性在 parseArgs 里已校验。
    gfx::LightSweep sweep;
    if (!sweep.init(opt.w, opt.h)) {
        fprintf(stderr, "[cli] 扫描 GI 初始化失败\n");
        return 1;
    }
    sweep.settings().rayRadius = opt.radius;
    sweep.settings().emitScale = opt.emit;
    // 诊断开关（见 LightSettings::debugScan）。留一个环境变量入口，
    // 是为了在排查光照问题时不必每次改代码重编。

    for (int i = 0; i < 3; i++) sweep.settings().ambient[i] = opt.ambient[i];
    sweep.uploadAll(canvas.image(), canvas.materialData());

    if (opt.progressive) {
        // 渐进哈希采样：逐帧累积，每帧每像素采一个方向
        sweep.setProgressive(true);
        sweep.setDenoise(opt.denoise, opt.denoiseStrength, 3);
        const int nf = std::max(1, opt.progFrames);
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < nf; f++) {
            sweep.beginFrame();
            sweep.renderLayer(0);
            sweep.endFrame();
        }
        sweep.composite();
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("[cli] 渐进哈希采样：%d 帧共 %.2f ms（%.2f ms/帧，%d 个方向%s）\n", nf, ms,
               ms / (double)nf, sweep.directionCount(), opt.denoise ? "，去噪开" : "");
    } else {
        const auto t0 = std::chrono::steady_clock::now();
        sweep.render();
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("[cli] 扫描 GI 耗时 %.2f ms（%d 个方向）\n", ms, sweep.directionCount());
    }
    const GLuint srcTex = sweep.resultTexture();

    // --- 4. 回读并写 PNG ---
    Image result(opt.w, opt.h);
    if (!gfx::readbackTexture(srcTex, opt.w, opt.h, result.data())) {
        fprintf(stderr, "[cli] 回读渲染结果失败\n");
        return 1;
    }
    // 输出统一给不透明 alpha，方便直接查看
    for (size_t i = 0; i < result.pixelCount(); i++) result.data()[i].a = 255;
    // 写 PNG 之前翻一次 —— 与画板导出口径一致（用户实测导出上下颠倒）
    result.flipVertical();

    if (opt.scene == "orient") {
        auto rl = [&](int y) {
            double s = 0.0;
            for (int x = 0; x < opt.w; x++) {
                const RGBA8 p = result.data()[(size_t)y * opt.w + x];
                s += p.r + p.g + p.b;
            }
            return s / (3.0 * opt.w);
        };
        printf("[朝向] 导出图 首行 %.1f  末行 %.1f（已在导出前翻一次）\n", rl(0),
               rl(opt.h - 1));
    }

    if (!writePNG(opt.out, result)) return 1;

    printf("[cli] 已写出: %s\n", opt.out.c_str());
    return 0;
}
