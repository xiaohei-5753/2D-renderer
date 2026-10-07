// core/dz_plane.cpp — d-z 平面上的 2D 光线追踪（第一版：只求正确，不求快）
#include "dz_plane.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pb {
namespace {

// 把 0..255 归一化到 0..1。
inline float u8(uint8_t v) { return (float)v / 255.0f; }

// 射线在参数 t（已走的弧长，单位是 d 轴格数）处的位置。
//
// 方向定义（与 DzParams 的注释一致）：
//   |dd| = 1，|dz| = 1 / stepsPerLevel
// 于是"沿 d 每走 stepsPerLevel 格，z 上升 1 层"。
inline void rayAt(const DzParams& prm, float t, float d0, float z0, float& d, float& z) {
    const float n = std::sqrt(1.0f + 1.0f / (prm.stepsPerLevel * prm.stepsPerLevel));
    d = d0 + (float)prm.dirSign * (t / n);
    z = z0 + (t / n) / prm.stepsPerLevel;
}

}  // namespace

void dzSampleLight(const DzPlane& p, const DzParams& prm, int d, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (p.empty() || d < 0 || d >= p.width) return;

    // 先把这条线上每一列的表面高度算出来。
    //
    // 这不只是优化：遍历里每个采样点都去 p.surfaceHeight(di) 重扫一整列，
    // 既慢，又把"射线当前所处的高度"和"这一列被填到多高"两个概念揉在一起 ——
    // 出了错很难定位。预先算成数组后，遍历里只剩纯粹的几何判断。
    std::vector<int> surf((size_t)p.width);
    for (int i = 0; i < p.width; i++) surf[(size_t)i] = p.surfaceHeight(i);

    const int h = surf[(size_t)d];
    if (h <= 0) {
        // 这一列是空气：没有任何表面可以承载光照，结果就是"没有"。
        // （不是"天空通过了" —— 天空的光需要有东西接住它。）
        return;
    }

    // 从竖直入射里取出 dz 步长：slope 越大表示越斜。
    // stepsPerLevel = 0 表示垂直入射，此时射线不横向移动，退化成"只被同列更高的像素挡"。
    const bool vertical = (prm.stepsPerLevel <= 0.0f);

    // ## 步长（第一版最重要的一处"正确性优先"）
    //
    // 沿射线的固定步长本质是数值积分，步长太大会**漏掉薄特征**：
    // 一格厚的自发光/遮挡物可能整个落在两个采样点之间。实测：
    // 步长 0.25 格时，1 格厚的自发光带会被完全漏掉（dz_test [5] 抓到了这一点）。
    //
    // 所以这里取 1/64 格 —— 远小于任何 1 格特征，保证不会穿漏。
    // 代价是每条射线约 (窗口高 * S * 64) 步，很慢，但这一版**只求正确**。
    // 第二版可以换成精确的格子遍历（DDA）：只在格子边界处取样，既精确又快。
    const float step = 1.0f / 64.0f;

    // 沿程未被衰减的光，初始值 = 天空（环境光）。
    float lt[3] = {prm.sky[0], prm.sky[1], prm.sky[2]};

    // 从"上表面"出发向上走。t 是已经走过的弧长。
    // 因为方向里 |dd| = 1，弧长 t 与 d 的变化量在竖直入射时无关，所以用 t 统一处理：
    //   * t 很小 -> 仍在出发点
    //   * t 增大 -> d 向 dirSign 方向平移，z 上升
    const float z0 = (float)h;
    const float d0 = (float)d + 0.5f;

    // 最多走多少弧长：必须够走到窗口之上，同时把 d 的横向范围算进去。
    // 窗口高 maxHeight，横向最多 p.width。取一个宽松上界即可（第一版不追求紧）。
    const float tEnd =
        vertical ? (float)(p.maxHeight + 2)
                 : (float)(p.maxHeight + 2) * (std::sqrt(1.0f + prm.stepsPerLevel * prm.stepsPerLevel));

    for (float t = 0.0f; t <= tEnd; t += step) {
        float dd = 0.0f, zz = 0.0f;
        if (vertical) {
            dd = d0;
            zz = z0 + t;
        } else {
            rayAt(prm, t, d0, z0, dd, zz);
        }

        // 走出窗口上方 -> 见到了天空，沿途的光（含初始值）就是结果
        if (zz > (float)(p.maxHeight + 1)) break;
        // 横向走出窗口 -> 同样算"见到了外面的光"
        if (dd < 0.0f || dd >= (float)p.width) break;

        const int di = (int)std::floor(dd);
        if (di < 0 || di >= p.width) break;

        // 判断射线此刻是否"埋在这一列的像素里"：
        // 第 di 列的像素占据高度区间 [0, surf[di]]，所以 zz < surf[di] 就是命中。
        //
        // 只用高度比较，**不要**掺进格子行号 —— 行号只是用来取颜色，
        // 让它参与遮挡判定正是我先前绕不出来的原因（两个概念混在一起，
        // 错了以后既看不出也讲不通）。
        //
        // kGrazEps：射线正好擦过像素顶面时，浮点误差会让结果在"挡住/擦过"
        // 之间抖动，边界上会偶尔多出一格。加一个远小于 1 格的容差让"擦过"
        // 稳定地算作擦过。
        const int sh = surf[(size_t)di];
        constexpr float kGrazEps = 1e-3f;
        if (sh < 1 || zz + kGrazEps >= (float)sh) continue;

        // 命中的是这一列最上面那一格
        const DzCell& c = p.at(sh, di);

        // 2D 光追的"命中 + 处理材质"，顺序很重要：
        //   1. 先把**已有的**沿程光按这一步的介质衰减（吸收 + albedo 染色）；
        //   2. 再加上这个像素自己的自发光 —— 它是"在介质内部产生的光"，
        //      不应该再被自己那一份覆盖率衰减一次。
        //
        // 这就是"高层的画布照亮低层"的载体：上层的自发光沿射线往下传，
        // 每穿过一层就被那一层的 albedo 与覆盖率过滤一次。
        const float cov = u8(c.albedo.a);
        const float trans = 1.0f - cov;
        lt[0] = lt[0] * trans * u8(c.albedo.r) + u8(c.emR);
        lt[1] = lt[1] * trans * u8(c.albedo.g) + u8(c.emG);
        lt[2] = lt[2] * trans * u8(c.albedo.b) + u8(c.emB);
    }

    out[0] = lt[0];
    out[1] = lt[1];
    out[2] = lt[2];
}

void dzTraceLine(const DzPlane& p, const DzParams& prm, std::vector<float>& out) {
    out.assign((size_t)std::max(0, p.width) * 3u, 0.0f);
    for (int d = 0; d < p.width; d++) {
        float lt[3];
        dzSampleLight(p, prm, d, lt);
        out[(size_t)d * 3 + 0] = lt[0];
        out[(size_t)d * 3 + 1] = lt[1];
        out[(size_t)d * 3 + 2] = lt[2];
    }
}

RGBA8 dzShade(const DzCell& surface, const float light[3], const DzParams& prm) {
    float rgb[3];
    for (int i = 0; i < 3; i++) {
        // 漫反射：表面颜色 × 收到的光
        const float alb[3] = {u8(surface.albedo.r), u8(surface.albedo.g), u8(surface.albedo.b)};
        rgb[i] = alb[i] * light[i];
        if (prm.addEmission) {
            const float em[3] = {u8(surface.emR), u8(surface.emG), u8(surface.emB)};
            rgb[i] += em[i];
        }
        rgb[i] = std::clamp(rgb[i], 0.0f, 1.0f);
    }
    auto to8 = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return RGBA8{to8(rgb[0]), to8(rgb[1]), to8(rgb[2]), 255};
}

}  // namespace pb
