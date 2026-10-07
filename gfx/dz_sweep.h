// gfx/dz_sweep.h — d-z 平面光追：只做搬运，射线交给现有引擎
//
// ## 形状
//
//   d-z 位图 = **d 的长度 × 层数** 的窄而宽位图：
//       第 1 层合成的 z 坐标就是 1，第 2 层是 2，依此类推。
//   这张位图**本身就是一个多图层文档**，所以直接把它喂给现有的 2D 光线追踪
//   引擎（gfx::LightSweep），射线推进 / 遮挡 / 累积 / 合成全部由引擎负责。
//
//   本模块只做三件事：
//     ① 按收集方向 ω 把世界采样成 d-z 位图（一个 compute pass）
//     ② 把位图逐层交给引擎渲染
//     ③ 取「每列最高点加一」那一格的受光，写回画布对应像素并按方向累加
//
// ## 斜射怎么体现
//
//   引擎在它的网格里走 45° 射线。所以斜射由**位图横向拉伸**表达：
//   位图横向 = len * S 列（S = stepsPerLevel），一层竖向 1 格对应横向 S 格，
//   于是引擎的 45° 等价于真实世界的斜率 —— 不需要给引擎加任何参数。
#pragma once

#include <cstdint>
#include <vector>

#include "core/material.h"
#include "core/types.h"
#include "gl_common.h"

namespace pb::gfx {

class LightSweep;

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
};

class DzSweep {
public:
    ~DzSweep();

    bool init(int w, int h, int layers);
    void shutdown();
    bool valid() const { return albedoTex_ != 0; }

    // 采样半径 m：喂给引擎方向表的半径。方向上可用的斜率由它决定
    // （半径 m 的圆上那些整数点里就含 (1, S) 这类斜率，所以影子长度由它管）。
    void setRayRadius(int m) { m_ = m; }

    void uploadField(const SurfaceField& f);

    // 跑完整流程，结果在 resultTexture()。
    bool render(const DzSweepParams& prm);

    GLuint resultTexture() const { return outTex_; }


private:
    void releaseTextures();
    GLuint readBackBitmap(int W, int H);

    int w_ = 0, h_ = 0, layers_ = 0;
    int m_ = 128;             // 采样半径（方向表半径）
    GLuint albedoTex_ = 0;    // RGBA8 : 压平后的 albedo
    GLuint matTex_ = 0;       // RGBA32F: 自发光 RGB + (ior + flags*8)
    GLuint heightTex_ = 0;    // R8UI  : 表面层号
    GLuint outTex_ = 0;       // RGBA8 : 最终输出
    GLuint accSSBO_ = 0;      // 定点累积（按方向累加受光）
    GLuint pixTex_ = 0;       // RGBA8 : 截面位图（本方向的）
    GLuint litTex_ = 0;       // RGBA8 : 引擎在截面上的渲染结果
    int bitmapW_ = 0, bitmapH_ = 0;
    SurfaceField field_;      // 投影/上色在 CPU 上做，留一份输入
    GLuint buildProg_ = 0;
    LightSweep* engine_ = nullptr;   // 现有 2D 光追引擎（射线全交给它）
};

}  // namespace pb::gfx
