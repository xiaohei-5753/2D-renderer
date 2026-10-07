// render/voxel_volume.h — 渲染端的输入：体素空间
//
// 这是 GUI 端与渲染端之间**唯一的数据契约**。GUI 负责把「图层」翻译成体素，
// 渲染端只管从这里读，不知道图层、画布、ImGui 的存在。
//
// 每个体素 (x,y,z)：
//   color    rgb = 表面颜色 C，a = 不透明度 A（0 = 空气）
//   emission rgb = 自发光 E（与 C 完全独立）
//
// 索引：idx(x,y,z) = (z*Y + y)*X + x
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/types.h"

namespace pb::render {

struct VoxelVolume {
    int X = 0, Y = 0, Z = 0;
    std::vector<RGBA8> color;      // C + A
    std::vector<RGBA8> emission;   // E

    void init(int x, int y, int z) {
        X = std::max(0, x);
        Y = std::max(0, y);
        Z = std::max(0, z);
        color.assign((size_t)X * (size_t)Y * (size_t)Z, RGBA8{0, 0, 0, 0});
        emission.assign(color.size(), RGBA8{0, 0, 0, 0});
    }

    bool empty() const { return X <= 0 || Y <= 0 || Z <= 0; }
    size_t idx(int x, int y, int z) const {
        return ((size_t)z * (size_t)Y + (size_t)y) * (size_t)X + (size_t)x;
    }
    bool inBounds(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < X && y < Y && z < Z;
    }

    // 该处是否实体（A > 0）
    bool solid(int x, int y, int z) const { return color[idx(x, y, z)].a > 0; }

    // 柱高 h(x,y) = 最高的实体层；没有实体返回 -1
    int columnTop(int x, int y) const {
        for (int z = Z - 1; z >= 0; z--)
            if (solid(x, y, z)) return z;
        return -1;
    }

    static float a01(const RGBA8& c) { return (float)c.a / 255.0f; }
    static float e01(const RGBA8& e) { return (float)e.r / 255.0f; }
};

}  // namespace pb::render
