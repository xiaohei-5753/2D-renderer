// core/dz25.h — 2.5D 光追的**正向层间递推**实现（纯 CPU，可 headless 验证）
//
// ## 为什么不用"逐像素反向投射"
//
//   反向投射要为每个表面点枚举一个**离散方向集合** `(dx,dy,dz)`。
//   能不能打到光源，取决于射线坡度是否正好落在某个离散方向上。
//   z 方向尤其糟：Z=4 时 dz 只有 1..3，而水平半径 R 一大，
//   坡度间隔就是 1/R —— 中间会整片打不到光源。
//   表现：**紧贴光源处发黑、隔一段才亮**，即条带状伪影。
//   加抖动只能把相干条带打散成噪声，治不了"中间空掉"。
//
// ## 正向层间递推（本文件）
//
//   规格第九节：每体素每方向只访问一次，O(K·X·Y·Z)。
//   固定水平方向 (dx,dy) 与斜率 S（每上升一层横向走 S 格）后，
//   第 z 层格子的光只依赖**第 z-1 层的斜上方 S 格**：
//
//       lt(z,x,y) = 上游经过 (z,x,y) 的介质之后的值
//       上游 = (z==0) ? 出界光 : lt(z-1, x-S*sx, y-S*sy)
//
//   所以按层递增做一遍 DP 即可 —— **层内没有依赖**，任意顺序都对。
//   每个格子都被算到，没有空洞。
//
//   上色仍按规格第六/十三节的多层累积：
//       Color = Σ_i T_i·(I0·A_i·C_i + E_i)，  I0 = 柱顶正上方的辐照度
#pragma once

#include <cstdint>
#include <vector>

#include "core/types.h"
#include "core/voxel.h"

namespace pb {

struct Dz25Params {
    float emitScale = 1.0f;
    // 只收集这个水平半径内的发光体素（0 = 不限）。远处的贡献被 1/r³ 压得很小。
    int radius = 0;
    // 辐照度增益：把朗伯权重 cosθ/r² 缩放到与方向采样版本相近的亮度。
    float gain = 400.0f;
    // 距离衰减指数：2 = 1/r²（物理平方反比，衰减快、光晕窄）；
    // 1 = 1/r（2D 泛光惯例，铺得开）。像素画要的是后者。
    float falloff = 1.0f;
    // 出界天光：解析版里天光就是"环境项"，直接加在辐照度上（可选）。
    bool ambientOn = false;
    float ambient[3] = {0.0f, 0.0f, 0.0f};
};

// 统计（调优用）：过距离检查的对数 / 细步进步数 / 块级粗判步数
void dz25Stats(long long* pairs, long long* steps, long long* coarse);
void dz25ResetStats();

// 输出：X×Y 的 RGB 画布（RGBA8，alpha = 255）
void trace25D(const VoxelGrid& g, const Dz25Params& prm, std::vector<RGBA8>& out);

}  // namespace pb
