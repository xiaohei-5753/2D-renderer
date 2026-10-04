// gfx/material_pack.h — 把逐像素材质打包成 GPU 用的缓冲
//
// 光照的两个通道（扫描 GI、路径追踪）都要把 Material 送进 GPU，打包规则必须一致。
//
// 为什么用 float 而不是 RGBA8：完全解耦之后，一个像素需要 **5 个量** ——
// 自发光 RGB（3）+ 折射率（1）+ 标志位（1）。RGBA8 只有 4 个通道，
// 硬塞就得把折射率量化到 6 位。用 RGBA32F 一像素 4 个 float，
// 把折射率与标志位合成到第 4 个通道即可，精确且不需要第二张纹理。
//
// 着色器里的解码约定：
//     rgb = 自发光颜色（0..1，与 albedo 完全无关）
//     a   = iorValue + flags * 8.0        （iorValue ∈ [1.0, 3.55]，间距 8 足够分开）
//     解码： flags = floor(a/8 + 0.5);  ior = a - flags*8
#pragma once

#include <vector>

#include "core/material.h"
#include "core/types.h"

namespace pb::gfx {

// 只打包 r 指定的矩形（紧凑排列，bw*bh*4 个 float）
inline void packMaterials(const Material* mats, int w, const Rect& r,
                          std::vector<float>& out) {
    if (!mats || r.empty()) return;
    const int bw = r.width(), bh = r.height();
    out.resize((size_t)bw * (size_t)bh * 4);
    const float inv255 = 1.0f / 255.0f;
    for (int y = 0; y < bh; y++) {
        const Material* src = mats + (size_t)(r.y0 + y) * (size_t)w + (size_t)r.x0;
        float* dst = out.data() + (size_t)y * (size_t)bw * 4;
        for (int x = 0; x < bw; x++) {
            dst[x * 4 + 0] = src[x].emR * inv255;
            dst[x * 4 + 1] = src[x].emG * inv255;
            dst[x * 4 + 2] = src[x].emB * inv255;
            dst[x * 4 + 3] = src[x].iorValue() + (float)(src[x].flags) * 8.0f;
        }
    }
}

}  // namespace pb::gfx
