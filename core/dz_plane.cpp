// core/dz_plane.cpp — d-z 平面上的 2D 光线追踪（第一版：只求正确，不求快）
#include "dz_plane.h"

#include <algorithm>
#include <cmath>


namespace pb {
namespace {

// 把 0..255 归一化到 0..1。
inline float u8(uint8_t v) { return (float)v / 255.0f; }

// 射线位置：t 是已走的弧长，方向 (|dd|, |dz|) = (1, 1/S) 归一化之后。
// 于是"沿 d 每走 S 格，z 上升 1 层"。
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
        // 这一列是空气：没有表面可以承载光照，结果就是"没有"。
        // （不是"天空通过了" —— 天空的光需要有东西接住它。）
        return;
    }

    // stepsPerLevel = 0 表示垂直入射：射线不横向移动，只可能被同列更高处挡住。
    const bool vertical = (prm.stepsPerLevel <= 0.0f);

    // ## 采样步长（这里的关键是"按 d 走固定步长"，不是按弧长走固定步长）
    //
    // 沿射线的固定步长本质是数值积分，必须保证**每个格子至少被采到几次**，
    // 否则薄特征会被整条漏掉。我先后踩过两次：
    //   * 弧长步长 1/4      -> 1 格厚的自发光带整条消失
    //   * 弧长步长 1/8      -> 仍然会漏（因为 d 的推进速度随 S 变化，
    //                          弧长步长换算到 d 上就不固定了）
    //
    // 正确做法：**让 d 每次只走 0.25 格**。因为 |dd|/弧长 = 1/n 随 S 变化，
    // 换算回去就是 t 步长 = 0.25 * n —— 这样无论 S 多大，
    // 每个整数列都稳定地被采到 4 次，不会再出现"卡在格子边界上"的抖动。
    constexpr float dStep = 0.25f;

    // 沿程未被衰减的光，初始值 = 天空（环境光）。
    // **这是唯一的入射光来源**：空气格子本身不自发光。
    float lt[3] = {prm.sky[0], prm.sky[1], prm.sky[2]};

    const float z0 = (float)h;
    const float d0 = (float)d + 0.5f;

    // 弧长上界：必须够走到**窗口之外**。
    //
    // ⚠️ 我一开始只按"高度"算（(maxHeight+2) * n），那是错的：
    // 射线是斜的，它要横穿整个窗口宽度才可能走到外面。斜射的射线会在
    // 水平方向走很远，按高度截断就会**在半空中把射线砍掉** ——
    // 表现为"远处的光永远到不了"（dz_test [5] 抓到了这个）。
    // 所以上界要把宽度也算进去，并留出余量。
    const float n = vertical ? 1.0f
                             : std::sqrt(1.0f + 1.0f / (prm.stepsPerLevel * prm.stepsPerLevel));
    // 需要横穿的列数：至少 width（走到横向出界），再叠加爬到顶所需的高度
    const float reach = vertical ? 0.0f : (float)p.width + (float)(p.maxHeight + 2) * prm.stepsPerLevel;
    const float tEnd = vertical ? (float)(p.maxHeight + 2) : reach * n;
    // t 的步长：让 d 恰好走 dStep 格（见上面的说明）
    const float step = vertical ? dStep : dStep * n;

    for (float t = 0.0f; t <= tEnd; t += step) {
        // ⚠️ 这里**不能**做"沿程光已耗尽就早停"的优化。
        //
        // 我加过 `if (lt < eps) break;` 并把它当成纯粹的提速，结果它是个真 bug：
        // 沿程光衰减到 0 并不代表后面没有光了 —— 射线还能**从介质里获得自发光**。
        // 当天光设成 0（只看自发光）时，lt 一开始就是 0，于是整条射线第一次
        // 采样就退出了，自发光永远传不出来。
        // 教训：早停条件必须基于"不可能再有贡献"，而不是"当前携带量为 0"。
        float dd = 0.0f, zz = 0.0f;
        if (vertical) {
            dd = d0;
            zz = z0 + t;
        } else {
            rayAt(prm, t, d0, z0, dd, zz);
        }

        // 走出窗口上方 -> 见到了天空，沿途的光就是结果
        if (zz > (float)(p.maxHeight + 1)) break;
        // 横向走出窗口 -> 同样算见到了外面的光
        if (dd < 0.0f || dd >= (float)p.width) break;

        const int di = (int)std::floor(dd);

        // 判断射线此刻是否"埋在这一列的像素里"：
        // 第 di 列的像素占据高度区间 [0, surf[di]]，所以 zz < surf[di] 就是命中。
        //
        // 只用高度比较，**不要**掺进格子行号 —— 行号只用来取颜色，
        // 让它参与遮挡判定正是我先前绕不出来的原因（两个概念混在一起，
        // 错了以后既看不出也讲不通）。
        //
        // kGrazEps：射线正好擦过像素顶面时，浮点误差会让结果在"挡住/擦过"
        // 之间抖动，边界上会偶尔多出一格。加一个远小于 1 格的容差让"擦过"
        // 稳定地算作擦过。
        const int sh = surf[(size_t)di];
        constexpr float kGrazEps = 1e-3f;
        if (sh < 1 || zz + kGrazEps >= (float)sh) continue;
        // maxHeight 之上没有格子（那里永远是空气）。少了这个检查会越界读到
        // z = maxHeight + 1 —— 被 DzPlane::at 的越界守卫抓到过一次。
        if (sh > p.maxHeight) continue;

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
