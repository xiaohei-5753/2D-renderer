// core/dz25slice.h — 2.5D 光追：截面 + 「顶边→底边」斜线扫描
//
// ## 和"圆周采样"的区别
//
//   之前是在截面上用中点圆生成方向 (Δd, Δz) 一条条射线去射。
//   薄光源在**角度空间是零测度**的：离散方向必然漏掉它，于是紧贴光源处发黑、
//   隔一段才亮 —— 条带。
//
//   本文件改成按**端点**采样：一条斜线 = 顶边上一点 + 底边上一点。
//   过任意（接收点, 光源）的直线与顶边恰好交于一点，所以把顶边采遍，
//   每个光源都会被某条斜线穿过 —— 覆盖是完整的。
//
// ## 截面
//
//   宽 W = 沿扫描方向的长度，高 H = 层数 + 1。
//   最后一行 H-1 是**天空行**（最高层之上加的那一层）：光从那里进入截面。
//
// ## 斜线扫描
//
//   固定斜率 τ（顶边点相对底边点的横向偏移，跨越 H-1 层），
//   该斜率下所有斜线**互相平行**，一次扫描把它们全走完：
//
//       线：(k, H-1)  →  (k - τ, 0)        k 取遍（含平面外，见"延长"）
//
//   沿线从顶往底走，维护 carry：
//       到达格子：记录辐照度（若该格是接收点）→ carry = carry*(1-A) + E
//
//   * 每个 τ 的成本 O(W·H)，τ 取值 O(W·H) → 每个截面 O(W²H²)
//   * 侧边入射：k 的取值范围**超出** [0,W)，这就是"延长顶边采样面"，
//     让比顶边还平的斜线也能从左右侧边进入
#pragma once

#include <cstdint>
#include <vector>

#include "core/types.h"
#include "core/voxel.h"

namespace pb {

struct Dz25SliceParams {
    float emitScale = 1.0f;
    bool ambientOn = true;
    float ambient[3] = {0.0f, 0.0f, 0.0f};
    int directions = 4;      // 使用的截面方向数（1..4：+x/-x/+y/-y）
};

// 驱动：对若干水平方向建截面、做斜线扫描、投影回 xy 画布
void trace25DSlice(const VoxelGrid& g, const Dz25SliceParams& prm,
                   std::vector<RGBA8>& out);

// 斜线扫描（单独暴露，便于测试）：算出每列"柱顶正上方"的辐照度（irr 大小 W*3）
void sliceScan(const std::vector<RGBA8>& col, const std::vector<RGBA8>& emis, int W, int H,
               const Dz25SliceParams& prm, std::vector<float>& irr);

}  // namespace pb
