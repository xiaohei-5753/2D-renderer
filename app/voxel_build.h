// app/voxel_build.h — GUI 端 → 渲染端的翻译层
//
// GUI 只有「图层」的概念；渲染端只认体素空间。
// 这个文件就是那条边界：第 i 个图层（0 基）放在 z = i，逐像素如实搬运。
//
// 这是**唯一**该做这件事的地方 —— 渲染端不知道图层存在，GUI 不知道体素结构。
#pragma once

#include "core/layer.h"
#include "render/voxel_volume.h"

namespace pb::app {

// stride：每个图层在 z 方向占几格（层与层之间留空格）。
//
// 为什么需要它：图层是画布的语义，一格一层的话，上一层的光源只比下层表面高
// 半个格子 —— 对 +z 法相面来说几乎是掠射（cosθ ≈ 0.5/r），照不亮任何东西。
// 给层留出厚度后，上层光源真正"在上方"，光照才出得来。
inline void buildVoxelVolume(const Document& doc, render::VoxelVolume& v,
                             int layerStride = 4) {
    const int W = doc.width();
    const int H = doc.height();
    const int L = doc.layerCount();
    const int st = layerStride < 1 ? 1 : layerStride;
    const int Z = (L < 1 ? 1 : L) * st;
    v.init(W, H, Z);
    if (L < 1 || W <= 0 || H <= 0) return;

    for (int li = 0; li < L; li++) {
        const Layer& layer = doc.layer(li);   // 0 = 最底层
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const size_t k = v.idx(x, y, li * st);
                v.color[k] = layer.albedo.at(x, y);      // C + A
                const Material m = layer.materialAt(x, y);
                v.emission[k] = RGBA8{m.emR, m.emG, m.emB, 255};   // E
            }
        }
    }
}

}  // namespace pb::app
