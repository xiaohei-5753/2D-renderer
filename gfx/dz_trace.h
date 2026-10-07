// gfx/dz_trace.h — 2.5D 光线追踪（按规格直接实现「柱顶曲面 + 上半球辐照度」）
//
//   输入：体素空间 V（X×Y×Z），每个体素有 solid / C / A / E
//   输出：X×Y 的 2D 画布
//
//   对每个 (x,y)：h = 最高的实体 z；在 p=(x, y, h+1) 处求**上半球**入射辐照度
//   I(p)；最终 Color = mix(I + E_top, C_top, A_top)。
//
// 方向集合用 Bresenham 中点圆（规格第七节：不约分、不去重），
// 射线步进用整数 Bresenham，z 每步 +1（保证只取上半球）。
#pragma once

#include "core/voxel.h"
#include "gl_common.h"

namespace pb::gfx {

struct DzTraceParams {
    int radius = 128;                 // 方向集合半径（中点圆）
    float sunCol[3] = {1.0f, 0.97f, 0.9f};   // 出界看到的"太阳"颜色
    float wallCol[3] = {0.25f, 0.27f, 0.32f}; // 其他方向的背景（墙/环境）
    float sunDir[2] = {0.7f, 0.7f};          // 太阳方向的 xy 分量
    float sunSoft = 0.15f;                   // 太阳角大小：1-dot <= sunSoft 判为太阳
    float emitScale = 1.0f;
    // 射线的"上爬率"：每走 1 个水平像素，z 上升多少。
    // 决定光晕铺多开：= 场景高度/半径 时，射线在半径范围内扫过整个高度带。
    int zRate = 1;   // 0 = 诊断模式（把 h 与 A 直接输出）
};

class DzTrace {
public:
    ~DzTrace();

    bool init(int x, int y, int z);
    void shutdown();
    bool valid() const { return prog_ != 0; }

    void uploadVolume(const VoxelGrid& g);
    bool render(const DzTraceParams& prm);

    GLuint resultTexture() const { return outTex_; }

private:
    int X_ = 0, Y_ = 0, Z_ = 0;
    GLuint voxTex_ = 0;     // RGBA8 3D：C + A
    GLuint emitTex_ = 0;    // RGBA8 3D：E
    GLuint outTex_ = 0;     // RGBA8 2D：输出画布
    GLuint prog_ = 0;
};

}  // namespace pb::gfx
