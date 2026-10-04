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

    uint8_t flags = 0;

    uint8_t reserved = 0;

    static constexpr uint8_t kReflective = 1u << 0;  // 镜面反射
    static constexpr uint8_t kSolid = 1u << 1;       // 强制完全遮挡（忽略 alpha）

    bool reflective() const { return (flags & kReflective) != 0; }
    bool solid() const { return (flags & kSolid) != 0; }
    void setReflective(bool on) {
        flags = on ? (uint8_t)(flags | kReflective) : (uint8_t)(flags & ~kReflective);
    }
    void setSolid(bool on) {
        flags = on ? (uint8_t)(flags | kSolid) : (uint8_t)(flags & ~kSolid);
    }

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

    void clearEmission() { emR = emG = emB = 0; }

    float iorValue() const { return 1.0f + ior / 100.0f; }
    void setIorValue(float v) {
        if (v < 1.0f) v = 1.0f;
        if (v > 3.55f) v = 3.55f;
        ior = (uint8_t)((v - 1.0f) * 100.0f + 0.5f);
    }
};

inline bool operator==(const Material& a, const Material& b) {
    return a.emR == b.emR && a.emG == b.emG && a.emB == b.emB && a.ior == b.ior &&
           a.flags == b.flags;
}
inline bool operator!=(const Material& a, const Material& b) { return !(a == b); }

}  // namespace pb
