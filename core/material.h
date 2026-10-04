// core/material.h — 每像素的光学属性
//
// P1 阶段只负责**存储与编辑**这些属性；真正读取它们的是 P2 的光追通道。
// 之所以现在就把数据模型定下来，是因为把"光"和"颜色"分开是画板的核心需求，
// 事后往文档格式里塞通道代价很大。
//
// 关键设计：自发光（emission）与遮挡（albedo 的 alpha）是**两个独立的量**。
// 原实现里 alpha 同时兼任两者，导致表达不了这两种像素：
//   - 不透明但发光：自发光墙面 / 屏幕
//   - 半透明但不发光：彩色玻璃（只挡一部分光，自己不亮）
#pragma once

#include <cstdint>

namespace pb {

struct Material {
    // 自发光强度 0..255。0 = 完全不发光。
    uint8_t emission = 0;

    // 折射率，编码为 (ior - 1.0) * 100，于是 0..255 对应 ior 1.00 .. 3.55。
    // 0 表示不折射（空气或普通表面）。
    uint8_t ior = 0;

    uint8_t flags = 0;

    uint8_t reserved = 0;

    static constexpr uint8_t kReflective = 1u << 0;  // 镜面反射
    static constexpr uint8_t kSolid = 1u << 1;       // 强制完全遮挡（忽略 alpha）

    bool reflective() const { return (flags & kReflective) != 0; }
    bool solid() const { return (flags & kSolid) != 0; }
    void setReflective(bool on) { flags = on ? (uint8_t)(flags | kReflective) : (uint8_t)(flags & ~kReflective); }
    void setSolid(bool on) { flags = on ? (uint8_t)(flags | kSolid) : (uint8_t)(flags & ~kSolid); }

    float iorValue() const { return 1.0f + ior / 100.0f; }
    void setIorValue(float v) {
        if (v < 1.0f) v = 1.0f;
        if (v > 3.55f) v = 3.55f;
        ior = (uint8_t)((v - 1.0f) * 100.0f + 0.5f);
    }
};

inline bool operator==(const Material& a, const Material& b) {
    return a.emission == b.emission && a.ior == b.ior && a.flags == b.flags;
}
inline bool operator!=(const Material& a, const Material& b) { return !(a == b); }

}  // namespace pb
