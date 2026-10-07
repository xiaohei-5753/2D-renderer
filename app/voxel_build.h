// app/voxel_build.h — 把多图层文档构建成 2.5D 光追要的体素空间
//
// 图层就对应 z：第 i 个图层（0 基）放在 z = i。每个体素直接取该图层在该处的
// 像素与材质 —— 完全如实，不做任何压平。
//
// 这样"图层"就真的是 3D 体素空间里的一层，2.5D 光追才能算出：
//   * 柱高 h(x,y) = 最高的不透明/有像素的层
//   * 上层的光源照亮下层的柱顶
#pragma once

#include <vector>

#include "core/layer.h"
#include "core/voxel.h"

namespace pb::app {

// 把 doc 的所有图层灌进网格。图层数就是 Z。
inline void buildVoxelGrid(const Document& doc, VoxelGrid& g) {
    const int W = doc.width();
    const int H = doc.height();
    const int L = doc.layerCount();
    g.init(W, H, L < 1 ? 1 : L);
    if (L < 1 || W <= 0 || H <= 0) return;

    for (int li = 0; li < L; li++) {
        const Layer& layer = doc.layer(li);   // 0 = 最底层
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const RGBA8 c = layer.albedo.at(x, y);
                const Material m = layer.materialAt(x, y);
                Voxel& v = g.at(x, y, li);
                v.color = c;      // rgb = 表面颜色 C，a = 不透明度 A
                v.mat = m;        // emR/emG/emB = 自发光 E
            }
        }
    }
}

}  // namespace pb::app
