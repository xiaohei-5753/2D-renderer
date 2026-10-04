// gfx/material_pack.h — 把逐像素材质打包成纹理用的 RGBA8
//
// 光照的两个通道（扫描 GI、路径追踪）都要把 Material 送进 GPU，打包规则必须一致，
// 所以抽到这里。着色器里的解码约定：
//     r = 自发光强度 (0..255)
//     g = 折射率，编码为 (ior - 1.0) * 100  ->  解码 ior = 1.0 + g * 2.55
//     b = 标志位（bit0 = 反射，bit1 = 强制实心）
//     a = 备用
#pragma once

#include <vector>

#include "core/material.h"
#include "core/types.h"

namespace pb::gfx {

// 只打包 r 指定的矩形（紧凑排列，bw*bh），避免为一次小改动准备整张图
inline void packMaterials(const Material* mats, int w, const Rect& r,
                          std::vector<RGBA8>& out) {
    if (!mats || r.empty()) return;
    const int bw = r.width(), bh = r.height();
    out.resize((size_t)bw * (size_t)bh);
    for (int y = 0; y < bh; y++) {
        const Material* src = mats + (size_t)(r.y0 + y) * (size_t)w + (size_t)r.x0;
        RGBA8* dst = out.data() + (size_t)y * (size_t)bw;
        for (int x = 0; x < bw; x++) {
            dst[x] = RGBA8{src[x].emission, src[x].ior, src[x].flags, 255};
        }
    }
}

}  // namespace pb::gfx
