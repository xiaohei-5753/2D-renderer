// core/composite.h — 图层合成，以及"层间光照"需要的辅助通道
//
// 这一层是纯 CPU、可 headless 验证的，而且**两个光照方案都要它**：
//   * 显示：无论如何都需要把图层按 alpha 压平成一张图
//   * 模型 B：需要下面两张逐层通道（occlAbove / emitFor）
//   * 模型 A：压平的结果就是"相机沿 -Z 看到的第一个不透明体素"，同样要用
//
// ## 为什么需要 occlAbove / emitFor（回顾一下难点）
//
// 光照在画布平面(XY)内传播，图层沿视线方向(Z)堆叠，
// 而**无限薄的层不会挡住另一层平面内的光** —— 所以"上层给下层投影"不会自然发生。
// 模型 B 显式补上这个垂直方向的关系：
//
//   occlAbove[i](x,y) = 1 - Π(k>i) (1 - cov_k(x,y))
//       第 i 层**上方**所有层的累计遮盖。用它给该层的平面内光线额外衰减 = 投影。
//
//   emitFor_i(x,y)    = Σ(k≠i) em_k(x,y) · Π(中间层的 (1-cov))
//       其他层的光透过**中间层**到达第 i 层的量。把它当作第 i 层的额外光源 = 层间照亮。
//       这里的"中间层"是 i 与 k 之间真正夹着的那些层，所以下层的光能照上来、
//       上层的光也能照下去，且都会被子透明层染色/衰减。
//
// 这是**风格化**的模型（不是严格物理），但良定义、可预期，且保留了平面观感。
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "image.h"
#include "layer.h"
#include "material.h"
#include "types.h"

namespace pb {

// 把图层从下往上按 alpha 压平。
//
// 材质合并规则：**最上层有覆盖的像素，其材质胜出**（简单、可预期）。
// outAlbedo / outMaterial 必须已经是 doc 的尺寸；只重算 region 指定的区域。
//
// 调用方负责两件事：先取走 Document::takeDirtyUnion() 决定 region，
// 再把这次压平的范围标到目标缓冲的脏矩形上（这样它才知道要往 GPU 传哪一块）。
// 具体怎么串见 app/main.cpp 的 syncComposite()。
void flattenDocument(const Document& doc, Image& outAlbedo,
                     std::vector<Material>& outMaterial, const Rect& region);

// 第 i 层上方的累计遮盖（0 = 完全通透，255 = 完全被盖住）
void buildOcclAbove(const Document& doc, int layerIndex, const Rect& region,
                    std::vector<uint8_t>& out);

// 其他层的光透过中间层到达第 i 层的量（逐通道 RGB）
void buildEmitFor(const Document& doc, int layerIndex, const Rect& region,
                  std::vector<RGBA8>& out);

}  // namespace pb
