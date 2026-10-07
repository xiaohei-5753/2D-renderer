// gfx/dz_sweep.h — 把「d-z 平面 2D 光追」套到现有 GPU 扫描线上
//
// ## 整体形状（两个扫描线 + 合成，就是这样）
//
//   第 1 条扫描线（build）：生成 d-z 位图。
//       对每个收集方向 ω，沿 ω 采样画布，得到一张 2D 位图：
//           横轴 u = 沿 ω 的位置（0..len-1）
//           纵轴 z = 图层高度（0..L-1，位图行 row = L-1-z）
//       每格放「第 z 层在该采样点上的像素」：表面那格带真实颜色与自发光，
//       其下各层只负责遮挡。**每一格都是一个像素**。
//
//   第 2 条扫描线（trace）：在 d-z 位图上跑完整的 2D 光线追踪。
//       逐格 DDA 推进，每命中一格累积一次：
//           lt = lt * mix(1, albedo, cov) * (1-cov) + em
//       与现有 2D 光追同一套推进与累积，只是网格换成了 d-z。
//
//   合成（resolve）：取每列「最高点加一」那一格的受光，
//       写回它在 (x,y) 上对应的像素，按方向数取平均。
//
// ## 为什么这样就对
//
//   * 位图里每格都是像素，遮挡/穿透/自发光天然正确，不需要任何
//     "高度/可见度"的特殊语义（那些语义正是我先前反复出错的地方）。
//   * 推进是逐格 DDA，不是参数化射线：不会漏薄特征、不会在边界抖动。
#pragma once

#include <cstdint>
#include <vector>

#include "core/material.h"
#include "core/types.h"
#include "gl_common.h"

namespace pb::gfx {

// 上表面场：压平后的画布 + 每个像素"被填到哪一层"。
struct SurfaceField {
    int w = 0, h = 0, layers = 0;
    std::vector<RGBA8> albedo;       // w*h，压平后的颜色
    std::vector<Material> material;  // w*h
    std::vector<uint8_t> height;     // w*h，覆盖该像素的最高层号（0 = 空气）
};

struct DzSweepParams {
    int directions = 24;
    float sky[3] = {0.35f, 0.38f, 0.45f};   // 天空初始值（环境光）
    float emitScale = 1.0f;
    // z 轴斜射程度：沿 d 每走 stepsPerLevel 格、射线上升 1 层。
    // 0 = 垂直入射（影子零偏移）；越大影子拉得越长。
    float stepsPerLevel = 4.0f;
};

class DzSweep {
public:
    ~DzSweep();

    bool init(int w, int h, int layers);
    void shutdown();
    bool valid() const { return albedoTex_ != 0; }

    void uploadField(const SurfaceField& f);

    // 跑完「两个扫描线 + 合成」，结果在 resultTexture()。
    bool render(const DzSweepParams& prm);

    GLuint resultTexture() const { return outTex_; }

private:
    void releaseTextures();
    void ensureBitmap(int len, int L, int dirs);

    int w_ = 0, h_ = 0, layers_ = 0;

    GLuint albedoTex_ = 0;    // RGBA8 : 压平后的 albedo
    GLuint matTex_ = 0;       // RGBA32F: 自发光 RGB + (ior + flags*8)
    GLuint heightTex_ = 0;    // R8UI  : 表面高度（层号）
    GLuint outTex_ = 0;       // RGBA8 : 最终输出
    GLuint accSSBO_ = 0;      // 定点累积（原子加，按方向累加受光）

    // d-z 位图（每个方向一层）
    GLuint pixArray_ = 0;     // RGBA8  : 每格的像素颜色（表面格带自发光）
    GLuint flagArray_ = 0;    // R8UI   : bit0 有像素, bit1 表面格
    GLuint lightTex_ = 0;     // RGBA16F: 每格的受光（2D 光追结果）
    int bitmapW_ = 0, bitmapH_ = 0, bitmapD_ = 0;

    GLuint buildProg_ = 0, traceProg_ = 0, resolveProg_ = 0, accumProg_ = 0;
};

}  // namespace pb::gfx
