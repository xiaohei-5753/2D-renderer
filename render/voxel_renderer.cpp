// render/voxel_renderer.cpp — +z 面的 3D 体素光线追踪
//
// 主射线（所有模式共用）
//   正交俯视：像素 (x,y) 沿 (0,0,-1) 射下，打到每列最高的实体体素，
//   取它的 **+z 面**（法线 n = (0,0,1)），命中点 p = (x+.5, y+.5, h+1)。
//
// 全局光照有两种求法，可用设置切换、直接 A/B：
//
//   Gi::ConeTrace（默认，VCT）
//     把体素化成 3D 辐照度网格（rgb = 自发光 E，a = 不透明度 A），
//     建 3D **mip 金字塔**，然后从 p 沿上半球打 N 个**锥**：
//         acc += T · sample.rgb · sample.a
//         T   *= (1 - sample.a)
//     锥越远越粗，正好采样对应层级的 mip（footprint ≈ t·tanα）。
//     好处：**无噪声**、天然多尺度、远处自动用粗层 —— 不用再堆样本。
//
//   Gi::LightSampling（上一版，留作对照）
//     把每个 E>0 的体素当面光源，按功率建 CDF 做蒙特卡洛 + 渐进累积。
//     方差大，要累积很多帧。
//
// 太阳那一条阴影用 Amanatides-Woo 3D 体素 DDA。
#include "render/voxel_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace pb::render {
namespace {

inline float u8f(uint8_t v) { return (float)v / 255.0f; }

// xorshift：线程内确定性随机
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float f01() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
};

// 一层 mip：4 float / 体素（rgb + a）
struct MipLevel {
    int X = 0, Y = 0, Z = 0;
    std::vector<float> v;
    void init(int x, int y, int z) {
        X = std::max(1, x);
        Y = std::max(1, y);
        Z = std::max(1, z);
        v.assign((size_t)X * (size_t)Y * (size_t)Z * 4u, 0.0f);
    }
    size_t idx(int x, int y, int z) const {
        return (((size_t)z * (size_t)Y + (size_t)y) * (size_t)X + (size_t)x) * 4u;
    }
};

}  // namespace

class VoxelTracer final : public VoxelRenderer {
public:
    bool setVolume(const VoxelVolume& v) override {
        vol_ = v;
        const int X = v.X, Y = v.Y, Z = v.Z;

        // 列高缓存（主射线是垂直的）
        top_.assign((size_t)X * (size_t)Y, -1);
        for (int y = 0; y < Y; y++)
            for (int x = 0; x < X; x++)
                top_[(size_t)y * (size_t)X + (size_t)x] = v.columnTop(x, y);

        // ---- 光源表（Gi::LightSampling 用）----
        lights_.clear();
        cdf_.clear();
        totalPower_ = 0.0f;
        for (int z = 0; z < Z; z++)
            for (int y = 0; y < Y; y++)
                for (int x = 0; x < X; x++) {
                    const RGBA8 e = v.emission[v.idx(x, y, z)];
                    const float p = u8f(e.r) + u8f(e.g) + u8f(e.b);
                    if (p <= 0.0f) continue;
                    lights_.push_back(Light{x, y, z, u8f(e.r), u8f(e.g), u8f(e.b)});
                    totalPower_ += p;
                    cdf_.push_back(totalPower_);
                }

        buildMips();
        resetAccumulation();
        if (getenv("PB_VOXDBG"))
            fprintf(stderr, "[vox] 体素 %dx%dx%d  光源 %zu  总功率 %.1f  mip 层数 %zu\n", X, Y, Z,
                    lights_.size(), totalPower_, mips_.size());
        return true;
    }

    void setSettings(const RenderSettings& s) override {
        const bool diff = s.gi != set_.gi || s.lightSamples != set_.lightSamples ||
                          s.emissiveIntensity != set_.emissiveIntensity ||
                          s.cones != set_.cones || s.coneMaxDist != set_.coneMaxDist ||
                          s.giIntensity != set_.giIntensity ||
                          std::memcmp(s.sunDir, set_.sunDir, sizeof(s.sunDir)) != 0 ||
                          s.sunIntensity != set_.sunIntensity || s.shadows != set_.shadows ||
                          std::memcmp(s.skyColor, set_.skyColor, sizeof(s.skyColor)) != 0;
        set_ = s;
        if (diff) resetAccumulation();
        // emitScale 会进 mip 第 0 层，变了要重建
        if (s.emitScale != emitScaleUsed_) buildMips();
    }

    void resetAccumulation() override {
        accum_.clear();
        frames_ = 0;
    }
    int accumFrames() const override { return frames_; }
    bool converged() const override {
        // 锥追踪是确定性的：一帧就是最终结果
        if (set_.gi == RenderSettings::Gi::ConeTrace) return frames_ >= 1;
        return !set_.progressive || frames_ >= set_.maxAccumFrames;
    }

    void render(std::vector<RGBA8>& out) override {
        const auto t0 = std::chrono::steady_clock::now();
        const int X = vol_.X, Y = vol_.Y;
        out.assign((size_t)std::max(0, X) * (size_t)std::max(0, Y), RGBA8{0, 0, 0, 255});
        if (vol_.empty()) return;

        const size_t n = (size_t)X * (size_t)Y;
        if (accum_.size() != n * 3u) {
            accum_.assign(n * 3u, 0.0f);
            frames_ = 0;
        }
        if (converged()) {
            toImage(out);
            lastMs_ = 0.0;
            return;
        }

        float L[3] = {set_.sunDir[0], set_.sunDir[1], set_.sunDir[2]};
        const float ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (ln > 1e-6f) { L[0] /= ln; L[1] /= ln; L[2] /= ln; }
        const float ndl = std::max(0.0f, L[2]);
        const int limit = (X + Y + vol_.Z) * 2 + 8;

        const int K = std::max(1, set_.lightSamples);
        const bool useEm = set_.gi == RenderSettings::Gi::LightSampling &&
                           !lights_.empty() && totalPower_ > 0.0f;
        const bool useVct = set_.gi == RenderSettings::Gi::ConeTrace;
        const uint32_t frameSeed = (uint32_t)(frames_ * 2654435761u + 12345u);

        // 锥方向：上半球斐波那契（cosθ = 1-(i+.5)/N ∈ (0,1]）
        std::vector<float> coneDir;
        float coneTan = 0.0f;
        if (useVct) {
            const int N = std::max(1, set_.cones);
            coneDir.reserve((size_t)N * 3u);
            const float ga = 2.39996323f;
            for (int i = 0; i < N; i++) {
                const float cz = 1.0f - ((float)i + 0.5f) / (float)N;
                const float sz = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
                const float phi = (float)i * ga;
                coneDir.push_back(sz * std::cos(phi));
                coneDir.push_back(sz * std::sin(phi));
                coneDir.push_back(cz);
            }
            // N 个锥大致铺满半球：每个占 2π/N 立体角
            const float omega = 2.0f * 3.14159265f / (float)N;
            const float cosA = std::max(-1.0f, 1.0f - omega / (2.0f * 3.14159265f));
            coneTan = std::tan(std::acos(std::min(1.0f, cosA)));
        }

        auto rows = [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                for (int x = 0; x < X; x++) {
                    const int h = top_[(size_t)y * (size_t)X + (size_t)x];
                    if (h < 0) continue;
                    const size_t ci = vol_.idx(x, y, h);
                    const RGBA8 C = vol_.color[ci];
                    const RGBA8 E = vol_.emission[ci];
                    const float A = VoxelVolume::a01(C);
                    const float p[3] = {(float)x + 0.5f, (float)y + 0.5f, (float)(h + 1)};

                    // 太阳 + 天空（解析项）
                    float vis = 1.0f;
                    if (set_.shadows && ndl > 0.0f) {
                        float tr = 1.0f;
                        vis = traceShadow(p, L, tr, limit) ? 0.0f : tr;
                    }
                    float lit[3];
                    for (int c = 0; c < 3; c++) {
                        const float sc = (c == 0 ? set_.sunColor[0]
                                                 : (c == 1 ? set_.sunColor[1] : set_.sunColor[2]));
                        const float kc = (c == 0 ? set_.skyColor[0]
                                                 : (c == 1 ? set_.skyColor[1] : set_.skyColor[2]));
                        lit[c] = sc * set_.sunIntensity * ndl * vis + kc;
                    }

                    // ---- 体素锥追踪 ----
                    if (useVct && !mips_.empty()) {

                        const int N = (int)(coneDir.size() / 3u);
                        // 半球辐照度积分： I = Σ_c L_c · cosθ_c · Ω_c
                        //   Ω_c = 2π/N（每个锥占的立体角），cosθ_c = 锥方向的 z。
                        // 我第一版只写了 Σ L_c/N —— 漏了 cosθ 和 2π，
                        // 亮度就低了 2π 倍（约 6.3 倍），所以看着发暗。
                        const float omega = (N > 0) ? 2.0f * 3.14159265f / (float)N : 0.0f;
                        for (int i = 0; i < N; i++) {
                            const float d[3] = {coneDir[(size_t)i * 3u],
                                                coneDir[(size_t)i * 3u + 1],
                                                coneDir[(size_t)i * 3u + 2]};
                            float acc[3] = {0, 0, 0};
                            traceCone(p, d, coneTan, acc);
                            const float w = d[2] * omega * set_.giIntensity;
                            for (int c = 0; c < 3; c++) lit[c] += acc[c] * w;
                        }
                    }

                    // ---- 光源采样（对照）----
                    if (useEm) {
                        Rng rng(frameSeed ^ ((uint32_t)y * 73856093u) ^
                                ((uint32_t)x * 19349663u));
                        float acc[3] = {0, 0, 0};
                        int good = 0;
                        for (int k = 0; k < K; k++) {
                            const float u = rng.f01() * totalPower_;
                            const size_t li =
                                (size_t)(std::lower_bound(cdf_.begin(), cdf_.end(), u) -
                                         cdf_.begin());
                            const Light& q = lights_[std::min(li, lights_.size() - 1)];
                            const float qc[3] = {(float)q.x + 0.5f, (float)q.y + 0.5f,
                                                 (float)q.z + 0.5f};
                            float d[3] = {qc[0] - p[0], qc[1] - p[1], qc[2] - p[2]};
                            const float r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                            if (r2 < 1e-6f) continue;
                            const float r = std::sqrt(r2);
                            d[0] /= r; d[1] /= r; d[2] /= r;
                            const float cosT = d[2];
                            if (cosT <= 0.0f) continue;
                            const float pw = q.er + q.eg + q.eb;
                            if (pw <= 0.0f) continue;
                            const float pdf = pw / totalPower_;
                            float tr = 1.0f;
                            if (set_.shadows && traceShadow(p, d, tr, limit, r - 1.0f)) continue;
                            const float w = cosT / r2 / pdf * set_.emissiveIntensity * tr;
                            acc[0] += q.er * w;
                            acc[1] += q.eg * w;
                            acc[2] += q.eb * w;
                            good++;
                        }
                        if (good > 0) {
                            const float inv = 1.0f / (float)K;
                            for (int c = 0; c < 3; c++) lit[c] += acc[c] * inv;
                        }
                    }

                    const size_t ai = ((size_t)y * (size_t)X + (size_t)x) * 3u;
                    for (int c = 0; c < 3; c++) {
                        const float Cs = u8f(c == 0 ? C.r : (c == 1 ? C.g : C.b));
                        const float Es = u8f(c == 0 ? E.r : (c == 1 ? E.g : E.b));
                        accum_[ai + (size_t)c] += Es * set_.emitScale + Cs * A * lit[c];
                    }
                }
            }
        };

        const int hw = (int)std::thread::hardware_concurrency();
        const int nt = std::max(1, std::min(hw > 0 ? hw : 4, Y));
        if (nt <= 1) {
            rows(0, Y);
        } else {
            std::vector<std::thread> th;
            const int chunk = (Y + nt - 1) / nt;
            for (int i = 0; i < nt; i++) {
                const int a = i * chunk, b = std::min(Y, a + chunk);
                if (a >= b) break;
                th.emplace_back(rows, a, b);
            }
            for (auto& t : th) t.join();
        }

        frames_++;
        toImage(out);
        lastMs_ = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
    }

    double lastMs() const override { return lastMs_; }

private:
    struct Light {
        int x, y, z;
        float er, eg, eb;
    };

    // ---- 体素化 + mip 金字塔 ----
    void buildMips() {
        mips_.clear();
        const int X = vol_.X, Y = vol_.Y, Z = vol_.Z;
        if (X <= 0 || Y <= 0 || Z <= 0) return;
        emitScaleUsed_ = set_.emitScale;

        MipLevel l0;
        l0.init(X, Y, Z);
        for (int z = 0; z < Z; z++)
            for (int y = 0; y < Y; y++)
                for (int x = 0; x < X; x++) {
                    const size_t si = vol_.idx(x, y, z);
                    const RGBA8 c = vol_.color[si];
                    const RGBA8 e = vol_.emission[si];
                    const float A = VoxelVolume::a01(c);
                    float* d = &l0.v[l0.idx(x, y, z)];
                    // rgb = 自发光（辐射），a = 不透明度
                    d[0] = u8f(e.r) * emitScaleUsed_;
                    d[1] = u8f(e.g) * emitScaleUsed_;
                    d[2] = u8f(e.b) * emitScaleUsed_;
                    d[3] = A;
                }
        mips_.push_back(std::move(l0));

        // 2×2×2 盒式下采样，直到 1×1×1
        while (mips_.back().X > 1 || mips_.back().Y > 1 || mips_.back().Z > 1) {
            const MipLevel& s = mips_.back();
            MipLevel d;
            d.init((s.X + 1) / 2, (s.Y + 1) / 2, (s.Z + 1) / 2);
            for (int z = 0; z < d.Z; z++)
                for (int y = 0; y < d.Y; y++)
                    for (int x = 0; x < d.X; x++) {
                        // 下采样必须是**按不透明度加权**的平均辐射：
                        // 直接盒式平均会把自发光和空气一起平均，粗层里辐射被稀释，
                        // 于是远处几乎收不到光（实测光晕衰减快得离谱）。
                        // 标准 VCT 的存法：rgb = Σ(rgb·a)/Σa（辐射不被空格稀释），
                        // a = 平均不透明度（负责表示覆盖率/遮挡）。
                        float sumA = 0.0f, sumRgb[3] = {0, 0, 0};
                        int cnt = 0;
                        for (int dz = 0; dz < 2; dz++)
                            for (int dy = 0; dy < 2; dy++)
                                for (int dx = 0; dx < 2; dx++) {
                                    const int sx = x * 2 + dx, sy = y * 2 + dy,
                                              sz = z * 2 + dz;
                                    if (sx >= s.X || sy >= s.Y || sz >= s.Z) continue;
                                    const float* pp = &s.v[s.idx(sx, sy, sz)];
                                    sumA += pp[3];
                                    for (int c = 0; c < 3; c++) sumRgb[c] += pp[c] * pp[3];
                                    cnt++;
                                }
                        if (cnt <= 0) cnt = 1;
                        float* dp = &d.v[d.idx(x, y, z)];
                        for (int c = 0; c < 3; c++)
                            dp[c] = (sumA > 1e-6f) ? (sumRgb[c] / sumA) : 0.0f;
                        dp[3] = sumA / (float)cnt;
                    }
            mips_.push_back(std::move(d));
        }
    }

    // 在某一层做三线性采样
    void sampleMip(int level, float x, float y, float z, float out[4]) const {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        if (level < 0 || level >= (int)mips_.size()) return;
        const MipLevel& m = mips_[(size_t)level];
        const float inv = 1.0f / (float)(1 << level);   // 该层体素尺寸 = 2^level
        const float fx = x * inv - 0.5f, fy = y * inv - 0.5f, fz = z * inv - 0.5f;
        const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy), z0 = (int)std::floor(fz);
        const float tx = fx - (float)x0, ty = fy - (float)y0, tz = fz - (float)z0;
        for (int dz = 0; dz < 2; dz++)
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const int sx = x0 + dx, sy = y0 + dy, sz = z0 + dz;
                    if (sx < 0 || sy < 0 || sz < 0 || sx >= m.X || sy >= m.Y || sz >= m.Z)
                        continue;
                    const float wgt = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty) *
                                      (dz ? tz : 1.0f - tz);
                    const float* pp = &m.v[m.idx(sx, sy, sz)];
                    for (int c = 0; c < 4; c++) out[c] += pp[c] * wgt;
                }
    }

    // 沿 d 打一个半角 tanA 的锥，累积到 acc
    void traceCone(const float p[3], const float d[3], float tanA, float acc[3]) const {
        if (mips_.empty()) return;
        const int maxLevel = std::min((int)mips_.size() - 1, std::max(0, set_.giMaxMip));
        const float maxDist = std::max(1.0f, set_.coneMaxDist);
        const float mt = std::max(tanA, 1e-3f);

        float T = 1.0f;
        float t = 0.75f;                       // 跳过自己那一格
        int guard = 512;
        while (t < maxDist && T > 1e-3f && guard-- > 0) {
            const float r = std::max(0.5f, t * mt);   // 锥在距离 t 处的半径
            int level = 0;
            while (level < maxLevel && (float)(1 << (level + 1)) <= r * 2.0f) level++;
            float s[4];
            sampleMip(level, p[0] + d[0] * t, p[1] + d[1] * t, p[2] + d[2] * t, s);

            // ---- 不透明度补偿（分图层遮挡的关键）----
            //
            // mip 层 L 的一个 texel 是 2^L × 2^L × 2^L 个体素的**平均**。
            // 一层不透明板只占块里很小一部分，平均后 a 很小，
            // 于是 T *= (1-a) 几乎不掉 —— 光会从板子里漏过去，
            // 表现就是"看不到分图层的光线遮挡"。
            // 沿轴看，穿过这个 texel 等于穿过 2^L 个体素，所以累积不透明度为
            //     1 - (1-a)^(2^L)
            float a = s[3];
            if (a > 0.0f && level > 0) {
                const float k = (float)(1u << level);
                a = std::min(1.0f, 1.0f - std::pow(1.0f - a, k));
            }
            acc[0] += T * s[0] * a;
            acc[1] += T * s[1] * a;
            acc[2] += T * s[2] * a;
            T *= (1.0f - a);
            t += std::max(0.75f, r * 0.5f);
        }
    }

    // 阴影射线（Amanatides-Woo 3D DDA）
    bool traceShadow(const float p[3], const float d[3], float& tr, int limit,
                     float maxT = 1e30f) const {
        tr = 1.0f;
        int x = (int)std::floor(p[0]);
        int y = (int)std::floor(p[1]);
        int z = (int)std::floor(p[2]);
        const int sx = (d[0] > 0) ? 1 : (d[0] < 0 ? -1 : 0);
        const int sy = (d[1] > 0) ? 1 : (d[1] < 0 ? -1 : 0);
        const int sz = (d[2] > 0) ? 1 : (d[2] < 0 ? -1 : 0);
        const float inf = 1e30f;
        auto tDelta = [&](float dd) { return (dd != 0.0f) ? std::fabs(1.0f / dd) : inf; };
        auto tMax = [&](float pp, int cell, int st, float dd) {
            if (dd == 0.0f || st == 0) return inf;
            const float bound = (st > 0) ? (float)(cell + 1) : (float)cell;
            return (bound - pp) / dd;
        };
        float tMaxX = tMax(p[0], x, sx, d[0]);
        float tMaxY = tMax(p[1], y, sy, d[1]);
        float tMaxZ = tMax(p[2], z, sz, d[2]);
        const float tDx = tDelta(d[0]), tDy = tDelta(d[1]), tDz = tDelta(d[2]);
        for (int it = 0; it < limit; it++) {
            float tHit;
            if (tMaxX < tMaxY && tMaxX < tMaxZ) { tHit = tMaxX; x += sx; tMaxX += tDx; }
            else if (tMaxY < tMaxZ) { tHit = tMaxY; y += sy; tMaxY += tDy; }
            else { tHit = tMaxZ; z += sz; tMaxZ += tDz; }
            if (tHit > maxT) return false;
            if (!vol_.inBounds(x, y, z)) return false;
            const float A = VoxelVolume::a01(vol_.color[vol_.idx(x, y, z)]);
            if (A >= 1.0f) return true;
            if (A > 0.0f) {
                tr *= (1.0f - A);
                if (tr < 1e-3f) return true;
            }
        }
        return false;
    }

    void toImage(std::vector<RGBA8>& out) const {
        const int X = vol_.X, Y = vol_.Y;
        const float inv = (frames_ > 0) ? 1.0f / (float)frames_ : 1.0f;
        for (size_t i = 0; i < (size_t)X * (size_t)Y; i++) {
            float c[3];
            for (int k = 0; k < 3; k++) c[k] = accum_[i * 3u + (size_t)k] * inv;
            const float ov = std::max(c[0], std::max(c[1], c[2]));
            if (ov > 1.0f) { c[0] /= ov; c[1] /= ov; c[2] /= ov; }
            auto to8 = [](float v) {
                return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            };
            out[i] = RGBA8{to8(c[0]), to8(c[1]), to8(c[2]), 255};
        }
    }

    VoxelVolume vol_;
    RenderSettings set_;
    std::vector<int> top_;
    std::vector<Light> lights_;
    std::vector<float> cdf_;
    float totalPower_ = 0.0f;
    std::vector<MipLevel> mips_;      // 体素锥追踪的金字塔
    float emitScaleUsed_ = -1.0f;
    std::vector<float> accum_;
    int frames_ = 0;
    double lastMs_ = 0.0;
};

std::unique_ptr<VoxelRenderer> createVoxelRenderer() {
    return std::make_unique<VoxelTracer>();
}

}  // namespace pb::render
