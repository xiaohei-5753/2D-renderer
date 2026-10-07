// core/dz25slice.cpp — 截面 + 「顶边→底边」斜线扫描（推导见 dz25slice.h）
#include "dz25slice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pb {
namespace {

inline float u8f(uint8_t v) { return (float)v / 255.0f; }

// 该列最高的实体层；没有实体返回 -1
int columnTop(const std::vector<RGBA8>& col, int W, int H, int d) {
    const int layers = H - 1;                       // 最后一行是天空行
    for (int z = layers - 1; z >= 0; z--)
        if (col[(size_t)d * (size_t)H + (size_t)z].a > 0) return z;
    return -1;
}

}  // namespace

namespace {

// 沿一条斜线从顶往底走，逐格累积。
// 线：(d, z) 出发，方向 (sgn(tau), -1)。用 2D DDA —— 斜率平的时候
// 一步 z 会对应多步 d，DDA 会自己把这些中间格子都走到（这正是"逐格穿过"）。
void walkLine(const std::vector<RGBA8>& col, const std::vector<RGBA8>& emis, int W, int H,
              const std::vector<int>& zr, const Dz25SliceParams& prm, float emitScale,
              int d0, int z0, int tau, std::vector<float>& irr, std::vector<int>& hits) {
    const int Hm1 = H - 1;
    const int adx = std::abs(tau), adz = Hm1;
    if (adz <= 0) return;
    const int sx = (tau > 0) ? 1 : (tau < 0 ? -1 : 0);

    int d = d0, z = z0;
    float td = adx > 0 ? 0.5f / (float)adx : 1e30f;
    float tz = 0.5f / (float)adz;
    const float dtd = adx > 0 ? 1.0f / (float)adx : 1e30f;
    const float dtz = 1.0f / (float)adz;

    float carry[3] = {prm.ambient[0], prm.ambient[1], prm.ambient[2]};
    if (!prm.ambientOn) { carry[0] = carry[1] = carry[2] = 0.0f; }

    const int guard = W + H + adx + 4;
    for (int step = 0; step < guard; step++) {
        if (z < 0 || z > Hm1 || d < 0 || d >= W) break;   // 离开平面
        if (z == zr[d] && zr[d] >= 0) {
            irr[(size_t)d * 3u + 0] += carry[0];
            irr[(size_t)d * 3u + 1] += carry[1];
            irr[(size_t)d * 3u + 2] += carry[2];
            hits[(size_t)d]++;
        }
        const RGBA8 c = col[(size_t)d * (size_t)H + (size_t)z];
        const float A = u8f(c.a);
        if (A > 0.0f) {
            const RGBA8 e = emis[(size_t)d * (size_t)H + (size_t)z];
            const float E[3] = {u8f(e.r) * emitScale, u8f(e.g) * emitScale,
                                u8f(e.b) * emitScale};
            if (A >= 1.0f) {
                carry[0] = E[0]; carry[1] = E[1]; carry[2] = E[2];
            } else {
                carry[0] = carry[0] * (1.0f - A) + E[0];
                carry[1] = carry[1] * (1.0f - A) + E[1];
                carry[2] = carry[2] * (1.0f - A) + E[2];
            }
        }
        if (td < tz) { d += sx; td += dtd; }
        else { z -= 1; tz += dtz; }
    }
}

}  // namespace

void sliceScan(const std::vector<RGBA8>& col, const std::vector<RGBA8>& emis, int W, int H,
               const Dz25SliceParams& prm, std::vector<float>& irr) {
    irr.assign((size_t)W * 3u, 0.0f);
    if (W <= 0 || H < 2) return;

    std::vector<int> zr(W, -1);
    for (int d = 0; d < W; d++) {
        const int t = columnTop(col, W, H, d);
        if (t >= 0 && t + 1 < H) zr[d] = t + 1;
    }

    const int Hm1 = H - 1;
    std::vector<int> hits((size_t)W, 0);
    const float es = prm.emitScale;

    // 斜率 τ：斜线跨越整个高度 H-1 时的横向位移。
    // 要覆盖"横向跨 W 格、纵向 1 层"这种最平的斜线，需要 τ 到 W·(H-1)。
    const int tauMax = std::max(1, W * Hm1);
    int usedTau = 0;

    for (int tau = -tauMax; tau <= tauMax; tau++) {
        const int adx = std::abs(tau);
        if (adx <= Hm1) {
            // ---- 陡：按**顶边**截距枚举 ----
            //
            // 顶边上每 1 格一条线；范围稍微外扩，让略斜的线也能从侧边进来
            // （这就是"延长光源发出面"）。
            for (int k = -adx - 2; k <= W + adx + 1; k++)
                walkLine(col, emis, W, H, zr, prm, es, k, Hm1, tau, irr, hits);
        } else {
            // ---- 平：按**左边界的 z 截距**枚举 ----
            //
            // 平的斜线在同一个 z 上会横跨很多列，若仍按顶边截距枚举就会有
            // O(τ) 条线互相重叠（而且极慢）。改按"在 d=0 处的 z"枚举：
            //   z0 ∈ [0, (H-1)·(1 + W/τ)]，共 O(H·W/τ) 条 —— 正好铺满平面。
            const int z0max = Hm1 + (int)((double)Hm1 * (double)W / (double)adx) + 2;
            for (int z0 = 0; z0 <= z0max; z0++) {
                // τ>0 的线往右下走 -> 参考左边 d=0；
                // τ<0 的线往左下走 -> 参考右边 d=W-1（否则一起步就出界）
                const bool right = (tau < 0);
                const int edge = right ? (W - 1) : 0;
                int d0 = edge, zz0 = z0;
                if (z0 > Hm1) {
                    // 这条线在参考边上的交点已在顶边之上，于是它从**顶边**进入
                    const double dd = (double)(z0 - Hm1) * (double)adx / (double)Hm1;
                    const int off = (int)std::ceil(dd);
                    d0 = right ? (edge - off) : (edge + off);
                    zz0 = Hm1;
                }
                if (d0 < 0 || d0 >= W) continue;
                walkLine(col, emis, W, H, zr, prm, es, d0, zz0, tau, irr, hits);
            }
        }
        usedTau++;
    }

    for (int d = 0; d < W; d++) {
        const int hh = hits[(size_t)d];
        if (hh <= 1) continue;
        const float inv = 1.0f / (float)hh;
        irr[(size_t)d * 3u + 0] *= inv;
        irr[(size_t)d * 3u + 1] *= inv;
        irr[(size_t)d * 3u + 2] *= inv;
    }
    if (usedTau > 0) {
        const float inv = 1.0f / (float)usedTau;
        for (float& v : irr) v *= inv;
    }
    if (getenv("PB_DBG")) {
        float mx = 0.0f;
        int mxd = 0, hmax = 0;
        for (int d = 0; d < W; d++) {
            if (irr[(size_t)d * 3u] > mx) { mx = irr[(size_t)d * 3u]; mxd = d; }
            hmax = std::max(hmax, hits[(size_t)d]);
        }
        fprintf(stderr, "[slice] W=%d H=%d tauMax=%d usedTau=%d | irr 最大 %.4f @d=%d | hits 最大 %d\n",
                W, H, tauMax, usedTau, mx, mxd, hmax);
    }
}
void trace25DSlice(const VoxelGrid& g, const Dz25SliceParams& prm,
                   std::vector<RGBA8>& out) {
    const int X = g.X, Y = g.Y, Z = g.Z;
    out.assign((size_t)std::max(0, X) * (size_t)std::max(0, Y), RGBA8{0, 0, 0, 255});
    if (X <= 0 || Y <= 0 || Z <= 0) return;

    const int H = Z + 1;
    // 累积每个像素的辐照度
    std::vector<float> acc((size_t)X * (size_t)Y * 3u, 0.0f);

    const int nd = std::clamp(prm.directions, 1, 4);

    for (int dir = 0; dir < nd; dir++) {
        const bool alongX = (dir == 0 || dir == 1);
        const int lines = alongX ? Y : X;          // 扫描线条数
        const int L = alongX ? X : Y;              // 每条线的长度

        std::vector<RGBA8> col, emis, irr8;
        std::vector<float> irr;

        for (int li = 0; li < lines; li++) {
            // 建这一条截面
            col.assign((size_t)L * (size_t)H, RGBA8{0, 0, 0, 0});
            emis.assign((size_t)L * (size_t)H, RGBA8{0, 0, 0, 0});
            for (int l = 0; l < L; l++) {
                int x = 0, y = 0;
                if (dir == 0) { x = l; y = li; }
                else if (dir == 1) { x = L - 1 - l; y = li; }
                else if (dir == 2) { x = li; y = l; }
                else { x = li; y = L - 1 - l; }
                for (int z = 0; z < Z; z++) {
                    const Voxel& v = g.at(x, y, z);
                    const size_t k = (size_t)l * (size_t)H + (size_t)z;
                    col[k] = v.color;
                    emis[k] = RGBA8{v.mat.emR, v.mat.emG, v.mat.emB, 255};
                }
            }

            sliceScan(col, emis, L, H, prm, irr);

            // 投影回画布
            for (int l = 0; l < L; l++) {
                int x = 0, y = 0;
                if (dir == 0) { x = l; y = li; }
                else if (dir == 1) { x = L - 1 - l; y = li; }
                else if (dir == 2) { x = li; y = l; }
                else { x = li; y = L - 1 - l; }
                const size_t pi = ((size_t)y * (size_t)X + (size_t)x) * 3u;
                acc[pi + 0] += irr[(size_t)l * 3u + 0];
                acc[pi + 1] += irr[(size_t)l * 3u + 1];
                acc[pi + 2] += irr[(size_t)l * 3u + 2];
            }
        }
    }

    const float invDir = (nd > 0) ? 1.0f / (float)nd : 0.0f;

    // 上色：多层累积
    for (int y = 0; y < Y; y++) {
        for (int x = 0; x < X; x++) {
            const size_t pi = (size_t)y * (size_t)X + (size_t)x;
            int h = -1;
            for (int z = Z - 1; z >= 0; z--)
                if (g.at(x, y, z).solid()) { h = z; break; }
            if (h < 0) continue;
            const float I[3] = {acc[pi * 3u + 0] * invDir, acc[pi * 3u + 1] * invDir,
                                acc[pi * 3u + 2] * invDir};
            float c3[3] = {0, 0, 0};
            float T = 1.0f;
            for (int z = h; z >= 0; z--) {
                const Voxel& v = g.at(x, y, z);
                const float A = v.a01();
                if (A <= 0.0f) continue;
                const float E[3] = {u8f(v.mat.emR) * prm.emitScale,
                                    u8f(v.mat.emG) * prm.emitScale,
                                    u8f(v.mat.emB) * prm.emitScale};
                const float C[3] = {u8f(v.color.r), u8f(v.color.g), u8f(v.color.b)};
                for (int c = 0; c < 3; c++) c3[c] += T * (I[c] * A * C[c] + E[c]);
                T *= (1.0f - A);
                if (T < 1e-3f) break;
            }
            const float ov = std::max(c3[0], std::max(c3[1], c3[2]));
            if (ov > 1.0f) { c3[0] /= ov; c3[1] /= ov; c3[2] /= ov; }
            auto to8 = [](float v) {
                return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            };
            out[pi] = RGBA8{to8(c3[0]), to8(c3[1]), to8(c3[2]), 255};
        }
    }
}

}  // namespace pb
