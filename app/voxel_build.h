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

inline void buildVoxelVolume(const Document& doc, render::VoxelVolume& v) {
    const int W = doc.width();
    const int H = doc.height();
    const int L = doc.layerCount();
    v.init(W, H, L < 1 ? 1 : L);
    if (L < 1 || W <= 0 || H <= 0) return;

    for (int li = 0; li < L; li++) {
        const Layer& layer = doc.layer(li);   // 0 = 最底层
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const size_t k = v.idx(x, y, li);
                v.color[k] = layer.albedo.at(x, y);      // C + A
                const Material m = layer.materialAt(x, y);
                v.emission[k] = RGBA8{m.emR, m.emG, m.emB, 255};   // E
            }
        }
    }
}

}  // namespace pb::app
