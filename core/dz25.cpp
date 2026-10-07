// core/dz25.cpp — 2.5D 光追：**解析 gather**（不采样方向）
//
// ## 为什么放弃"方向集合"
//
//   方向集合是一条 1D 曲线（中点圆）或一张离散坡度表。光源只有 1 格厚时，
//   "射线能否穿过光源"是阶跃的：紧贴光源的格子可能一条都打不到，稍远反而打到。
//   表现就是**紧贴光源处发黑、隔一段才亮**的条带。加方向、加抖动都只是把
//   条带挪位置或打散成噪声，治不了根 —— 因为薄光源在角度上是零测度的。
//
// ## 解析 gather（本文件）
//
//   规格第四节 I(p) = (1/|D+|) Σ_{d∈D+} L(p,d) 是**半球积分**。
//   它的连续极限是：对场景里每个发光体素 q 求几何贡献 —— 不需要任何方向
//   采样，于是**不可能产生条带**。
//
//   对每个表面点 p=(x,y,h+1) 与每个发光体素 q：
//     * 只取上半球：要求 q.z > p.z
//     * 遮挡/透射：从 p 沿直线走到 q，路上遇到不透明体素就断，
//       半透明按 (1-A) 衰减
//     * 权重按朗伯辐照度：dE = E(q)·cosθ / r²，其中 cosθ = Δz/r
//
//   复杂度 O(表面点数 × 发光体素数 × 步数)。发光体素通常不多，
//   换来的是完全无条带。
#include "dz25.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace pb {
namespace {

inline float u8f(uint8_t v) { return (float)v / 255.0f; }

struct Emitter {
    int x, y, z;
    float e[3];
};

}  // namespace

void trace25D(const VoxelGrid& g, const Dz25Params& prm, std::vector<RGBA8>& out) {
    const int X = g.X, Y = g.Y, Z = g.Z;
    out.assign((size_t)std::max(0, X) * (size_t)std::max(0, Y), RGBA8{0, 0, 0, 255});
    if (X <= 0 || Y <= 0 || Z <= 0) return;

    const float es = prm.emitScale;

    // ---- 1. 收集发光体素 ----
    std::vector<Emitter> ems;
    for (int z = 0; z < Z; z++)
        for (int y = 0; y < Y; y++)
            for (int x = 0; x < X; x++) {
                const Voxel& v = g.at(x, y, z);
                const float e[3] = {u8f(v.mat.emR) * es, u8f(v.mat.emG) * es,
                                    u8f(v.mat.emB) * es};
                if (e[0] > 0.0f || e[1] > 0.0f || e[2] > 0.0f)
                    ems.push_back(Emitter{x, y, z, {e[0], e[1], e[2]}});
            }

    // ---- 2. 柱高 ----
    const size_t plane = (size_t)X * (size_t)Y;
    std::vector<int> height(plane, -1);
    for (int y = 0; y < Y; y++)
        for (int x = 0; x < X; x++)
            for (int z = Z - 1; z >= 0; z--)
                if (g.at(x, y, z).solid()) {
                    height[(size_t)y * (size_t)X + (size_t)x] = z;
                    break;
                }

    // ---- 3. 逐表面点解析 gather（按行多线程）----
    //
    // 成本 = 表面点数 × 发光体素数 × 步数，所以两件事最有效：
    //   1. 按行切给多个线程
    //   2. 限制半径 —— 1/r³ 让远处的贡献迅速变得无关紧要
    const float outer = prm.radius > 0 ? (float)prm.radius : 1e9f;
    std::vector<float> I0(plane * 3u, 0.0f);

    auto gatherRows = [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < X; x++) {
            const size_t pi = (size_t)y * (size_t)X + (size_t)x;
            const int h = height[pi];
            if (h < 0) continue;
            const int pz = h + 1;

            float acc[3] = {0, 0, 0};
            for (const Emitter& q : ems) {
                const int dz = q.z - pz;
                if (dz <= 0) continue;                 // 只取上半球
                const int dx = q.x - x, dy = q.y - y;
                const float r2 = (float)(dx * dx + dy * dy + dz * dz);
                if (r2 > outer * outer) continue;      // 太远：1/r³ 已经无关紧要
                const float r = std::sqrt(r2);

                // 从 p 沿直线走到 q，累透射率；遇到不透明就断
                float T = 1.0f;
                {
                    const int adx = std::abs(dx), ady = std::abs(dy);
                    const int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
                    const int sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
                    int cx = x, cy = y, cz = pz;
                    float tx = adx > 0 ? 0.5f / (float)adx : 1e30f;
                    float ty = ady > 0 ? 0.5f / (float)ady : 1e30f;
                    float tz = 0.5f / (float)dz;
                    const float dtx = adx > 0 ? 1.0f / (float)adx : 1e30f;
                    const float dty = ady > 0 ? 1.0f / (float)ady : 1e30f;
                    const float dtz = 1.0f / (float)dz;
                    int guard = adx + ady + dz + 4;
                    while (guard-- > 0) {
                        if (tx < ty && tx < tz) { cx += sx; tx += dtx; }
                        else if (ty < tz) { cy += sy; ty += dty; }
                        else { cz += 1; tz += dtz; }
                        if (cx == q.x && cy == q.y && cz == q.z) break;   // 到达光源
                        if (cx < 0 || cy < 0 || cz < 0 || cx >= X || cy >= Y || cz >= Z)
                            break;
                        const float A = g.at(cx, cy, cz).a01();
                        if (A >= 1.0f) { T = 0.0f; break; }
                        if (A > 0.0f) {
                            T *= (1.0f - A);
                            if (T < 1e-3f) { T = 0.0f; break; }
                        }
                    }
                }
                if (T <= 0.0f) continue;

                // 朗伯辐照度权重：cosθ / r² = (dz/r) / r²
                const float wgt = (float)dz / (r2 * r) * prm.gain;
                acc[0] += q.e[0] * T * wgt;
                acc[1] += q.e[1] * T * wgt;
                acc[2] += q.e[2] * T * wgt;
            }
            I0[pi * 3u + 0] = acc[0];
            I0[pi * 3u + 1] = acc[1];
            I0[pi * 3u + 2] = acc[2];
        }
    }
    };

    {
        const int hw = (int)std::thread::hardware_concurrency();
        const int nt = std::max(1, std::min(hw > 0 ? hw : 4, Y));
        if (nt <= 1) {
            gatherRows(0, Y);
        } else {
            std::vector<std::thread> th;
            th.reserve((size_t)nt);
            const int chunk = (Y + nt - 1) / nt;
            for (int i = 0; i < nt; i++) {
                const int a = i * chunk, b = std::min(Y, a + chunk);
                if (a >= b) break;
                th.emplace_back(gatherRows, a, b);
            }
            for (auto& t : th) t.join();
        }
    }

    // ---- 4. 上色：多层累积 Color = Σ_i T_i·(I0·A_i·C_i + E_i) ----
    for (int y = 0; y < Y; y++) {
        for (int x = 0; x < X; x++) {
            const size_t pi = (size_t)y * (size_t)X + (size_t)x;
            const int h = height[pi];
            if (h < 0) continue;
            const float* I = &I0[pi * 3u];
            float col[3] = {0, 0, 0};
            float T = 1.0f;
            for (int z = h; z >= 0; z--) {
                const Voxel& v = g.at(x, y, z);
                const float A = v.a01();
                if (A <= 0.0f) continue;
                const float E[3] = {u8f(v.mat.emR) * es, u8f(v.mat.emG) * es,
                                    u8f(v.mat.emB) * es};
                const float C[3] = {u8f(v.color.r), u8f(v.color.g), u8f(v.color.b)};
                for (int c = 0; c < 3; c++) col[c] += T * (I[c] * A * C[c] + E[c]);
                T *= (1.0f - A);
                if (T < 1e-3f) break;
            }
            const float ov = std::max(col[0], std::max(col[1], col[2]));
            if (ov > 1.0f) { col[0] /= ov; col[1] /= ov; col[2] /= ov; }
            auto to8 = [](float v) {
                return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            };
            out[pi] = RGBA8{to8(col[0]), to8(col[1]), to8(col[2]), 255};
        }
    }
}

}  // namespace pb
