// gfx/material_pack.h — 把逐像素材质打包成 GPU 用的缓冲
//
// 光照的两个通道（扫描 GI、路径追踪）都要把 Material 送进 GPU。
//
// ## 演进过程（两次压缩，思路都是"去掉不需要的东西"）
//
// 最早一个像素要 5 个量（自发光 RGB + 折射率 + 标志位），RGBA8 装不下，
// 于是用了 **RGBA32F = 16 字节/像素**。
//
// **第一次压缩**：扫描阶段不需要 ior —— ior 只在路径追踪的折射里用，
// 而扫描 GI 是直线扫描，物理上做不了弯曲光路。于是扫描侧只要 RGB+flags。
//
// **第二次压缩**：flags 里只有 kSolid 一位在用，而它是个**冗余功能**
// （效果等价于"把 alpha 设成 255"），而且**着色器从来没读过 flags**，
// 只有材质面板的 UI 和一个预览用过。删掉 flags 与从未被读过的 reserved 之后，
// 扫描侧只要 **RGB = 3 字节/像素**。
//
// 现在两条路：
//   * 扫描：`packMaterialsScan()` -> RGB8，3 字节/像素（16 -> 3，省 5.3 倍）
//   * 路径追踪：`packMaterials()` -> RGBA32F，保留 ior（精确）
//
// 两者共用同一个 `Material`，只是取用的字段不同 —— **不是冗余，是解耦**。
#pragma once

#include <vector>

#include "core/material.h"
#include "core/types.h"

namespace pb::gfx {

// ---------------------------------------------------------------------------
// 扫描 GI 用：每像素 3 字节，紧凑排列（RGBRGBRGB...）
//   R,G,B = 自发光（0..255 原始字节）
//
// 纹理用 GL_RGB8 / GL_RGB / GL_UNSIGNED_BYTE，每 texel 恰好 3 字节、无填充。
// 上传前**必须** glPixelStorei(GL_UNPACK_ALIGNMENT, 1)，否则行首会被对齐到 4 字节，
// 整张图会错位。
//
// shader 侧解码：`vec3 em = texelFetch(u_mat, sp, 0).rgb;`（已归一化到 0..1）
//
// 覆盖率不在材质里 —— 它在 canvasArray 的 alpha 通道。
// ---------------------------------------------------------------------------
inline void packMaterialsScan(const Material* mats, int w, const Rect& r,
                              std::vector<uint8_t>& out) {
    if (!mats || r.empty()) return;
    const int bw = r.width(), bh = r.height();
    out.resize((size_t)bw * (size_t)bh * 3);
    for (int y = 0; y < bh; y++) {
        const Material* src = mats + (size_t)(r.y0 + y) * (size_t)w + (size_t)r.x0;
        uint8_t* dst = out.data() + (size_t)y * (size_t)bw * 3;
        for (int x = 0; x < bw; x++) {
            dst[x * 3 + 0] = src[x].emR;
            dst[x * 3 + 1] = src[x].emG;
            dst[x * 3 + 2] = src[x].emB;
        }
    }
}

// ---------------------------------------------------------------------------
// 路径追踪用：每像素 4 个 float（保留 ior，精确）
//
// 着色器里的解码约定：
//     rgb = 自发光颜色（0..1，与 albedo 完全无关）
//     a   = iorValue
//     解码： float iorv = m.a;    —— flags 删掉之后就是直读，没有位运算了
// ---------------------------------------------------------------------------
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
            dst[x * 4 + 3] = src[x].iorValue();
        }
    }
}

}  // namespace pb::gfx
