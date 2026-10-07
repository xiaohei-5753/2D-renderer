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
        cascades_.clear();            // 几何变了，cascade 要重建
        cascadeL0_ = -1.0f;
        resetAccumulation();
        if (getenv("PB_VOXDBG"))
            fprintf(stderr, "[vox] 体素 %dx%dx%d  光源 %zu  总功率 %.1f  mip 层数 %zu\n", X, Y, Z,
                    lights_.size(), totalPower_, mips_.size());
        return true;
    }

    void setSettings(const RenderSettings& s) override {
        const bool diff = s.gi != set_.gi || s.lightSamples != set_.lightSamples ||
                          s.coneAperture != set_.coneAperture ||
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
        if (s.cascades != set_.cascades || s.cascadeBase != set_.cascadeBase ||
            s.cascadeRays != set_.cascadeRays || s.emitScale != set_.emitScale)
            cascades_.clear();
    }

    void resetAccumulation() override {
        accum_.clear();
        frames_ = 0;
    }
    int accumFrames() const override { return frames_; }
    bool converged() const override {
        // 锥追踪第一帧就能出图（确定性），但继续跑会靠"逐帧旋转锥集合 +
        // 帧间平均"把离散锥的结构性误差抹平，所以也让它参与累积。
        if (set_.gi == RenderSettings::Gi::ConeTrace)
            return !set_.progressive || frames_ >= std::max(1, set_.maxAccumFrames);
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
        const bool useRc = set_.gi == RenderSettings::Gi::RadianceCascades;
        if (useRc && cascades_.empty()) buildCascades();
        const uint32_t frameSeed = (uint32_t)(frames_ * 2654435761u + 12345u);

        // 锥方向：上半球斐波那契（cosθ = 1-(i+.5)/N ∈ (0,1]）
        std::vector<float> coneDir;
        float coneTan = 0.0f;
        if (useVct) {
            const int N = std::max(1, set_.cones);
            coneDir.reserve((size_t)N * 3u);
            const float ga = 2.39996323f;
            // 逐帧把整个锥集合转一个角度。
            //
            // 离散锥集合的积分误差是"结构性"的（相邻锥之间的覆盖不匀），
            // 光靠加锥只能压到一定程度。让它每帧转一个角度再帧间平均，
            // 残余误差就摊成噪声被抹平 —— 这是 VCT 的标准做法。
            const float frameRot = (set_.progressive && set_.maxAccumFrames > 1)
                                       ? (float)frames_ * 2.39996323f
                                       : 0.0f;
            for (int i = 0; i < N; i++) {
                const float cz = 1.0f - ((float)i + 0.5f) / (float)N;
                const float sz = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
                const float phi = (float)i * ga + frameRot;
                coneDir.push_back(sz * std::cos(phi));
                coneDir.push_back(sz * std::sin(phi));
                coneDir.push_back(cz);
            }
            // 锥半角：必须用点集的**覆盖半径**，不能用"面积等效半径"。
            //
            // 我第一版按"每个锥占 2π/N 立体角"算 α = acos(1-1/N)。
            // 那是面积等效半径；而 Fibonacci 点集的覆盖半径大约是它的 1.8 倍，
            // 于是相邻锥之间有空隙、某些方位角一个锥都覆盖不到。
            // 表现：等距圆环上 min=0、光斑状伪影。
            // （实测 N=9 时最近邻要 63°，公式只给 27°。）
            //
            // 半球上 N 个点的覆盖半径约为 2.5/√N（整球面是 2/√N）。
            const float cover = 2.5f / std::sqrt((float)N);
            const float ap = std::clamp(set_.coneAperture, 0.3f, 3.0f);
            coneTan = std::tan(std::min(1.396f, cover * ap));   // 上限 80°
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

                    // ---- Radiance Cascades ----
                    if (useRc) {
                        float rc[3] = {0, 0, 0};
                        mergeCascades(x, y, rc);
                        for (int c = 0; c < 3; c++) lit[c] += rc[c] * set_.giIntensity;
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

    // ================= Radiance Cascades =================
    //
    // 每个 cascade 只负责一段距离区间，且**角度分辨率随距离翻倍**：
    //
    //    cascade c：探测点间距 2^c 像素，射线数 K0·4^c，
    //               负责区间 [L0·2^(c-1), L0·2^c)   （c=0 从 0 开始）
    //
    // 距离翻倍 -> 半球立体角翻 4 倍 -> 射线数也翻 4 倍，所以
    // **每条射线覆盖的立体角恒定**，不会像 VCT 那样在远处出现空隙（光斑）。
    // 而探测点数掉 4 倍正好抵消：每个 cascade 的射线总数都是 N·K0。
    //
    // 我们的表面法线处处是 +z，所以每个探测点可以把该 cascade 的方向贡献
    // 预先合成一个 float3 存起来 —— 合并阶段就退化成"逐层双线性插值再相加"。

    struct Cascade {
        int spacing = 1;         // 探测点间距（像素）
        int gw = 0, gh = 0;      // 探测点网格尺寸
        int dirs = 1;            // 射线数
        float r0 = 0, r1 = 0;    // 负责的距离区间
        std::vector<float> val;  // gw*gh*3：每个探测点的辐照度贡献
    };

    void ensureCascades() {
        const int want = std::clamp(set_.cascades, 1, 12);
        const float L0 = std::max(0.5f, set_.cascadeBase);
        const int K0 = std::max(1, set_.cascadeRays);
        if ((int)cascades_.size() == want && cascadeL0_ == L0 && cascadeK0_ == K0) return;
        cascadeL0_ = L0;
        cascadeK0_ = K0;

        const int X = vol_.X, Y = vol_.Y;
        cascades_.clear();
        cascades_.resize((size_t)want);
        for (int c = 0; c < want; c++) {
            Cascade& cc = cascades_[(size_t)c];
            cc.spacing = 1 << c;
            cc.dirs = K0 * (1 << (2 * c));                  // 4^c
            cc.r0 = (c == 0) ? 0.0f : L0 * (float)(1 << (c - 1));
            cc.r1 = L0 * (float)(1 << c);
            cc.gw = (X + cc.spacing - 1) / cc.spacing;
            cc.gh = (Y + cc.spacing - 1) / cc.spacing;
            cc.val.assign((size_t)cc.gw * (size_t)cc.gh * 3u, 0.0f);
        }
    }

    void buildCascades() {
        ensureCascades();
        const int limit = (vol_.X + vol_.Y + vol_.Z) * 2 + 8;
        for (Cascade& cc : cascades_) {
            const float dOmega = 2.0f * 3.14159265f / (float)cc.dirs;
            // 方向集合：上半球斐波那契
            std::vector<float> dirs((size_t)cc.dirs * 3u);
            {
                const float ga = 2.39996323f;
                for (int i = 0; i < cc.dirs; i++) {
                    const float cz = 1.0f - ((float)i + 0.5f) / (float)cc.dirs;
                    const float sz = std::sqrt(std::max(0.0f, 1.0f - cz * cz));
                    const float phi = (float)i * ga;
                    dirs[(size_t)i * 3u + 0] = sz * std::cos(phi);
                    dirs[(size_t)i * 3u + 1] = sz * std::sin(phi);
                    dirs[(size_t)i * 3u + 2] = cz;
                }
            }
            auto doProbe = [&](int pi) {
                const int gx = pi % cc.gw, gy = pi / cc.gw;
                const int x = gx * cc.spacing, y = gy * cc.spacing;
                if (x >= vol_.X || y >= vol_.Y) return;
                const int h = top_[(size_t)y * (size_t)vol_.X + (size_t)x];
                if (h < 0) return;
                const float p[3] = {(float)x + 0.5f, (float)y + 0.5f, (float)(h + 1)};
                float acc[3] = {0, 0, 0};
                for (int i = 0; i < cc.dirs; i++) {
                    float r[3];
                    traceCascadeRay(p, &dirs[(size_t)i * 3u], cc.r0, cc.r1, r, limit);
                    const float w = dirs[(size_t)i * 3u + 2] * dOmega;   // cosθ · ΔΩ
                    acc[0] += r[0] * w;
                    acc[1] += r[1] * w;
                    acc[2] += r[2] * w;
                }
                float* d = &cc.val[(size_t)pi * 3u];
                d[0] = acc[0];
                d[1] = acc[1];
                d[2] = acc[2];
            };

            const int total = cc.gw * cc.gh;
            const int hw = (int)std::thread::hardware_concurrency();
            const int nt = std::max(1, std::min(hw > 0 ? hw : 4, total));
            if (nt <= 1) {
                for (int i = 0; i < total; i++) doProbe(i);
            } else {
                std::vector<std::thread> th;
                const int chunk = (total + nt - 1) / nt;
                for (int t = 0; t < nt; t++) {
                    const int a = t * chunk, b = std::min(total, a + chunk);
                    if (a >= b) break;
                    th.emplace_back([&doProbe, a, b]() {
                        for (int i = a; i < b; i++) doProbe(i);
                    });
                }
                for (auto& t : th) t.join();
            }
        }
    }

    // 从 p 沿 d 走到 r1：全程累积透射率（遮挡要正确），
    // 但**只在本 cascade 负责的区间 [r0, r1) 累积辐射**。
    void traceCascadeRay(const float p[3], const float d[3], float r0, float r1, float out[3],
                         int limit) const {
        out[0] = out[1] = out[2] = 0.0f;
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
        float T = 1.0f;

        for (int it = 0; it < limit; it++) {
            float tHit;
            if (tMaxX < tMaxY && tMaxX < tMaxZ) { tHit = tMaxX; x += sx; tMaxX += tDx; }
            else if (tMaxY < tMaxZ) { tHit = tMaxY; y += sy; tMaxY += tDy; }
            else { tHit = tMaxZ; z += sz; tMaxZ += tDz; }
            if (tHit > r1) break;
            if (!vol_.inBounds(x, y, z)) break;
            const size_t ci = vol_.idx(x, y, z);
            const float A = VoxelVolume::a01(vol_.color[ci]);
            if (A > 0.0f) {
                if (tHit >= r0) {
                    const RGBA8 e = vol_.emission[ci];
                    out[0] += T * u8f(e.r) * set_.emitScale * A;
                    out[1] += T * u8f(e.g) * set_.emitScale * A;
                    out[2] += T * u8f(e.b) * set_.emitScale * A;
                }
                T *= (1.0f - A);
                if (T < 1e-3f) break;
            }
        }
    }

    // 合并：逐层双线性插值再相加
    void mergeCascades(int x, int y, float lit[3]) const {
        for (const Cascade& cc : cascades_) {
            const float u = ((float)x + 0.5f) / (float)cc.spacing - 0.5f;
            const float v = ((float)y + 0.5f) / (float)cc.spacing - 0.5f;
            const int x0 = (int)std::floor(u), y0 = (int)std::floor(v);
            const float tx = u - (float)x0, ty = v - (float)y0;
            for (int dy = 0; dy <= 1; dy++)
                for (int dx = 0; dx <= 1; dx++) {
                    const int gx = std::clamp(x0 + dx, 0, cc.gw - 1);
                    const int gy = std::clamp(y0 + dy, 0, cc.gh - 1);
                    const float w = (dx ? tx : 1.0f - tx) * (dy ? ty : 1.0f - ty);
                    const float* d = &cc.val[((size_t)gy * (size_t)cc.gw + (size_t)gx) * 3u];
                    lit[0] += d[0] * w;
                    lit[1] += d[1] * w;
                    lit[2] += d[2] * w;
                }
        }
    }

    // 阴影射线（Amanatides-Woo 3D DDA）
    bool traceShadow(const float p[3], const float d[3], float& tr, int limit,
                     float maxT = 1e30f) const {        tr = 1.0f;
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
    std::vector<Cascade> cascades_;   // Radiance Cascades
    float cascadeL0_ = -1.0f;
    int cascadeK0_ = -1;
    float emitScaleUsed_ = -1.0f;
    std::vector<float> accum_;
    int frames_ = 0;
    double lastMs_ = 0.0;
};

std::unique_ptr<VoxelRenderer> createVoxelRenderer() {
    return std::make_unique<VoxelTracer>();
}

}  // namespace pb::render
