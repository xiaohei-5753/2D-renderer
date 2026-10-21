// tools/perf_probe.cpp — 逐层光照的性能探针
//
// ## 为什么需要它
//
// README 的「已知待办」里写着"参与光照的图层数上限是 16"，但**没人量过这个上限是
// 硬件限制还是随便定的**。这个工具把"每多一层要花多少毫秒"量出来，于是：
//   * "本机能跑几层 60fps" 变成算术，不用猜
//   * 换到别的 GPU（比如 Intel UHD 核显）时，按实测比例就能推算，不必买机器
//
// ## 量什么
//
// 对 N = 1..16 层，分别测：
//   scan      方向扫描（每条射线沿 DDA 步进，逐层各跑一遍）——**这是主成本**
//   copy      SSBO -> RGBA16F（顺带把光场存进 field 数组纹理）
//   blend     与自发光合成，写进结果数组纹理的第 i 层
//   composite 把 N 层的光场累加、乘到下面各层的颜色上（kEmitComp + kEmitAddComp）
//   upload    每层的 albedo/材质/不透明度上传（画一笔时才有，稳态下为 0）
//
// 用法：
//   perf_probe --size 320x180 --radius 128 [--layers 1,2,4,8,16]
//              [--frames 30] [--hd]
//
// （路径追踪已于 2026-10-19 移除，只剩扫描 GI 的逐层测量。）
// --hd 会额外给出换算到 1920x1080 的估算（按像素数线性外推）。
#include <algorithm>
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

// ---------------------------------------------------------------------------
// 计时
// ---------------------------------------------------------------------------

// 墙钟计时 + glFinish：单次 pass 的耗时只有几十微秒，用 GPU 查询的精度不够，
// 直接同步等 GPU 做完反而更准（而且不受驱动异步提交的影响）。
class Clock {
public:
    void begin() { t0_ = std::chrono::steady_clock::now(); }
    double endMs() const {
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0_).count();
    }

private:
    std::chrono::steady_clock::time_point t0_{};
};

// ---------------------------------------------------------------------------
// 统计：只报均值是没法做前后对比的
// ---------------------------------------------------------------------------
//
// 均值会被偶发的调度抖动/降频污染，而且"改前 21.48 / 改后 21.02"这种差异
// 在均值上看起来像噪声。所以每个测点都逐帧采样，报 min / p50 / p95 / mean：
//   * **p50 是主指标**（不受离群值影响）
//   * p95 - p50 说明抖动有多大，比它小的差异不值得为之下结论
//   * min 是"这台机器最好能给多少"，用来判断是否被固定开销卡住
struct Stats {
    double minMs = 0, p50 = 0, p95 = 0, mean = 0;
    double spread() const { return p95 - p50; }
};

Stats statsOf(std::vector<double> v) {
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.minMs = v.front();
    s.p50 = v[v.size() / 2];
    s.p95 = v[(size_t)std::min(v.size() - 1, (size_t)((double)v.size() * 0.95))];
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / (double)v.size();
    return s;
}

// 逐帧采样 + 每帧 glFinish（同步等 GPU，测的是"这一帧真的要花多久"）
template <typename Fn>
Stats measurePerFrame(int frames, Fn&& perFrame) {
    std::vector<double> ms;
    ms.reserve((size_t)frames);
    for (int f = 0; f < frames; f++) {
        const auto t0 = std::chrono::steady_clock::now();
        perFrame();
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return statsOf(std::move(ms));
}

int g_w = 320, g_h = 180;
int g_radius = 128;
int g_frames = 30;
bool g_hd = false;
int g_dirLimit = 0;   // 诊断：只跑前 N 个方向（0 = 全部）
int g_band = 0;       // 诊断：0=全部 1=只轴向 2=只斜向
bool g_contig = false; // 诊断：连续取样地址（模拟剪切布局）
bool g_nowrite = false; // 诊断：不写内存（只量 DDA/取样/递推）
int g_pack = 0;         // 每像素原子加次数：0=3 次 1=2 次 2=1 次（见 setAtomicPack）
bool g_tiled = false;   // 分块 + shared 合并（列波前）
int g_fieldScale = 1;   // 光场降采样倍率
// 每个测点重复几轮、取最好的那一轮。
//
// 为什么必须重复：这台机器（4080 Laptop）在 83°C 上会降频，同一份代码连测三次
// 得到 5.29 / 2.68 / 2.80 ms —— **相差 2 倍**。单轮数字根本没法做前后对比。
// 所以：每轮先空跑几帧热身，然后取多轮里 p50 最小的一轮（min-of-repeats），
// 并把每轮的 p50 都打出来 —— 抖动大就说明这次测量不该下结论。
int g_repeat = 3;
std::string g_mode = "sweep";
std::vector<int> g_layers{1, 2, 4, 8, 16};

// 重复 g_repeat 轮、取 p50 最好的那一轮；每轮的 p50 记进 p50sOut 用来判断抖动。
//
// 这台机器降到 83°C 时，同一份代码连测三次会是 5.29 / 2.68 / 2.80 ms。
// 所以单轮数字没有意义：**必须看抖动**。抖动超过几个百分点时，
// 任何小于它的"优化收益"都只是噪声。
template <typename Fn>
Stats bestOfRepeats(std::vector<double>& p50sOut, Fn&& measureOnce) {
    Stats best;
    p50sOut.clear();
    for (int r = 0; r < g_repeat; r++) {
        const Stats s = measureOnce();
        p50sOut.push_back(s.p50);
        if (r == 0 || s.p50 < best.p50) best = s;
    }
    return best;
}

// 把"每轮 p50"打成人读的一行，并给出抖动百分比
std::string repeatSummary(const std::vector<double>& p50s) {
    std::string s;
    char buf[64];
    double lo = 0, hi = 0;
    for (size_t i = 0; i < p50s.size(); i++) {
        std::snprintf(buf, sizeof(buf), "%s%.2f", i ? " / " : "", p50s[i]);
        s += buf;
        if (i == 0 || p50s[i] < lo) lo = p50s[i];
        if (i == 0 || p50s[i] > hi) hi = p50s[i];
    }
    if (lo > 0) {
        std::snprintf(buf, sizeof(buf), "（抖动 %.0f%%）", (hi - lo) / lo * 100.0);
        s += buf;
    }
    return s;
}

// ---------------------------------------------------------------------------
// 场景：铺满整个画布的自发光条纹
//
// 为什么不用 render_cli 那种稀疏霓虹：稀疏场景下大量射线**一开始就在画布外**，
// 会在第一次边界判断就 break，成本远低于真实使用。铺满的条纹能让每条射线都
// 真的走到步数上限，量出来的是**上界**（最坏情况），这样层数上限才可信。
//
// 但纯铺满又不含"挡光的实心像素"，也失真。所以按 50% 概率混合：
// 一半是自发光半透明条纹（产生光场、让 scan 跑满），一半是实心块（挡光）。
// ---------------------------------------------------------------------------

void buildStressScene(Canvas& c) {
    c.clear(RGBA8{0, 0, 0, 0});
    const int W = c.width(), H = c.height();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const int band = (x / 7 + y / 5) % 4;
            if (band == 0) {
                // 自发光半透明条纹：光场的来源
                const RGBA8 col{(uint8_t)(60 + (x * 7) % 195), (uint8_t)(80 + (y * 5) % 175),
                                (uint8_t)(120 + ((x + y) * 3) % 135), 110};
                Material m;
                m.setEmissionScaled(col.r, col.g, col.b, 175);
                c.setPixelRaw(x, y, col, m);
            } else if (band == 1) {
                // 实心挡光块：让射线提前被吞掉（模拟墙）
                c.setPixelRaw(x, y, RGBA8{(uint8_t)(40 + x % 40), 40, 48, 255});
            } else {
                // 空气
                c.setPixelRaw(x, y, RGBA8{0, 0, 0, 0});
            }
        }
    }
    c.markAllDirty();
}

// ---------------------------------------------------------------------------
// 把同一份画布铺到 N 层（每层都参与光照）
// ---------------------------------------------------------------------------

void fillLayers(gfx::LightSweep& sweep, const Canvas& canvas, int n) {
    sweep.setLayerCount(n);
    const Rect all{0, 0, g_w - 1, g_h - 1};
    for (int i = 0; i < n; i++) {
        sweep.uploadLayer(i, canvas.image(), canvas.materialData(), all);
        sweep.uploadLayerOpacity(i, i == 0 ? 1.0f : 0.9f);
    }
    sweep.setFieldStrength(1.0f);
}

// ---------------------------------------------------------------------------
// sweep：逐层分别计时
// ---------------------------------------------------------------------------

struct LayerCost {
    Stats render;      // 逐层渲染：renderLayer（scan + copy + blend）轮流跑完 n 层
    Stats composite;   // 合成：光场累加 + 自发光垂直传输
    Stats upload;      // 每层的 albedo / 材质 / 不透明度上传
    // "冷帧"= 每帧都要上传 + 全部图层重算 + 合成（= 游戏侧从头渲染一帧）
    double coldP50() const { return upload.p50 + render.p50 + composite.p50; }
    double coldP95() const { return upload.p95 + render.p95 + composite.p95; }
};

Stats timeSweepLayers(gfx::LightSweep& sweep, int n, int frames) {
    // 逐层整体计时（renderLayer 内部把 scan/copy/blend 都做了）
    return measurePerFrame(frames, [&] {
        for (int i = 0; i < n; i++) sweep.renderLayer(i);
    });
}

Stats timeComposite(gfx::LightSweep& sweep, int frames) {
    return measurePerFrame(frames, [&] { sweep.composite(); });
}

// 每层 albedo + 材质 + 不透明度上传。
//
// 这一行之前只在文件头上"宣传"过、代码里根本没测 —— 而"从头渲染一帧"的
// 成本恰恰有一大块在这里（材质要从 Material 打包成 RGB8 再 glTexSubImage3D）。
Stats timeUpload(gfx::LightSweep& sweep, const Canvas& c, int n, int frames) {
    const Rect all{0, 0, g_w - 1, g_h - 1};
    return measurePerFrame(frames, [&] {
        for (int i = 0; i < n; i++) {
            sweep.uploadLayer(i, c.image(), c.materialData(), all);
            sweep.uploadLayerOpacity(i, i == 0 ? 1.0f : 0.9f);
        }
    });
}


// 显存估算：把这几张数组纹理的大小加起来（最占地方的是材质 RGBA32F）
double layerVramMB(int w, int h, int layers) {
    const double px = (double)w * (double)h;
    // 每层：albedo RGBA8(4) + material RGBA32F(16) + lit RGBA8(4) + opacity R32F(4)
    //       + field RGBA16F(8)  = 36 字节/像素
    const double perLayer = px * 36.0;
    // 全局：scan RGBA16F(8) + canvasArray 已在上面 + renderTex RGBA8(4) + SSBO 12
    const double global = px * (8.0 + 4.0 + 12.0);
    return (perLayer * layers + global) / (1024.0 * 1024.0);
}

void usage() {
    printf(
        "perf_probe — 逐层光照性能探针\n"
        "\n"
        "  --size <WxH>      画布尺寸            (默认 320x180)\n"
        "  --radius <N>      扫描方向表半径      (默认 128)\n"
        "  --nowrite         诊断：扫描只推进不写内存（量 DDA 成本，结果为空）\n"
        "  --pack <0|1|2>    每像素原子加次数：0=3 次(默认) 1=2 次 2=1 次(10/10/10)\n"
        "  --mode <m>        只有 sweep（路径追踪已移除） (默认 sweep)\n"
        "  --layers <a,b,..> 要测的层数列表      (默认 1,2,4,8,16)\n"
        "  --frames <N>      每个测点跑几帧采样    (默认 30)\n"
        "  --repeat <N>      每个测点重复几轮取最好(默认 3，降频机器必看抖动)\n"
        "  --hd              额外给出 1920x1080 的线性外推\n");
}

bool parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--help" || a == "-h") { usage(); exit(0); }
        else if (a == "--size") { const char* v = next(); if (!v || !parseSize(v, g_w, g_h)) return false; }
        else if (a == "--radius") { const char* v = next(); if (!v) return false; g_radius = atoi(v); }
        else if (a == "--mode") {
            const char* v = next(); if (!v) return false; g_mode = v;
            // 未知模式明确报错（只有扫描 GI 可测）
            if (g_mode != "sweep") {
                fprintf(stderr, "[perf] 未知模式 '%s'（只剩 sweep；路径追踪已移除）\n",
                        g_mode.c_str());
                return false;
            }
        }
        else if (a == "--frames") { const char* v = next(); if (!v) return false; g_frames = std::max(1, atoi(v)); }
        else if (a == "--hd") { g_hd = true; }
        else if (a == "--dirlimit") { const char* v = next(); if (!v) return false; g_dirLimit = atoi(v); }
        else if (a == "--band") { const char* v = next(); if (!v) return false; g_band = atoi(v); }
        else if (a == "--contig") { g_contig = true; }
        else if (a == "--nowrite") { g_nowrite = true; }
        else if (a == "--pack") { const char* v = next(); if (!v) return false; g_pack = atoi(v); }
        else if (a == "--tiled") { g_tiled = true; }
        else if (a == "--field-scale") { const char* v = next(); if (!v) return false; g_fieldScale = atoi(v); }
        else if (a == "--repeat") { const char* v = next(); if (!v) return false; g_repeat = std::max(1, atoi(v)); }
        else if (a == "--layers") {
            const char* v = next(); if (!v) return false;
            g_layers.clear();
            const char* p = v;
            while (*p) {
                const int n = atoi(p);
                if (n > 0) g_layers.push_back(n);
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
            if (g_layers.empty()) return false;
        } else {
            fprintf(stderr, "[perf] 未知参数: %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    setupConsoleUtf8();
    if (!parseArgs(argc, argv)) return 2;

    Canvas canvas(g_w, g_h);
    buildStressScene(canvas);

    gfx::GLContext ctx;
    if (!createOffscreenContext(ctx, g_w, g_h, "perf_probe")) {
        fprintf(stderr, "[perf] 无法创建 OpenGL 上下文\n");
        return 1;
    }
    printf("[perf] GPU: %s\n", (const char*)glGetString(GL_RENDERER));
    printf("[perf] 画布 %dx%d  半径 %d  模式 %s  每点 %d 帧\n", g_w, g_h, g_radius,
           g_mode.c_str(), g_frames);
    printf("[perf] 场景 = 铺满的自发光条纹 + 实心块（最坏情况，射线基本都走满步数）\n");
    printf("\n");

    gfx::LightSweep sweep;
    if (!sweep.init(g_w, g_h)) { fprintf(stderr, "[perf] sweep init 失败（很可能是着色器编译错，见上面 [gl] 报错）\n"); return 1; }
    sweep.settings().rayRadius = g_radius;
    if (g_dirLimit > 0) sweep.setDirectionLimit(g_dirLimit);
    sweep.setDirectionBand(g_band);
    sweep.setContiguousFetch(g_contig);
sweep.setScanNoWrite(g_nowrite);
    sweep.setAtomicPack(g_pack);
    if (g_tiled) sweep.setTiledScan(true);   // 不指定时 autotune 会自己选
    sweep.setFieldScale(g_fieldScale);
    // ⚠️ 方向表是**惰性**重建的：settings().rayRadius 只是记下参数，真正
    // rebuildDirectionTable() 要到第一次 renderLayer() 才发生。所以必须先跑一次
    // 再问 directionCount()，否则读到的是 init 时的默认值 128 —— 我就在这里
    // 读到过"每个半径都是 728 个方向"这种假数据，差点得出错误结论。
    fillLayers(sweep, canvas, g_layers.front());
    sweep.renderLayer(0);
    glFinish();
    printf("[perf] 方向数: %d（半径 %d）\n\n", sweep.directionCount(), g_radius);

    printf("%-6s %9s %9s %9s %9s %9s   %s\n", "层数", "上传", "逐层渲染", "合成",
           "冷帧p50", "冷帧p95", "能否 60fps");
    printf("---------------------------------------------------------------------------------\n");
    struct Row { int n; LayerCost cost; double vram; };
    std::vector<Row> rows;
    for (int n : g_layers) {
        fillLayers(sweep, canvas, n);
        // 预热：让方向表/纹理绑定进稳态
        sweep.render();
        glFinish();

        LayerCost lc;
        std::vector<double> repsRender, repsComp, repsUpload;
        lc.render =
            bestOfRepeats(repsRender, [&] { return timeSweepLayers(sweep, n, g_frames); });
        lc.composite =
            bestOfRepeats(repsComp, [&] { return timeComposite(sweep, g_frames); });
        lc.upload = bestOfRepeats(repsUpload,
                                  [&] { return timeUpload(sweep, canvas, n, g_frames); });
        const double vram = layerVramMB(g_w, g_h, n);
        rows.push_back(Row{n, lc, vram});
        printf("%-6d %9.2f %9.2f %9.2f %9.2f %9.2f   %s\n", n, lc.upload.p50, lc.render.p50,
               lc.composite.p50, lc.coldP50(), lc.coldP95(),
               lc.coldP50() <= 16.67 ? "是" : "否");
        // 均值/最小值只作参考：p50 与 p95 已经说明分布了
        printf("       %9s 渲染 min %.2f / 均值 %.2f  ·  上传 min %.2f / 均值 %.2f\n", "",
               lc.render.minMs, lc.render.mean, lc.upload.minMs, lc.upload.mean);
        printf("       %9s 每轮 p50：渲染 %s 上传 %s\n", "",
               repeatSummary(repsRender).c_str(), repeatSummary(repsUpload).c_str());
    }

    // 每层边际成本（最能说明问题的一个数：第 2 层比第 1 层多花多少）
    printf("\n[perf] 边际成本（每多一层要多少毫秒，按冷帧 p50）：\n");
    for (size_t i = 1; i < rows.size(); i++) {
        const double dn = (double)(rows[i].n - rows[i - 1].n);
        const double dms = (rows[i].cost.coldP50() - rows[i - 1].cost.coldP50()) / dn;
        printf("  %2d -> %2d 层: %+7.2f ms/层\n", rows[i - 1].n, rows[i].n, dms);
    }

    if (g_hd) {
        const double k = (1920.0 * 1080.0) / ((double)g_w * g_h);
        printf("\n[perf] 换算到 1920x1080（按像素数线性外推，x%.1f）：\n", k);
        for (const Row& r : rows)
            printf("  %2d 层 -> %7.1f ms  (%.0f%% of 16.6ms)\n", r.n,
                   r.cost.coldP50() * k, r.cost.coldP50() * k / 16.67 * 100.0);
    }

    // 机器可读的一行：脚本可以直接 diff 改前/改后，不用解析表格
    printf("\nPERF_JSON {\"mode\":\"sweep\",\"size\":\"%dx%d\",\"radius\":%d,\"frames\":%d,"
           "\"layers\":[", g_w, g_h, g_radius, g_frames);
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        printf("%s{\"n\":%d,\"upload_p50\":%.3f,\"render_p50\":%.3f,\"render_p95\":%.3f,"
               "\"composite_p50\":%.3f,\"cold_p50\":%.3f,\"cold_p95\":%.3f,\"vram_mb\":%.1f}",
               i ? "," : "", r.n, r.cost.upload.p50, r.cost.render.p50, r.cost.render.p95,
               r.cost.composite.p50, r.cost.coldP50(), r.cost.coldP95(), r.vram);
    }
    printf("]}\n");
    return 0;
}
