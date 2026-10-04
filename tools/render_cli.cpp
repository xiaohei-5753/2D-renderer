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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/console.h"
#include "core/image.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/light_sweep.h"

using namespace pb;

namespace {

struct Options {
    std::string scene = "neon";
    int w = 320, h = 180;
    int radius = 128;
    float emit = 1.0f;
    float ambient[3] = {0.10f, 0.10f, 0.12f};
    std::string out = "out.png";
    std::string dumpCanvas;   // 非空则额外导出未受光的画布原图
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
        "  --dump-canvas <file.png>  额外导出未受光的画布原图\n");
}

bool parseSize(const char* s, int& w, int& h) {
    return sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
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
Material emissive(uint8_t e) {
    Material m;
    m.emission = e;
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
    const Material lamp = emissive(170);
    // 三根不同颜色的霓虹管
    c.drawLine(40, 30, 120, 30, rgb(80, 200, 255, A), lamp);
    c.drawLine(200, 40, 280, 40, rgb(255, 90, 140, A), lamp);
    c.drawLine(60, 150, 260, 150, rgb(255, 200, 90, A), lamp);
    // 一个实心色块：用来对比"实心像素不受光"
    c.fillRect(140, 70, 180, 110, rgb(40, 40, 48, 255));
    // 一个"不透明但发光"的像素——旧模型（em=rgb*a）根本表达不了这种情况
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

bool buildScene(const std::string& name, Canvas& c) {
    if (name == "empty") { c.clear(RGBA8{0, 0, 0, 0}); return true; }
    if (name == "neon") { sceneNeon(c); return true; }
    if (name == "cornell") { sceneCornell(c); return true; }
    if (name == "checker") { sceneChecker(c); return true; }
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
    gfx::ContextDesc desc;
    desc.width = opt.w;
    desc.height = opt.h;
    desc.visible = false;
    desc.title = "paintboard offline";

    gfx::GLContext ctx;
    if (!ctx.create(desc)) {
        fprintf(stderr, "[cli] 无法创建 OpenGL 上下文（离线渲染需要 GL 4.3）\n");
        return 1;
    }

    // --- 3. 跑光照 ---
    gfx::LightSweep sweep;
    if (!sweep.init(opt.w, opt.h)) {
        fprintf(stderr, "[cli] 光照管线初始化失败\n");
        return 1;
    }
    sweep.settings().rayRadius = opt.radius;
    sweep.settings().emitScale = opt.emit;
    for (int i = 0; i < 3; i++) sweep.settings().ambient[i] = opt.ambient[i];
    sweep.uploadAll(canvas.image(), canvas.materialData());

    const auto t0 = std::chrono::steady_clock::now();
    sweep.render();
    glFinish();
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("[cli] 光照耗时 %.2f ms（%d 个方向）\n", ms, sweep.directionCount());

    // --- 4. 回读并写 PNG ---
    Image result(opt.w, opt.h);
    if (!gfx::readbackTexture(sweep.resultTexture(), opt.w, opt.h, result.data())) {
        fprintf(stderr, "[cli] 回读渲染结果失败\n");
        return 1;
    }
    // 输出统一给不透明 alpha，方便直接查看
    for (size_t i = 0; i < result.pixelCount(); i++) result.data()[i].a = 255;
    if (!writePNG(opt.out, result)) return 1;

    printf("[cli] 已写出: %s\n", opt.out.c_str());
    return 0;
}
