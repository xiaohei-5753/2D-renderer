// core/dz_plane.h — d-z 平面上的 2D 光线追踪（第一版：只求正确，不求快）
//
// ## 这是什么
//
// 把「沿某条扫描射线方向 ω 收集光线」这件事，从 (x,y) 平面搬到 **(d, z) 平面**：
//
//   d 轴 = 沿扫描射线的位置（单位：画布像素，一格一个像素）
//   z 轴 = 图层高度（单位：层。第 i 层占 [i, i+1]，上表面在 z = i+1）
//
// 在这个平面上它就是一支**标准 2D 光线追踪器**：
//   * 场景 = 每个 (d,z) 格子里的像素（albedo / 覆盖率 / 自发光）
//   * 摄像机 = 我们要算的那个点：位置 d 的**最高像素**的上表面
//   * 光源 = 天空。天空**不是格子里的东西**，而是射线走到窗口外时的初始值
//            （所以空气格子不需要携带环境光 —— 光只在射线出发时注入一次）
//   * 射线 = 从光源方向射下来，与上表面求交；被更高的像素挡住就是处于阴影里
//
// ## 为什么需要它（要解决的问题）
//
// 平面内扫描不知道"高度"，所以"一个像素能挡住多远之外的像素"在 2D 里没有定义，
// 只能用一个硬开关（盖住/没盖住）近似。有了高度之后，影子会相对遮挡物**平移**
//   Δd = 高度 / 斜率
// 这就是有限阴影，而且是三维光照该有的行为。
//
// ## 这一版的约束（有意为之）
//
// **只追求正确，完全不管性能。** 射线用固定步长浮点步进（最简单的做法），
// 不使用 DDA、不做导数、不做任何加速结构。等正确性验收通过，第二版再谈等价优化
// （那时才可以引入整数步进、逐格 DDA、复用现有 GPU 结构）。
//
// 唯一保留的"优化"是：窗口高度只到 `maxHeight + 1`，因为更高的格子永远是空气，
// 射线在那里不可能与任何东西求交。这不改变结果，只是不浪费。
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "types.h"

namespace pb {

// d-z 平面上的一个格子。
struct DzCell {
    RGBA8 albedo{};     // 颜色 + 覆盖率（a）；a == 0 即空气
    uint8_t emR = 0;    // 自发光（与 albedo 完全独立，见 material.h）
    uint8_t emG = 0;
    uint8_t emB = 0;
};

// 一条扫描射线方向上的整个 d-z 平面。
//
// 内存布局：`cell[(z - 1) * width + d]`，所以**同一 d 的一整列在内存里是连续的**，
// 而同一层是跨步的。这样"某一列的最高像素"和"沿 d 收集"都很好写。
struct DzPlane {
    int width = 0;      // m：沿扫描射线的像素数
    int maxHeight = 0;  // 最高的那个上表面所在的 z（空气之上不再有格子）
    std::vector<DzCell> cell;

    void init(int w, int maxH) {
        width = w < 0 ? 0 : w;
        maxHeight = maxH < 0 ? 0 : maxH;
        cell.assign((size_t)width * (size_t)(maxHeight + 1), DzCell{});
    }

    bool empty() const { return width <= 0 || cell.empty(); }
    int height() const { return maxHeight + 1; }   // 窗口的行数
    bool inRange(int z, int d) const {
        return z >= 1 && z <= maxHeight && d >= 0 && d < width;
    }

    // ⚠️ 越界会直接终止程序，而不是悄悄写坏堆。
    //
    // 这里原来是裸的 vector::operator[]。我因为把 `maxHeight + 1`（行数）当成
    // 最高层号用了一次，写出去了一个元素：第 1 次运行看不出问题，
    // 到后面 free() 才发现堆被写坏 —— 排查花了很久，而且现场完全指不到真凶。
    // 这类"行数 vs 层号"的 off-by-one 太容易犯，所以让它当场响。
    //
    // 并且顺手打印调用栈：只知道"越界了"不够，得知道**谁**越界的。
    static void reportOOB(int z, int d, int maxH, int w) {
        fprintf(stderr, "[dz] 严重错误：访问越界 (z=%d, d=%d)，合法范围 z=1..%d, d=0..%d\n", z, d,
                maxH, w - 1);
        fflush(stderr);
        std::abort();
    }

    DzCell& at(int z, int d) {
        if (!inRange(z, d)) reportOOB(z, d, maxHeight, width);
        return cell[(size_t)(z - 1) * (size_t)width + (size_t)d];
    }
    const DzCell& at(int z, int d) const {
        if (!inRange(z, d)) reportOOB(z, d, maxHeight, width);
        return cell[(size_t)(z - 1) * (size_t)width + (size_t)d];
    }

    // 位置 d 处最高像素所在的 z（= 它的上表面高度）。没有像素时返回 0。
    int surfaceHeight(int d) const {
        if (d < 0 || d >= width) return 0;
        for (int z = maxHeight; z >= 1; z--)
            if (cell[(size_t)(z - 1) * (size_t)width + (size_t)d].albedo.a > 0) return z;
        return 0;
    }

    // 格子是否参与遮挡。覆盖率 > 0 就挡光（a == 255 是全挡，中间值半透）。
    static bool blocks(const DzCell& c) { return c.albedo.a > 0; }
};

// 渲染参数。
//
// ## 方向约定（这里错了会得到"影子朝反方向"这种很难看出来的 bug）
//
// 光**从左上斜射下来**，传播方向 = (d 增大, z 减小)。
// 因此从某个表面点"朝光源看"的方向是 **(d 减小, z 增大)** —— 也就是上、左。
// 所以可见性射线的 `dirSign = -1`（沿 d 减小的一侧）。
//
// 由此推出遮挡关系（S = stepsPerLevel，遮挡物 d0 处的表面高度 topZ）：
//     影子覆盖    [d0, d0 + S*(topZ-1)]
//     第一个受光列 d0 + S*(topZ-1) + 1
//
// 那个 +1 是**离散化**带来的：射线按 1/64 格积分，而"擦过"用了一个容差，
// 于是正好落在遮挡物顶角上的那条射线算作被挡住，受光边界因此右移一格。
// 这是当前数值积分实现的性质，改成精确 DDA 后边界会落在 d0 + S*(topZ-1)。
// 测试里按带 +1 的形式断言，这样一旦实现变了就会立刻被发现。
struct DzParams {
    // 光线方向。斜率的定义：**沿 d 每走多少格，射线在 z 上上升 1 层**。
    // 用"每层走几格"而不是"每格升几层"，是为了避免除零、也便于整数步进：
    //   stepsPerLevel = 0 -> 垂直入射（影子零偏移，退化成平面内扫描）
    //   stepsPerLevel = 1 -> 45°
    //   stepsPerLevel 越大 -> 光越贴近水平，影子拉得越长
    float stepsPerLevel = 4.0f;

    // 射线往哪一侧倾斜。默认 -1（朝左上，对应"光从左上射下来"）。
    int dirSign = -1;

    // 天空的初始光线（环境光）。**这是唯一的入射光来源**：
    // 射线从窗口走出去时带走它，空气格子本身不自发光。
    float sky[3] = {1.0f, 1.0f, 1.0f};

    // 混合：最终颜色 = 上表面的 albedo * 收到的光 + 自发光。
    // 返回 true 时用自发光与 albedo 一起算（见 dz_plane.cpp 里的说明）。
    bool addEmission = true;
};

// 求某一个 d 处、最高像素上表面收到的光（RGB，线性空间，可以超过 1）。
// 这就是"把射线投射到顶端像素得到的光"。
void dzSampleLight(const DzPlane& p, const DzParams& prm, int d, float out[3]);

// 对整个平面求一遍：每个 d 一格 RGB 结果。out 会被 resize 成 width*3。
void dzTraceLine(const DzPlane& p, const DzParams& prm, std::vector<float>& out);

// 最终颜色 = albedo * 光 + 自发光（都归一化到 0..1）。方便出图与断言。
RGBA8 dzShade(const DzCell& surface, const float light[3], const DzParams& prm);

}  // namespace pb
