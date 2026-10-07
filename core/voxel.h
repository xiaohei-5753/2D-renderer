// core/voxel.h — 2.5D 光线追踪的体素空间（按规格第一节）
//
// 输入是一个三维体素空间 V，尺寸 X × Y × Z。每个体素 (x,y,z) 有：
//   solid      是否实体
//   C          表面颜色 (r,g,b)
//   A          不透明度 [0,1]
//   E          自发光 (lr,lg,lb)
//
// 这里把它压成三张连续的数组，索引 idx(x,y,z) = (z*Y + y)*X + x。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "material.h"
#include "types.h"

namespace pb {

struct Voxel {
    RGBA8 color{};      // C（alpha 就是 A：0 = 空气，255 = 实体）
    Material mat{};     // E（emR/emG/emB）与 flags

    bool solid() const { return color.a > 0; }
    float a01() const { return (float)color.a / 255.0f; }
    float er01() const { return (float)mat.emR / 255.0f; }
    float eg01() const { return (float)mat.emG / 255.0f; }
    float eb01() const { return (float)mat.emB / 255.0f; }
};

class VoxelGrid {
public:
    int X = 0, Y = 0, Z = 0;

    void init(int x, int y, int z) {
        X = std::max(0, x);
        Y = std::max(0, y);
        Z = std::max(0, z);
        vox_.assign((size_t)X * (size_t)Y * (size_t)Z, Voxel{});
    }

    size_t idx(int x, int y, int z) const {
        return ((size_t)z * (size_t)Y + (size_t)y) * (size_t)X + (size_t)x;
    }
    bool inBounds(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < X && y < Y && z < Z;
    }
    Voxel& at(int x, int y, int z) { return vox_[idx(x, y, z)]; }
    const Voxel& at(int x, int y, int z) const { return vox_[idx(x, y, z)]; }
    Voxel& atIdx(size_t i) { return vox_[i]; }
    const Voxel& atIdx(size_t i) const { return vox_[i]; }
    const Voxel* data() const { return vox_.data(); }

    // 柱高 h(x,y) = max{ z | V(x,y,z) 是实体 }，没有实体时返回 -1
    int columnHeight(int x, int y) const {
        for (int z = Z - 1; z >= 0; z--)
            if (at(x, y, z).solid()) return z;
        return -1;
    }

    // 上限：A（不透明度）直接存体素的 alpha。
    static constexpr float kEps = 1e-3f;

private:
    std::vector<Voxel> vox_;
};

}  // namespace pb
