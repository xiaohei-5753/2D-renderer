// core/composite.h — 图层压平（把图层栈按 alpha 合成成一张图）
//
// 这一层是纯 CPU、可 headless 验证的。
//
// ## 它在现在这套光照里的位置
//
// 光照是**逐层**做的（每层各自照亮，再由 GPU 的 LayerComposite 用"光场"把各层
// 合起来），所以 CPU 侧的压平与光照无关，只服务两件事：
//   * 导出 PNG（"原始图"）：导出的应该就是用户看到的那张压平结果
//   * 无 GPU 的单测：验证 source-over 的语义
//
// ## 历史：曾经还有两条"层间光照通道"
//
// 早先试过一个叫模型 B 的方案，额外构造两张逐层通道：
//
//   occlAbove[i] = 1 - Π(k>i) (1 - cov_k)      第 i 层**上方**各层的累计遮盖
//   emitFor_i    = Σ(k≠i) em_k · Π(中间层的 (1-cov))   其他层透过中间层照过来的光
//
// 用来表达"上层给下层投影"和"层间互相照亮"。**这条路已经删掉了**，原因：
// 它会把"其他层透过来的光"塞进每一层的受光结果，于是**空图层也有值** ——
// 层数一多，画面就越来越亮（这是实际出现过的 bug）。而现在的逐层光场模型
// 本来就要求"每层做一轮独立的光照"，两者不能并存。详见 README 的「光照模型」。
//
// 更彻底的备选是模型 A：把层堆当成 W x H x N 的 3D 体素网格做真 3D 光追。
// **这条路已经试过并归档**（见 archive/README.md）——它能拿到分图层的光线遮挡，
// 但整体光效不如平面内光追，为此牺牲不划算。
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
void flattenDocument(const Document& doc, Image& outAlbedo,
                     std::vector<Material>& outMaterial, const Rect& region);

}  // namespace pb
