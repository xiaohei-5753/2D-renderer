// render/voxel_renderer.cpp — +z 面的 3D 体素光线追踪
//
// ## 主射线
//
//   正交俯视：像素 (x,y) 从 (x+.5, y+.5, +∞) 沿 (0,0,-1) 射下，打到最高的实体
//   体素，取它的 **+z 面**（法线 n = (0,0,1)）。命中点 p = (x+.5, y+.5, h+1)。
//
// ## 着色：重点在**自发光体素互相照亮**
//
//   每个发光体素都当成一个面光源（面积 = 一格，辐射 E）。对 p 的辐照度：
//
//       I(p) = Σ_q  E(q)·cosθ_q / r_q² · vis(p,q)
//
//   直接求和是 O(像素 × 光源)，所以走标准的**光源采样**：
//   按功率建 CDF，每帧每像素随机抽 K 个光源，用蒙特卡洛估计量
//
//       I ≈ (1/K) Σ_k  E(q_k)·cosθ/r² · vis / pdf(q_k)
//
//   配合**渐进累积**：帧间平均，噪点随时间收敛。
//
//   太阳 + 天空保留成一个解析项（不做重点）。
//
// ## 3D 光追的加速手段（都已就位或可加）
//
//   * 阴影射线 = Amanatides-Woo 3D 体素 DDA
//   * 空跳：粗占据网格（brick map），空格一次跨过
//   * 光源表按功率排序 + CDF 二分，O(log N) 抽样
//   * 可继续加：光源 BVH、SVO、阴影贴图
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

// xorshift：线程内确定性随机（同一帧同一像素结果稳定，便于比对）
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

// 粗占据网格：xy 每 8 格一块，z 每层一块（BZ 必须 = 1，
// 否则相邻层的实体会把射线所在层的块也标成非空，空跳就失效）
constexpr int BXY = 8;

}  // namespace

class VoxelTracer final : public VoxelRenderer {
public:
    bool setVolume(const VoxelVolume& v) override {
        vol_ = v;
        const int X = v.X, Y = v.Y, Z = v.Z;

        // 列高缓存：主射线是垂直的
        top_.assign((size_t)X * (size_t)Y, -1);
        for (int y = 0; y < Y; y++)
            for (int x = 0; x < X; x++)
                top_[(size_t)y * (size_t)X + (size_t)x] = v.columnTop(x, y);

        // 粗占据（空跳用）
        bx_ = (X + BXY - 1) / BXY;
        by_ = (Y + BXY - 1) / BXY;
        occ_.assign((size_t)bx_ * (size_t)by_ * (size_t)Z, 0);
        for (int z = 0; z < Z; z++)
            for (int y = 0; y < Y; y++)
                for (int x = 0; x < X; x++)
                    if (v.solid(x, y, z))
                        occ_[((size_t)z * (size_t)by_ + (size_t)(y / BXY)) * (size_t)bx_ +
                             (size_t)(x / BXY)] = 1;

        // ---- 光源表：E>0 的体素，按功率建 CDF ----
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
        if (getenv("PB_VOXDBG"))
            fprintf(stderr, "[vox] 体素 %dx%dx%d  实体光源 %zu  总功率 %.1f\\n", X, Y, Z,
                    lights_.size(), totalPower_);
        resetAccumulation();
        return true;
    }

    void setSettings(const RenderSettings& s) override {
        const bool lightChanged = (s.emissiveLights != set_.emissiveLights) ||
                                  (s.lightSamples != set_.lightSamples) ||
                                  (s.emissiveIntensity != set_.emissiveIntensity);
        const bool sunChanged = std::memcmp(s.sunDir, set_.sunDir, sizeof(s.sunDir)) != 0 ||
                                s.sunIntensity != set_.sunIntensity ||
                                s.shadows != set_.shadows ||
                                std::memcmp(s.skyColor, set_.skyColor, sizeof(s.skyColor)) != 0;
        set_ = s;
        if (lightChanged || sunChanged) resetAccumulation();
    }

    void resetAccumulation() override {
        accum_.clear();
        frames_ = 0;
    }

    int accumFrames() const override { return frames_; }

    bool converged() const override {
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
        if (set_.progressive && frames_ >= set_.maxAccumFrames) {
            // 已收敛：直接用现有累积出图，不再采样
            toImage(out);
            lastMs_ = 0.0;
            return;
        }

        // 主光方向
        float L[3] = {set_.sunDir[0], set_.sunDir[1], set_.sunDir[2]};
        const float ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (ln > 1e-6f) { L[0] /= ln; L[1] /= ln; L[2] /= ln; }
        const float ndl = std::max(0.0f, L[2]);
        const int limit = (X + Y + vol_.Z) * 2 + 8;

        const int K = std::max(1, set_.lightSamples);
        const bool useEm = set_.emissiveLights && !lights_.empty() && totalPower_ > 0.0f;
        const uint32_t frameSeed = (uint32_t)(frames_ * 2654435761u + 12345u);

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

                    // ---- 太阳 + 天空（解析项）----
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

                    // ---- 自发光体素当光源：蒙特卡洛 ----
                    if (useEm) {
                        Rng rng(frameSeed ^ ((uint32_t)y * 73856093u) ^
                                ((uint32_t)x * 19349663u));
                        float acc[3] = {0, 0, 0};
                        int good = 0;
                        for (int k = 0; k < K; k++) {
                            const float u = rng.f01() * totalPower_;
                            // CDF 二分
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
                            const float cosT = d[2];            // n = (0,0,1)
                            if (cosT <= 0.0f) continue;         // 只取上半球
                            // pdf = 该光源功率 / 总功率
                            const float pw =
                                q.er + q.eg + q.eb;
                            if (pw <= 0.0f) continue;
                            const float pdf = pw / totalPower_;
                            float tr = 1.0f;
                            // 只走到光源**之前**。光源是一整格方块，
                            // 沿任意方向的近面最近也在 r-0.87（半对角线），
                            // 所以留 1.0 的余量，否则射线会撞进光源自己的体积、
                            // 把每个光源都判成被遮挡 —— 自发光就一点都照不出去。
                            if (set_.shadows && traceShadow(p, d, tr, limit, r - 1.0f))
                                continue;
                            const float w = cosT / r2 / pdf * set_.emissiveIntensity * tr;
                            acc[0] += q.er * w;
                            acc[1] += q.eg * w;
                            acc[2] += q.eb * w;
                            good++;
                        }
                        if (good > 0) {
                            const float inv = 1.0f / (float)K;   // 注意：按 K 平均（未命中的算 0）
                            for (int c = 0; c < 3; c++) lit[c] += acc[c] * inv;
                        }
                    }

                    // ---- 合成：E + C·A·(光照) ----
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
        if (getenv("PB_VOXDBG") && frames_ == 1) {
            float mx = 0.0f;
            for (float v : accum_) mx = std::max(mx, v);
            fprintf(stderr, "[vox] 第1帧累积最大值 %.4f  K=%d  useEm=%d\\n", mx,
                    std::max(1, set_.lightSamples),
                    (set_.emissiveLights && !lights_.empty()) ? 1 : 0);
        }
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

    // 阴影射线：从 p 沿 d 走。被挡住返回 true；tr 是透射率（半透明衰减）。
    //
    // maxT：只走到这个距离为止。做面光源采样时**必须**传 r-ε ——
    // 否则射线会先撞进光源自己的体素（A=255），把每一个光源都判成"被遮挡"，
    // 自发光就一点都照不出去。
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

            if (tHit > maxT) return false;      // 到光源之前没被挡
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
    std::vector<int> top_;               // 每列的柱高
    std::vector<uint8_t> occ_;           // 粗占据：空跳用（已建，阴影射线里可加）
    int bx_ = 0, by_ = 0;
    std::vector<Light> lights_;          // 发光体素
    std::vector<float> cdf_;             // 功率累积分布
    float totalPower_ = 0.0f;
    std::vector<float> accum_;           // 渐进累积（X*Y*3）
    int frames_ = 0;
    double lastMs_ = 0.0;
};

std::unique_ptr<VoxelRenderer> createVoxelRenderer() {
    return std::make_unique<VoxelTracer>();
}

}  // namespace pb::render
