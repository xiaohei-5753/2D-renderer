// render/voxel_renderer.cpp — +z 面的 3D 体素光线追踪
//
// ## 主射线
//
//   正交俯视：每个画布像素 (x,y) 从 (x+0.5, y+0.5, +∞) 沿 (0,0,-1) 射下，
//   打到最高的实体体素，取它的 **+z 面**（法线 n = (0,0,1)）。
//   命中点 p = (x+0.5, y+0.5, h+1)，这就是要着色的表面点。
//
// ## 着色（3D 空间光追）
//
//   Color = E_top + C_top·A_top·( 太阳项 + 天空项 )
//     太阳项 = sunColor·sunIntensity·max(0, n·L)·visibility
//     天空项 = skyColor·（上半球可见比例，这里按"朝上的面"取 1）
//
//   visibility 由**阴影射线**决定：从 p 沿 L 做 3D 体素 DDA（Amanatides-Woo），
//   碰到不透明体素 -> 被遮住；半透明按 (1-A) 衰减。
//
// ## 为什么这样就"能套用 3D 光追的优化"
//
//   阴影射线是标准的 3D 体素遍历，所以下面这些都是现成可加的：
//     * 空跳（empty-space skipping）：粗占据网格 / brick map
//     * 稀疏体素八叉树（SVO）
//     * 阴影贴图（从光源方向的深度图，把所有阴影测试降成 O(1)）
//     * 多光源 / 面光源用光源 BVH + 重要性采样
//   而之前的解析 gather 是二维启发式，套不上这些。
#include "render/voxel_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace pb::render {
namespace {

inline float u8f(uint8_t v) { return (float)v / 255.0f; }

// Amanatides-Woo 体素 DDA：从 p 沿 d 走，返回是否被挡住。
// 半透明体素按 (1-A) 累积衰减；衰减到阈值以下就算被挡住。
// limit：最多走多少格（防止极端情况）。
bool traceOccluded(const VoxelVolume& v, const float p[3], const float d[3], float& trans,
                   int limit) {
    trans = 1.0f;

    int x = (int)std::floor(p[0]);
    int y = (int)std::floor(p[1]);
    int z = (int)std::floor(p[2]);

    const int stepX = (d[0] > 0) ? 1 : (d[0] < 0 ? -1 : 0);
    const int stepY = (d[1] > 0) ? 1 : (d[1] < 0 ? -1 : 0);
    const int stepZ = (d[2] > 0) ? 1 : (d[2] < 0 ? -1 : 0);

    const float inf = 1e30f;
    auto tDelta = [&](float dd) { return (dd != 0.0f) ? std::fabs(1.0f / dd) : inf; };
    auto tMax = [&](float pp, int cell, int st, float dd) {
        if (dd == 0.0f || st == 0) return inf;
        const float bound = (st > 0) ? (float)(cell + 1) : (float)cell;
        return (bound - pp) / dd;
    };
    float tMaxX = tMax(p[0], x, stepX, d[0]);
    float tMaxY = tMax(p[1], y, stepY, d[1]);
    float tMaxZ = tMax(p[2], z, stepZ, d[2]);
    const float tDx = tDelta(d[0]), tDy = tDelta(d[1]), tDz = tDelta(d[2]);

    for (int it = 0; it < limit; it++) {
        // 先走出起点所在格（起点在表面正上方，不在实体里）
        if (tMaxX < tMaxY && tMaxX < tMaxZ) { x += stepX; tMaxX += tDx; }
        else if (tMaxY < tMaxZ) { y += stepY; tMaxY += tDy; }
        else { z += stepZ; tMaxZ += tDz; }

        if (!v.inBounds(x, y, z)) return false;   // 走出体素空间 -> 没被挡

        const float A = VoxelVolume::a01(v.color[v.idx(x, y, z)]);
        if (A >= 1.0f) return true;               // 不透明：挡住
        if (A > 0.0f) {
            trans *= (1.0f - A);
            if (trans < 1e-3f) return true;
        }
    }
    return false;
}

}  // namespace

class VoxelTracer final : public VoxelRenderer {
public:
    bool setVolume(const VoxelVolume& v) override {
        vol_ = v;
        // 列高缓存：主射线是垂直的，预先算好每列的最高实体层
        top_.assign((size_t)v.X * (size_t)v.Y, -1);
        for (int y = 0; y < v.Y; y++)
            for (int x = 0; x < v.X; x++)
                top_[(size_t)y * (size_t)v.X + (size_t)x] = v.columnTop(x, y);
        return true;
    }

    void setSettings(const RenderSettings& s) override { set_ = s; }

    void render(std::vector<RGBA8>& out) override {
        const auto t0 = std::chrono::steady_clock::now();
        const int X = vol_.X, Y = vol_.Y;
        out.assign((size_t)std::max(0, X) * (size_t)std::max(0, Y), RGBA8{0, 0, 0, 255});
        if (vol_.empty()) return;

        // 主光方向归一化
        float L[3] = {set_.sunDir[0], set_.sunDir[1], set_.sunDir[2]};
        const float ln = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (ln > 1e-6f) { L[0] /= ln; L[1] /= ln; L[2] /= ln; }
        // +z 面的法线是 (0,0,1)，所以 n·L = L.z
        const float ndl = std::max(0.0f, L[2]);
        const int limit = (X + Y + vol_.Z) * 2 + 8;

        auto rows = [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                for (int x = 0; x < X; x++) {
                    const int h = top_[(size_t)y * (size_t)X + (size_t)x];
                    if (h < 0) continue;              // 空柱
                    const size_t ci = vol_.idx(x, y, h);
                    const RGBA8 C = vol_.color[ci];
                    const RGBA8 E = vol_.emission[ci];
                    const float A = VoxelVolume::a01(C);

                    // 命中点：柱顶那一格的 +z 面中心
                    const float p[3] = {(float)x + 0.5f, (float)y + 0.5f, (float)(h + 1)};

                    // 阴影射线
                    float vis = 1.0f;
                    if (set_.shadows && ndl > 0.0f) {
                        float tr = 1.0f;
                        if (traceOccluded(vol_, p, L, tr, limit)) vis = 0.0f;
                        else vis = tr;
                    }

                    float col[3];
                    for (int c = 0; c < 3; c++) {
                        const float Cs = u8f(c == 0 ? C.r : (c == 1 ? C.g : C.b));
                        const float Es = u8f(c == 0 ? E.r : (c == 1 ? E.g : E.b));
                        const float sun = set_.sunColor[c] * set_.sunIntensity * ndl * vis;
                        const float sky = set_.skyColor[c];      // 朝上的面：整片天可见
                        const float lit = sun + sky;
                        col[c] = Es * set_.emitScale + Cs * A * lit;
                    }
                    // 归一化：超过 1 就按最大值缩放（避免直接截断偏色）
                    const float ov = std::max(col[0], std::max(col[1], col[2]));
                    if (ov > 1.0f) { col[0] /= ov; col[1] /= ov; col[2] /= ov; }
                    auto to8 = [](float v) {
                        return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
                    };
                    out[(size_t)y * (size_t)X + (size_t)x] =
                        RGBA8{to8(col[0]), to8(col[1]), to8(col[2]), 255};
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

        lastMs_ = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
    }

    double lastMs() const override { return lastMs_; }

private:
    VoxelVolume vol_;
    RenderSettings set_;
    std::vector<int> top_;    // 每列的柱高
    double lastMs_ = 0.0;
};

std::unique_ptr<VoxelRenderer> createVoxelRenderer() {
    return std::make_unique<VoxelTracer>();
}

}  // namespace pb::render
