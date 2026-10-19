// core/material.h — 每像素的光学属性
//
// ## 完全解耦：像素有两组**互相独立**的颜色
//
//   1. albedo（存在 Canvas 里）：RGB = 显示成什么颜色、以及透光时把光过滤成什么颜色；
//      A = 遮挡程度。
//   2. 自发光（本结构里的 emR/emG/emB）：这个像素**自己发出**什么颜色的光。
//
// 两者完全独立，所以可以表达：
//   * 暗色表面发绿光（albedo 很暗 + 自发光绿）
//   * 红表面发蓝光（albedo 红 + 自发光蓝）
//   * 同一个 albedo 配任意自发光色
//
// 早期实现把自发光写成 albedo.rgb * 一个标量亮度，那是**耦合**的：
// 自发光颜色被 albedo 绑死，也无法表达"发光的颜色与表面颜色不同"。
#pragma once

#include <cstdint>

namespace pb {

struct Material {
    // 自发光颜色 RGB，各 0..255。0,0,0 = 不自发光。
    // 与 albedo 无关：它只决定"自己发出什么颜色的光"。
    uint8_t emR = 0, emG = 0, emB = 0;

    // 折射率，编码为 (ior - 1.0) * 100，于是 0..255 对应 ior 1.00 .. 3.55。
    // 0 表示不折射（空气或普通表面）。
    uint8_t ior = 0;

    // ------------------------------------------------------------------
    // 这里原来还有 flags / reserved 两个字节，**已删除**（2026-10）
    //
    // flags 里只有一位在用：kSolid（强制完全遮挡，忽略 alpha）。
    // 但它其实是个**冗余功能** —— shader 侧的效果就是 `cov = 1.0`，
    // 而"把 alpha 设成 255"得到的最终像素完全一样。
    // 更关键的是：全项目里只有材质面板的 UI 和一个预览读过它，
    // **光照的着色器从来没读过 flags**。所以删掉它对画质零影响。
    //
    // reserved 从来没被任何代码读过，是当初的过度设计。
    //
    // 删这两个字节换来：
    //   * sizeof(Material) 6 -> 4 字节（每像素材质表省 1/3）
    //   * 扫描阶段的材质纹理可以降到 **RGB8 = 3 字节/像素**
    //     （原来 RGBA8 的 A 通道就是拿去存 flags 的，现在不需要了）
    // ------------------------------------------------------------------
    //
    // 注意：`Material` 现在 == 4 字节，和 RGBA8 一一对应。
    // 扫描侧只打包 RGB（3 字节），因为 ior 只有路径追踪用。

    bool emissive() const { return emR || emG || emB; }

    // 自发光的"亮度"（取三通道最大值），仅用于界面显示
    uint8_t emissionLevel() const {
        const uint8_t m = emR > emG ? emR : emG;
        return m > emB ? m : emB;
    }

    // 直接用 albedo 的颜色作为自发光颜色（"让这个颜色发光"的一键操作）
    void setEmissionFrom(uint8_t r, uint8_t g, uint8_t b) {
        emR = r;
        emG = g;
        emB = b;
    }

    // 自发光 = **表面颜色 × 强度**（强度 0..255，即"最多还原到表面色本身"）。
    //
    // 这是材质预设默认用的语义：一个霓虹管是蓝的，它发的光就该是蓝的。
    // 用中性灰白当自发光会让所有发光体褪成同一种白色，彩色霓虹就没了。
    //
    // 关键：按**通道等比**缩放，而不是把三通道都取成同一个值。
    // 表面是 (255,200,0) 的琥珀色，若三通道都取最大值会变成 (170,170,170) 的灰，
    // 色相直接丢掉；等比缩放才保色相。
    //
    // 强度可以是 0（= 不自发光），这条路径不需要调用方加分支。
    void setEmissionScaled(uint8_t r, uint8_t g, uint8_t b, uint8_t scale) {
        emR = (uint8_t)((int)r * (int)scale / 255);
        emG = (uint8_t)((int)g * (int)scale / 255);
        emB = (uint8_t)((int)b * (int)scale / 255);
    }

    void clearEmission() { emR = emG = emB = 0; }

    float iorValue() const { return 1.0f + ior / 100.0f; }
    void setIorValue(float v) {
        if (v < 1.0f) v = 1.0f;
        if (v > 3.55f) v = 3.55f;
        ior = (uint8_t)((v - 1.0f) * 100.0f + 0.5f);
    }
};

inline bool operator==(const Material& a, const Material& b) {
    return a.emR == b.emR && a.emG == b.emG && a.emB == b.emB && a.ior == b.ior;
}
inline bool operator!=(const Material& a, const Material& b) { return !(a == b); }

}  // namespace pb
