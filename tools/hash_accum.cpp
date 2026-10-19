// tools/hash_accum.cpp — "哈希掩码 + 帧间累积"的角向采样实验（**纯 CPU，无 GL**）
//
// ## 要回答的问题
//
// 扫描 GI 的成本 = 每个像素 × 每个方向一次 deposit（Θ(D·W·H)），瓶颈是原子加。
// 一个诱人的替代方案是：**每帧每个像素只采一个方向**（用坐标哈希决定采哪个），
// 于是每个像素只写自己、无原子、无需任何同步；靠多帧累积收敛。
//
// 这个工具不实现那套渲染管线，而是**先回答它值不值得做**：
//
//   1. 把每个方向的光场**物化**一遍（D 张 W×H×3 的浮点场，离线诊断）；
//   2. 在 CPU 上模拟三种采样方案，逐帧累积；
//   3. 报"误差 vs 帧数"曲线，并与确定性扫描（Σ_d / D）逐像素比较。
//
// 这一步便宜、且能一次性判死/放行整类方案 —— 如果 16 帧就足够干净，
// 那"无原子 + 渐进"就是可行的；如果几百帧还脏，那它只适合当预览。
//
// ## 三种方案
//
//   HashRotate   d(p,f) = (hash(p) + f) mod D   —— 用户提的"掩码轮换"
//                每帧每个方向被 ~W·H/D 个像素采样（分层），且**每个像素在 D 帧内
//                恰好遍历全部方向** ⇒ 第 D 帧的累积结果与确定性扫描**相等**。
//   RandomFrame  d(p,f) = hash(p,f) mod D       —— 每帧独立随机（路径追踪的口径）
//   FixedRandom  d(p,f) = hash(p)               —— 每像素固定一个方向，永不收敛
//                （对照组：用来说明"轮换"这件事本身值多少）
//
// ## 为什么要在 CPU 上做
//
// 物化 D 张场之后，任何采样方案都只是"查表 + 加法"，728 帧也只要 10^7 次操作；
// 而每帧重算一遍 gather 需要 W·H·L ≈ 10^6~10^7 步 × 728 帧，慢两个数量级。
// 代价是**物化**这一步的内存（D·W·H·3 float）—— 所以默认画布是 160×90。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/bench_scene.h"
#include "core/composite.h"
#include "core/console.h"
#include "core/image.h"
#include "core/layer.h"
#include "core/paths.h"
#include "core/types.h"

using namespace pb;

namespace {

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------
int g_w = 160, g_h = 90;
int g_radius = 32;
int g_layer = -1;      // -1 = 自动挑"自发光像素最多"的那一层
int g_frames = 0;      // 0 = 跑满一个周期（D 帧）
float g_ambient = 0.25f;
float g_emitScale = 1.0f;
float g_propagate = 1.0f;
std::string g_scene = "neon-city";
std::string g_outDir = "out";
int g_scale = 2;          // 输出图整数放大（像素画不需要插值）
float g_exposure = 0.0f;  // 0 = 按参考解的 99.5 分位自动曝光
std::string g_seqDir;     // 非空 = 逐帧写 PNG，供人翻看
std::string g_montage;    // 空 = <out>/hash_montage.png
// 显示时减掉"只有环境光"的那份场。
//
// 为什么要减：光场 = 环境光 × 透过率 + 发光体洒出来的光。前者是一层均匀的灰
// （均值 0.4），会把后者压得看不见；直接看 F 只能看到一片灰 + 轻微斑驳。
// 减掉之后画面就是"发光体照亮了什么"，噪声和收敛过程一目了然。
// **只影响显示**，误差指标仍然按原始 F 算。
bool g_glowView = true;

void usage() {
    printf(
        "hash_accum — 角向采样（每帧每像素一个方向）的收敛实验，纯 CPU\n"
        "\n"
        "  --scene <name>   场景（默认 neon-city）\n"
        "  --size <WxH>     画布（默认 160x90；内存 = D*W*H*3*4 字节）\n"
        "  --radius <R>     方向表半径（默认 32 → 184 方向；128 → 728）\n"
        "  --layer <N>      用第几层（默认自动挑自发光最多的那层）\n"
        "  --frames <N>     累积到第几帧（默认 = 方向数 D）\n"
        "  --ambient <f>    环境光（默认 0.25）\n"
        "  --scale <N>      输出图放大倍数（默认 2）\n"
        "  --exposure <f>   曝光（默认自动：参考解 99.5 分位的倒数）\n"
        "  --seq <dir>      逐帧输出 PNG（看过程用）\n"
        "  --out <dir>      PNG 输出目录（默认 out）\n"
        "\n"
        "输出：<out>/hash_montage.png（N=1,2,4,8,16,32,64,D 的拼图）、\n"
        "      <out>/hash_ref.png（参考解）\n");
}

bool parseSize(const char* s, int& w, int& h) {
    int a = 0, b = 0;
    if (std::sscanf(s, "%dx%d", &a, &b) != 2) return false;
    if (a <= 0 || b <= 0) return false;
    w = a;
    h = b;
    return true;
}

// ---------------------------------------------------------------------------
// 方向表：与 gfx/light_sweep.cpp 的 rebuildDirectionTable() 逐字同构
// （中点圆一个八分圆 + 8 重对称）
// ---------------------------------------------------------------------------
struct Dir { int dx, dy; };

std::vector<Dir> buildDirs(int R) {
    std::vector<Dir> dirs;
    int m = 1 - R, x = 0, y = R;
    while (x < y) {
        const int px[8] = {x, -x, x, -x, y, -y, y, -y};
        const int py[8] = {y, y, -y, -y, x, x, -x, -x};
        for (int i = 0; i < 8; i++) dirs.push_back(Dir{px[i], py[i]});
        x++;
        if (m < 0) {
            m += 2 * x + 1;
        } else {
            y--;
            m += 2 * (x - y) + 1;
        }
    }
    return dirs;
}

// ---------------------------------------------------------------------------
// 物化：对每个方向做一遍扫描，把 deposit 写进各自的场（无原子）
//
// 与 kScanComp 的算术逐项对应（浮点版，不做定点量化 —— 这里研究的是采样误差，
// 不是量化误差）。deposit = clamp(lt / D, 0, 1)，与 GPU 的 add=lt*invN 一致。
// ---------------------------------------------------------------------------
void materialize(const Layer& L, const std::vector<Dir>& dirs, float ambient,
                 float emitScale, float cp, std::vector<float>& field,
                 double& stepsTotal) {
    const int W = L.width(), H = L.height();
    const int D = (int)dirs.size();
    const float invN = 1.0f / (float)D;
    field.assign((size_t)D * (size_t)W * (size_t)H * 3, 0.0f);
    stepsTotal = 0;

    for (int di = 0; di < D; di++) {
        const int ux = dirs[(size_t)di].dx, uy = dirs[(size_t)di].dy;
        const int dx = std::abs(ux), dy = std::abs(uy);
        const int sx = (ux >= 0) ? 1 : -1, sy = (uy >= 0) ? 1 : -1;
        const int rc = (dx >= dy) ? (2 * H + 1) : (2 * W + 1);
        // ⚠️ 与 kernel 一致：射线数补齐到 256 的倍数。多出来的射线大多一出界就停，
        // 但**有些会走进画布**（起点在画布外、朝向画布），kernel 的注释专门写了这点，
        // 丢掉它们会让参考解与 GPU 的结果不一致。
        const int rcPadded = ((rc + 255) / 256) * 256;
        float* out = field.data() + (size_t)di * (size_t)W * (size_t)H * 3;

        for (int ri = 0; ri < rcPadded; ri++) {
            int x, y;
            if (dx >= dy) {
                x = (sx > 0) ? 0 : W - 1;
                y = -H + ri;
            } else {
                x = -W + ri;
                y = (sy > 0) ? 0 : H - 1;
            }
            float lr = ambient, lg = ambient, lb = ambient;
            int er = dx - dy;
            while (true) {
                if (x >= 0 && x < W && y >= 0 && y < H) {
                    const RGBA8 c = L.albedo.at(x, y);
                    const Material m = L.material[(size_t)y * (size_t)W + (size_t)x];
                    const float cov = c.a / 255.0f;
                    const float cr = c.r / 255.0f, cg = c.g / 255.0f, cb = c.b / 255.0f;
                    const float emR = m.emR / 255.0f * emitScale;
                    const float emG = m.emG / 255.0f * emitScale;
                    const float emB = m.emB / 255.0f * emitScale;
                    // lt = lt * mix(1,rgb,cov) * (1-cov) + em;  然后 + em*(1-cov)*cp
                    const float k = 1.0f - cov;
                    const float tR = (k + cov * cr) * k;
                    const float tG = (k + cov * cg) * k;
                    const float tB = (k + cov * cb) * k;
                    lr = lr * tR + emR;
                    lg = lg * tG + emG;
                    lb = lb * tB + emB;
                    lr += emR * k * cp;
                    lg += emG * k * cp;
                    lb += emB * k * cp;

                    const size_t o = ((size_t)y * (size_t)W + (size_t)x) * 3;
                    out[o + 0] += std::clamp(lr * invN, 0.0f, 1.0f);
                    out[o + 1] += std::clamp(lg * invN, 0.0f, 1.0f);
                    out[o + 2] += std::clamp(lb * invN, 0.0f, 1.0f);
                }
                const int e2 = 2 * er;
                if (e2 > -dy) { er -= dy; x += sx; }
                if (e2 < dx) { er += dx; y += sy; }
                stepsTotal += 1.0;
                // 与 kernel 相同的"离开画布就停"判据（垂直方向越界不算，
                // 因为射线起点本来就在画布外的垂直一侧）
                if ((sx > 0 && x >= W) || (sx < 0 && x < 0) || (sy > 0 && y >= H) ||
                    (sy < 0 && y < 0))
                    break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 哈希
// ---------------------------------------------------------------------------
inline uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

inline uint32_t pixelHash(int x, int y) {
    return mix32((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u);
}

// ---------------------------------------------------------------------------
// 采样方案
// ---------------------------------------------------------------------------
enum class Scheme { HashRotate, SpreadRotate, RandomFrame, FixedRandom };
const char* schemeName(Scheme s) {
    switch (s) {
        case Scheme::HashRotate: return "HashRotate";
        case Scheme::SpreadRotate: return "SpreadRotate";
        case Scheme::RandomFrame: return "RandomFrame";
        case Scheme::FixedRandom: return "FixedRandom";
    }
    return "?";
}

// 与 D 互素的"黄金比例"步长：让连续帧采到的方向在角度上**均匀铺开**，
// 而不是连续的一段。轮换方案的关键就在这里 —— 连续步长（stride=1）在 N≪D 时
// 只覆盖一个小扇区，误差会一直很脏，直到 N 接近 D 才突然变好。
uint32_t spreadStride(int D) {
    const double golden = 0.6180339887498949;
    int s = (int)(golden * (double)D);
    if (s < 1) s = 1;
    auto gcd = [](int a, int b) { while (b) { const int t = a % b; a = b; b = t; } return a; };
    while (gcd(s, D) != 1) s++;
    return (uint32_t)s;
}

int pickDir(Scheme s, int x, int y, int frame, int D) {
    const uint32_t base = pixelHash(x, y);
    switch (s) {
        case Scheme::HashRotate:
            // 步长取 1（必然与 D 互素）：每个像素在 D 帧内**恰好遍历全部方向**，
            // 于是第 D 帧的累积结果与确定性求和相等 —— 这是"掩码轮换"的关键性质。
            return (int)((base + (uint32_t)frame) % (uint32_t)D);
        case Scheme::SpreadRotate: {
            static const uint32_t stride = spreadStride(D);
            return (int)((base + (uint32_t)frame * stride) % (uint32_t)D);
        }
        case Scheme::RandomFrame:
            return (int)(mix32(base ^ ((uint32_t)frame * 2654435761u)) % (uint32_t)D);
        case Scheme::FixedRandom:
            return (int)(base % (uint32_t)D);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 误差指标
// ---------------------------------------------------------------------------
struct ErrStat {
    // 光场本身的误差（RMSE 与 8 位级换算）
    double rmse = 0, maxAbs = 0, levels = 0, badPct = 0;
    // **最终显示图**的误差：display = clamp(albedo * (F + ambient))。
    // 这才是"看不看得出来"的指标 —— 光场误差要乘上 albedo 才落到屏幕上。
    double dispLevels = 0, dispBadPct = 0;
};

ErrStat compare(const Layer& L, const std::vector<float>& ref,
                const std::vector<float>& est, float ambient, int w, int h) {
    ErrStat s;
    double se = 0, mx = 0, dse = 0;
    long long bad = 0, dbad = 0, vals = 0, dvals = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const RGBA8 c = L.albedo.at(x, y);
            const float alb[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
            const size_t o = ((size_t)y * (size_t)w + (size_t)x) * 3;
            for (int k = 0; k < 3; k++) {
                const double d = (double)est[o + (size_t)k] - (double)ref[o + (size_t)k];
                se += d * d;
                mx = std::max(mx, std::abs(d));
                vals++;
                if (std::abs(d) * 255.0 > 1.0) bad++;

                const double rv = std::clamp((double)alb[k] * ((double)ref[o + (size_t)k] + ambient), 0.0, 1.0);
                const double ev = std::clamp((double)alb[k] * ((double)est[o + (size_t)k] + ambient), 0.0, 1.0);
                dse += (ev - rv) * (ev - rv);
                dvals++;
                if (std::abs(ev - rv) * 255.0 > 2.0) dbad++;
            }
        }
    s.rmse = std::sqrt(se / (double)vals);
    s.maxAbs = mx;
    s.levels = s.rmse * 255.0;
    s.badPct = 100.0 * (double)bad / (double)vals;
    s.dispLevels = std::sqrt(dse / (double)dvals) * 255.0;
    s.dispBadPct = 100.0 * (double)dbad / (double)dvals;
    return s;
}

// ---------------------------------------------------------------------------
// 把浮点场变成"能看"的图
//
// 光场的动态范围很大（最亮 ≈ 1，均值 ≈ 0.02）：直接 clamp 出来的图全黑，
// 看不出过程。所以按参考解的 **99.5 分位**归一曝光，再做 gamma 0.5 提亮暗部。
// ---------------------------------------------------------------------------
float autoExposure(const std::vector<float>& ref) {
    std::vector<float> v;
    v.reserve(ref.size());
    for (float f : ref)
        if (f > 0.0f) v.push_back(f);
    if (v.empty()) return 1.0f;
    std::sort(v.begin(), v.end());
    const size_t k = (size_t)((double)v.size() * 0.995);
    const float p = v[std::min(k, v.size() - 1)];
    return (p > 1e-6f) ? (1.0f / p) : 1.0f;
}

// RGB8，w*scale × h*scale（最近邻放大，像素画不需要插值）
std::vector<uint8_t> toneImage(const std::vector<float>& v, int w, int h, float exposure,
                               int scale) {
    std::vector<uint8_t> px((size_t)w * (size_t)scale * (size_t)h * (size_t)scale * 3, 0);
    const int W = w * scale, H = h * scale;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const size_t src = ((size_t)(y / scale) * (size_t)w + (size_t)(x / scale)) * 3;
            const size_t dst = ((size_t)y * (size_t)W + (size_t)x) * 3;
            for (int c = 0; c < 3; c++) {
                float f = v[src + (size_t)c] * exposure;
                f = std::clamp(f, 0.0f, 1.0f);
                f = std::sqrt(f);   // gamma 0.5
                px[dst + (size_t)c] = (uint8_t)(f * 255.0f + 0.5f);
            }
        }
    return px;
}

bool saveRGB(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h) {
    std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[(size_t)i * 4 + 0] = rgb[(size_t)i * 3 + 0];
        rgba[(size_t)i * 4 + 1] = rgb[(size_t)i * 3 + 1];
        rgba[(size_t)i * 4 + 2] = rgb[(size_t)i * 3 + 2];
        rgba[(size_t)i * 4 + 3] = 255;
    }
    return writePNG(path, w, h, rgba.data());
}

// --- 3x5 字模：0-9 数字 + 'N' + 'E'（只为在蒙太奇上标帧数/误差图）---
const uint8_t kGlyph3x5[13][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111},   // 0
    {0b010, 0b110, 0b010, 0b010, 0b111},   // 1
    {0b111, 0b001, 0b111, 0b100, 0b111},   // 2
    {0b111, 0b001, 0b111, 0b001, 0b111},   // 3
    {0b101, 0b101, 0b111, 0b001, 0b001},   // 4
    {0b111, 0b100, 0b111, 0b001, 0b111},   // 5
    {0b111, 0b100, 0b111, 0b101, 0b111},   // 6
    {0b111, 0b001, 0b001, 0b001, 0b001},   // 7
    {0b111, 0b101, 0b111, 0b101, 0b111},   // 8
    {0b111, 0b101, 0b111, 0b001, 0b111},   // 9
    {0b111, 0b100, 0b110, 0b100, 0b111},   // E
    {0b101, 0b111, 0b111, 0b101, 0b101},   // N
    {0b110, 0b101, 0b101, 0b101, 0b110},   // D
};

void drawGlyphRun(std::vector<uint8_t>& rgb, int w, int h, int x0, int y0, const char* s,
                  int scale, uint8_t r, uint8_t g, uint8_t b) {
    int cx = x0;
    for (const char* p = s; *p; p++) {
        int gi = -1;
        if (*p >= '0' && *p <= '9') gi = *p - '0';
        else if (*p == 'E') gi = 10;
        else if (*p == 'N') gi = 11;
        else if (*p == 'D') gi = 12;
        if (gi < 0) { cx += 2 * scale; continue; }
        for (int gy = 0; gy < 5; gy++)
            for (int gx = 0; gx < 3; gx++) {
                if (!(kGlyph3x5[gi][gy] & (1 << (2 - gx)))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        const int px = cx + gx * scale + sx;
                        const int py = y0 + gy * scale + sy;
                        if (px < 0 || px >= w || py < 0 || py >= h) continue;
                        const size_t o = ((size_t)py * (size_t)w + (size_t)px) * 3;
                        rgb[o + 0] = r;
                        rgb[o + 1] = g;
                        rgb[o + 2] = b;
                    }
            }
        cx += 4 * scale;
    }
}

// 误差图：按 max 通道，sqrt 压缩动态范围（这样 N=1..64 的变化都能看见）
std::vector<uint8_t> errorImage(const std::vector<float>& ref,
                                const std::vector<float>& est, int w, int h, int scale) {
    std::vector<uint8_t> px((size_t)w * (size_t)scale * (size_t)h * (size_t)scale * 3, 0);
    const int W = w * scale, H = h * scale;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const size_t src = ((size_t)(y / scale) * (size_t)w + (size_t)(x / scale)) * 3;
            float m = 0;
            for (int c = 0; c < 3; c++)
                m = std::max(m, std::abs(est[src + (size_t)c] - ref[src + (size_t)c]));
            const float g = std::sqrt(std::clamp(m, 0.0f, 1.0f));
            const uint8_t b = (uint8_t)(g * 255.0f + 0.5f);
            const size_t d = ((size_t)y * (size_t)W + (size_t)x) * 3;
            px[d + 0] = b;
            px[d + 1] = b;
            px[d + 2] = b;
        }
    return px;
}

// ---------------------------------------------------------------------------
// 去噪：à-trous 联合双边（3 层，5×5 抽头，空洞步长 1/2/4）
//
// 为什么值得试：这个方案的噪声是**逐像素独立**的，而引导图（覆盖率 = 画布 alpha）
// **完全没有噪声**。用引导图做值域权重，就能在"光场平滑、几何有边"的地方正确停住；
// 只用光场自己当引导，会把噪点当细节保留。
//
// 值域权重还必须**按局部亮度缩放**：光场的噪声方差正比于局部光强（和蒙特卡洛一样），
// 统一 σr 会在暗处过度平滑、在亮处又不动作。所以 σr(p) = kRange·(base + luma(p))。
// ---------------------------------------------------------------------------
inline float lumaOf(const std::vector<float>& v, size_t p) {
    return 0.2126f * v[p] + 0.7152f * v[p + 1] + 0.0722f * v[p + 2];
}

std::vector<float> atrousDenoise(const std::vector<float>& src,
                                 const std::vector<float>& guide, int w, int h, int levels,
                                 float sigmaG, float kRange, float base) {
    const float taps[5] = {1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16};
    std::vector<float> a = src, b(src.size());
    for (int lv = 0; lv < levels; lv++) {
        const int step = 1 << lv;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const size_t p = ((size_t)y * (size_t)w + (size_t)x) * 3;
                const float l0 = lumaOf(a, p);
                const float sr = std::max(1e-4f, kRange * (base + l0));
                const float inv2sr2 = 1.0f / (2.0f * sr * sr);
                const float inv2sg2 = 1.0f / (2.0f * sigmaG * sigmaG);
                const float g0 = guide[(size_t)y * (size_t)w + (size_t)x];
                float acc0 = 0, acc1 = 0, acc2 = 0, wsum = 0;
                for (int ty = -2; ty <= 2; ty++) {
                    const int qy = y + ty * step;
                    if (qy < 0 || qy >= h) continue;
                    for (int tx = -2; tx <= 2; tx++) {
                        const int qx = x + tx * step;
                        if (qx < 0 || qx >= w) continue;
                        const size_t q = ((size_t)qy * (size_t)w + (size_t)qx) * 3;
                        const float dg = guide[(size_t)qy * (size_t)w + (size_t)qx] - g0;
                        const float dl = lumaOf(a, q) - l0;
                        const float wt = taps[ty + 2] * taps[tx + 2] *
                                         std::exp(-dg * dg * inv2sg2 - dl * dl * inv2sr2);
                        acc0 += wt * a[q];
                        acc1 += wt * a[q + 1];
                        acc2 += wt * a[q + 2];
                        wsum += wt;
                    }
                }
                if (wsum > 1e-8f) {
                    b[p] = acc0 / wsum;
                    b[p + 1] = acc1 / wsum;
                    b[p + 2] = acc2 / wsum;
                } else {
                    b[p] = a[p];
                    b[p + 1] = a[p + 1];
                    b[p + 2] = a[p + 2];
                }
            }
        a.swap(b);
    }
    return a;
}
// 变体：**按逐像素噪声标准差**设值域权重（SVGF 的思路）
//
// σr(p) = c·σ_est(p)：噪声大的像素才允许大范围平均，噪声小的地方几乎不动。
// 这比"按亮度缩放"更对症 —— 亮度高的地方不一定噪声大（比如稳定的亮面），
// 而 σ_est 直接来自"同一像素在各方向上采到的值的方差"，是真正的置信度。
std::vector<float> atrousDenoiseVar(const std::vector<float>& src,
                                    const std::vector<float>& guide,
                                    const std::vector<float>& sigma, int w, int h, int levels,
                                    float sigmaG, float cRange, float floorSr) {
    const float taps[5] = {1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16};
    std::vector<float> a = src, b(src.size());
    for (int lv = 0; lv < levels; lv++) {
        const int step = 1 << lv;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const size_t pi = (size_t)y * (size_t)w + (size_t)x;
                const size_t p = pi * 3;
                const float l0 = lumaOf(a, p);
                const float sr = std::max(floorSr, cRange * sigma[pi]);
                const float inv2sr2 = 1.0f / (2.0f * sr * sr);
                const float inv2sg2 = 1.0f / (2.0f * sigmaG * sigmaG);
                const float g0 = guide[pi];
                float acc0 = 0, acc1 = 0, acc2 = 0, wsum = 0;
                for (int ty = -2; ty <= 2; ty++) {
                    const int qy = y + ty * step;
                    if (qy < 0 || qy >= h) continue;
                    for (int tx = -2; tx <= 2; tx++) {
                        const int qx = x + tx * step;
                        if (qx < 0 || qx >= w) continue;
                        const size_t qi = (size_t)qy * (size_t)w + (size_t)qx;
                        const size_t q = qi * 3;
                        const float dg = guide[qi] - g0;
                        const float dl = lumaOf(a, q) - l0;
                        const float wt = taps[ty + 2] * taps[tx + 2] *
                                         std::exp(-dg * dg * inv2sg2 - dl * dl * inv2sr2);
                        acc0 += wt * a[q];
                        acc1 += wt * a[q + 1];
                        acc2 += wt * a[q + 2];
                        wsum += wt;
                    }
                }
                if (wsum > 1e-8f) {
                    b[p] = acc0 / wsum;
                    b[p + 1] = acc1 / wsum;
                    b[p + 2] = acc2 / wsum;
                } else {
                    b[p] = a[p];
                    b[p + 1] = a[p + 1];
                    b[p + 2] = a[p + 2];
                }
            }
        a.swap(b);
    }
    return a;
}

struct Panel {
    std::string label;
    std::vector<uint8_t> rgb;   // 已放大
    int w = 0, h = 0;
};

bool writeMontage(const std::string& path, const std::vector<Panel>& panels, int cols) {
    if (panels.empty()) return false;
    const int pw = panels[0].w, ph = panels[0].h;
    const int gap = 4;
    const int rows = ((int)panels.size() + cols - 1) / cols;
    const int W = cols * pw + (cols + 1) * gap;
    const int H = rows * ph + (rows + 1) * gap;
    std::vector<uint8_t> out((size_t)W * (size_t)H * 3, 24);
    for (size_t i = 0; i < panels.size(); i++) {
        const Panel& p = panels[i];
        const int ox = gap + (int)(i % (size_t)cols) * (pw + gap);
        const int oy = gap + (int)(i / (size_t)cols) * (ph + gap);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                const size_t s = ((size_t)y * (size_t)pw + (size_t)x) * 3;
                const size_t d = ((size_t)(oy + y) * (size_t)W + (size_t)(ox + x)) * 3;
                out[d + 0] = p.rgb[s + 0];
                out[d + 1] = p.rgb[s + 1];
                out[d + 2] = p.rgb[s + 2];
            }
        // 左上角标签：黑底白字
        const int lx = ox + 4, ly = oy + 4;
        const int labelW = (int)p.label.size() * 4 * 3 + 2;
        const int labelH = 5 * 3 + 2;
        for (int y = -2; y < labelH; y++)
            for (int x = -2; x < labelW; x++) {
                const int px = lx + x, py = ly + y;
                if (px < 0 || px >= W || py < 0 || py >= H) continue;
                const size_t o = ((size_t)py * (size_t)W + (size_t)px) * 3;
                out[o + 0] = 0;
                out[o + 1] = 0;
                out[o + 2] = 0;
            }
        drawGlyphRun(out, W, H, lx, ly, p.label.c_str(), 3, 255, 255, 255);
    }
    return saveRGB(path, out, W, H);
}

int countEmissive(const Layer& L) {
    int n = 0;
    for (const Material& m : L.material)
        if (m.emR || m.emG || m.emB) n++;
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    setupConsoleUtf8();
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--scene") { const char* v = next(); if (!v) return 2; g_scene = v; }
        else if (a == "--size") { const char* v = next(); if (!v || !parseSize(v, g_w, g_h)) return 2; }
        else if (a == "--radius") { const char* v = next(); if (!v) return 2; g_radius = std::max(1, atoi(v)); }
        else if (a == "--layer") { const char* v = next(); if (!v) return 2; g_layer = atoi(v); }
        else if (a == "--frames") { const char* v = next(); if (!v) return 2; g_frames = std::max(1, atoi(v)); }
        else if (a == "--ambient") { const char* v = next(); if (!v) return 2; g_ambient = (float)atof(v); }
        else if (a == "--scale") { const char* v = next(); if (!v) return 2; g_scale = std::clamp(atoi(v), 1, 8); }
        else if (a == "--exposure") { const char* v = next(); if (!v) return 2; g_exposure = (float)atof(v); }
        else if (a == "--seq") { const char* v = next(); if (!v) return 2; g_seqDir = v; }
        else if (a == "--no-glow") { g_glowView = false; }
        else if (a == "--out") { const char* v = next(); if (!v) return 2; g_outDir = v; }
        else { fprintf(stderr, "[hash] 未知参数: %s\n", a.c_str()); usage(); return 2; }
    }

    bench::SceneId sid = bench::SceneId::NeonCity;
    if (!bench::sceneByName(g_scene, sid)) {
        fprintf(stderr, "[hash] 未知场景: %s\n", g_scene.c_str());
        return 2;
    }

    Document doc(g_w, g_h);
    bench::buildDoc(doc, sid, 0);
    if (doc.width() != g_w || doc.height() != g_h) {
        fprintf(stderr, "[hash] 场景尺寸与请求不一致\n");
        return 1;
    }

    // 选层：默认挑自发光像素最多的那层（光场的来源在那儿，角向分布最尖）
    int li = g_layer;
    if (li < 0) {
        int best = 0, bestN = -1;
        for (int i = 0; i < doc.layerCount(); i++) {
            const int n = countEmissive(doc.layer(i));
            if (n > bestN) { bestN = n; best = i; }
        }
        li = best;
    }
    if (li < 0 || li >= doc.layerCount()) {
        fprintf(stderr, "[hash] 层号越界（共 %d 层）\n", doc.layerCount());
        return 2;
    }
    const Layer& L = doc.layer(li);

    const std::vector<Dir> dirs = buildDirs(g_radius);
    const int D = (int)dirs.size();
    const int frames = (g_frames > 0) ? g_frames : D;
    const size_t fieldBytes = (size_t)D * (size_t)g_w * (size_t)g_h * 3 * sizeof(float);

    printf("[hash] 场景 %s，层 %d（自发光 %d 像素），%dx%d\n", g_scene.c_str(), li,
           countEmissive(L), g_w, g_h);
    printf("[hash] 方向数 D=%d（半径 %d），物化缓冲 %.1f MB，累积到 %d 帧\n", D, g_radius,
           (double)fieldBytes / (1024.0 * 1024.0), frames);

    // --- 物化 ---
    std::vector<float> field;
    double steps = 0;
    materialize(L, dirs, g_ambient, g_emitScale, g_propagate, field, steps);
    const double px = (double)g_w * (double)g_h;
    printf("[hash] 物化完成：扫描 %0.1f M 步（每像素 %.1f 步/方向）\n", steps / 1e6,
           steps / (px * (double)D));

    // --- 参考解：确定性求和 ---
    const int np = g_w * g_h;
    std::vector<float> ref((size_t)np * 3, 0.0f);
    for (int d = 0; d < D; d++) {
        const float* f = field.data() + (size_t)d * (size_t)np * 3;
        for (int i = 0; i < np * 3; i++) ref[(size_t)i] += f[(size_t)i];
    }
    double refMean = 0;
    for (float v : ref) refMean += v;
    refMean /= (double)ref.size();

    // --- 只含环境光的场（**只影响显示**，误差指标仍按原始 F 算）---
    std::vector<float> refAmb((size_t)np * 3, 0.0f);
    if (g_glowView) {
        std::vector<float> ambField;
        double dummy = 0;
        materialize(L, dirs, g_ambient, 0.0f, g_propagate, ambField, dummy);
        for (int d = 0; d < D; d++) {
            const float* f = ambField.data() + (size_t)d * (size_t)np * 3;
            for (int i = 0; i < np * 3; i++) refAmb[(size_t)i] += f[(size_t)i];
        }
    }
    auto displayOf = [&](const std::vector<float>& src) {
        std::vector<float> out(src.size());
        for (size_t i = 0; i < src.size(); i++)
            out[i] = g_glowView ? std::max(0.0f, src[i] - refAmb[i]) : src[i];
        return out;
    };
    const std::vector<float> dispRef = displayOf(ref);
    const float exposure = (g_exposure > 0.0f) ? g_exposure : autoExposure(dispRef);
    if (g_glowView) {
        double mAmb = 0, mDisp = 0;
        for (size_t i = 0; i < refAmb.size(); i++) mAmb += refAmb[i];
        for (size_t i = 0; i < dispRef.size(); i++) mDisp += dispRef[i];
        printf("[hash] 环境光基线均值 %.4f，减掉后的发光均值 %.4f（原始 %.4f）\n",
               mAmb / (double)refAmb.size(), mDisp / (double)dispRef.size(), refMean);
    }
    printf("[hash] 参考解（Σ_d/D）平均亮度 %.4f，曝光 %.2f×（gamma 0.5）%s\n", refMean,
           (double)exposure, g_glowView ? "，显示已减去环境光基线" : "");

    // --- 每帧的实际采样成本：gather 从边界走到像素需要的步数 ---
    // 对 x-major 方向，x 每步前进 1 ⇒ 起点到 p 的步数 = |p.x - 边界|
    double gatherSteps = 0;
    for (int y = 0; y < g_h; y++)
        for (int x = 0; x < g_w; x++) {
            const int d = pickDir(Scheme::HashRotate, x, y, 0, D);
            const int ux = dirs[(size_t)d].dx, uy = dirs[(size_t)d].dy;
            const int dx = std::abs(ux), dy = std::abs(uy);
            const int t = (dx >= dy) ? ((ux >= 0) ? x : (g_w - 1 - x))
                                     : ((uy >= 0) ? y : (g_h - 1 - y));
            gatherSteps += (double)t;
        }
    gatherSteps /= px;
    printf("[hash] 每帧每像素 gather 平均 %.1f 步（确定性扫描是 %d 步/像素）⇒ 单帧成本 %.2f×\n",
           gatherSteps, D, gatherSteps / (double)D);

    // --- 几种方案逐帧累积 ---
    std::vector<int> cps = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    if (frames > 0) cps.push_back(frames);
    cps.push_back(D);   // 第 D 帧 = 掩码轮换的"精确"点
    for (int& v : cps) v = std::min(v, frames);
    std::sort(cps.begin(), cps.end());
    cps.erase(std::unique(cps.begin(), cps.end()), cps.end());

    const Scheme schemes[4] = {Scheme::HashRotate, Scheme::SpreadRotate, Scheme::RandomFrame,
                               Scheme::FixedRandom};

    // 拼图要看的那几帧（小 N：这是"去噪能不能救回来"的关键区间）
    std::vector<int> panelNs = {1, 2, 4, 8};
    for (int& v : panelNs) v = std::min(v, frames);
    std::sort(panelNs.begin(), panelNs.end());
    panelNs.erase(std::unique(panelNs.begin(), panelNs.end()), panelNs.end());
    std::vector<Panel> rawPanels, denPanels, denErrPanels;

    // 去噪的引导图：覆盖率（画布 alpha）。它没有噪声，是理想的边缘向导。
    std::vector<float> guide((size_t)np, 0.0f);
    for (int y = 0; y < g_h; y++)
        for (int x = 0; x < g_w; x++)
            guide[(size_t)y * (size_t)g_w + (size_t)x] = L.albedo.at(x, y).a / 255.0f;

    printf("\n%-13s %5s %9s %10s %12s %10s %10s\n", "方案", "帧", "光场RMSE", "显示级",
           "显示>2级%", "最大误差", "单帧成本");
    printf("------------------------------------------------------------------------------\n");

    for (Scheme sc : schemes) {
        std::vector<float> acc((size_t)np * 3, 0.0f);
        std::vector<float> acc2((size_t)np * 3, 0.0f);   // 平方和：用来估逐像素噪声
        std::vector<float> est((size_t)np * 3, 0.0f);
        size_t nextCp = 0;
        for (int f = 0; f < frames; f++) {
            for (int y = 0; y < g_h; y++)
                for (int x = 0; x < g_w; x++) {
                    const int d = pickDir(sc, x, y, f, D);
                    const float* src = field.data() + (size_t)d * (size_t)np * 3;
                    const size_t o = ((size_t)y * (size_t)g_w + (size_t)x) * 3;
                    acc[o + 0] += src[o + 0];
                    acc[o + 1] += src[o + 1];
                    acc[o + 2] += src[o + 2];
                    acc2[o + 0] += src[o + 0] * src[o + 0];
                    acc2[o + 1] += src[o + 1] * src[o + 1];
                    acc2[o + 2] += src[o + 2] * src[o + 2];
                }
            const int n = f + 1;
            if (nextCp < cps.size() && n == cps[nextCp]) {
                // ⚠️ 每帧只采 1 个方向，而参考解是 D 个方向的平均：
                // 物化时已经把 1/D 折进 field 里了，所以从 N 个样本估计 F 必须乘 D/N。
                // （写成 1/N 会让结果小了整整 D 倍 —— 这个 bug 一开始就踩了，
                //   表现是"误差曲线完全平"，因为 est 恒等于 F/D。）
                const float inv = (float)D / (float)n;
                for (size_t i = 0; i < est.size(); i++) est[i] = acc[i] * inv;
                const ErrStat s = compare(L, ref, est, g_ambient, g_w, g_h);
                printf("%-13s %5d %9.5f %10.2f %12.1f %10.5f %10.2f\n", schemeName(sc), n,
                       s.rmse, s.dispLevels, s.dispBadPct, s.maxAbs, gatherSteps / (double)D);
                if (sc == Scheme::SpreadRotate) {
                    const bool wantPanel =
                        std::binary_search(panelNs.begin(), panelNs.end(), n);
                    // 逐像素噪声标准差：est = (D/N)Σs ⇒ σ_est = (D/N)·sqrt(Var(s)/N)，
                    // Var(s) = Σs²/N − (Σs/N)²
                    std::vector<float> sigma((size_t)np, 0.0f);
                    {
                        const double fn = (double)n;
                        const double scale = (double)D / fn;
                        for (int i = 0; i < np; i++) {
                            double v = 0;
                            for (int c = 0; c < 3; c++) {
                                const size_t o = (size_t)i * 3 + (size_t)c;
                                const double mean = (double)acc[o] / fn;
                                double var = (double)acc2[o] / fn - mean * mean;
                                if (var < 0) var = 0;
                                v = std::max(v, scale * std::sqrt(var / fn));
                            }
                            sigma[(size_t)i] = (float)v;
                        }
                    }
                    if (wantPanel) {
                        // ① 自适应 k = k0/√N（亮度缩放版）
                        for (float k0 : {0.4f, 0.8f, 1.2f}) {
                            const float k = k0 / std::sqrt((float)n);
                            const std::vector<float> dn =
                                atrousDenoise(est, guide, g_w, g_h, 3, 0.08f, k, 0.02f);
                            const ErrStat sd = compare(L, ref, dn, g_ambient, g_w, g_h);
                            printf("    luma  N=%-3d k=%.2f(=%.2f/√N) RMSE %7.5f  显示 %6.2f\n", n,
                                   (double)k, (double)k0, sd.rmse, sd.dispLevels);
                        }
                        // ② 逐像素方差引导（SVGF 思路）
                        for (float c : {2.0f, 3.5f, 5.0f}) {
                            const std::vector<float> dn = atrousDenoiseVar(
                                est, guide, sigma, g_w, g_h, 3, 0.08f, c, 1e-3f);
                            const ErrStat sd = compare(L, ref, dn, g_ambient, g_w, g_h);
                            printf("    sigma N=%-3d c=%.1f             RMSE %7.5f  显示 %6.2f\n", n,
                                   (double)c, sd.rmse, sd.dispLevels);
                        }
                        // 面板：方差引导 c=3.5（理论上最该保住 1px 结构）
                        const std::vector<float> den = atrousDenoiseVar(
                            est, guide, sigma, g_w, g_h, 3, 0.08f, 3.5f, 1e-3f);

                        Panel pr;
                        pr.label = "N" + std::to_string(n);
                        pr.w = g_w * g_scale;
                        pr.h = g_h * g_scale;
                        pr.rgb = toneImage(displayOf(est), g_w, g_h, exposure, g_scale);
                        rawPanels.push_back(std::move(pr));

                        Panel pd = pr;
                        pd.label = "D" + std::to_string(n);
                        pd.rgb = toneImage(displayOf(den), g_w, g_h, exposure, g_scale);
                        denPanels.push_back(std::move(pd));

                        Panel pe = pr;
                        pe.label = "E" + std::to_string(n);
                        pe.rgb = errorImage(ref, den, g_w, g_h, g_scale);
                        denErrPanels.push_back(std::move(pe));
                    }
                }
                nextCp++;
            }
        }
        printf("\n");
    }

    // 参考图与拼图
    {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/hash_ref.png", g_outDir.c_str());
        saveRGB(path, toneImage(dispRef, g_w, g_h, exposure, g_scale), g_w * g_scale,
                g_h * g_scale);
        printf("[hash] 参考图 -> %s\n", path);

        std::snprintf(path, sizeof(path), "%s/hash_montage.png", g_outDir.c_str());
        std::vector<Panel> all = rawPanels;
        for (Panel& p : denPanels) all.push_back(std::move(p));
        for (Panel& p : denErrPanels) all.push_back(std::move(p));
        if (writeMontage(path, all, (int)rawPanels.size()))
            printf("[hash] 拼图（N=原始 / D=去噪 / E=去噪后误差）-> %s\n", path);
    }
    return 0;
}
