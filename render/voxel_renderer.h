// render/voxel_renderer.h — 渲染端的对外接口
//
// GUI 端只 include 这个头和 voxel_volume.h。渲染端内部的加速结构、
// 线程池、GPU 资源都藏在实现里，GUI 不需要知道。
//
// ## 本次要渲染的东西
//
//   只渲染体素的 **+z 法相面**（即每根柱子顶面那一格的上表面），
//   输出一张 X×Y 的 2D 画布。
//   光线追踪的方法本身是**正统的 3D 空间光线追踪**（Amanatides-Woo 体素 DDA、
//   阴影射线、可用的 3D 加速结构），所以 3D 光追的优化手段都能直接套用。
#pragma once

#include <memory>
#include <vector>

#include "core/types.h"
#include "render/voxel_volume.h"

namespace pb::render {

struct RenderSettings {
    // --- 主光（太阳）---
    float sunDir[3] = {-0.4f, -0.5f, 0.75f};   // 指向**光源**的方向（世界系，z 向上）
    float sunColor[3] = {1.0f, 0.96f, 0.88f};
    float sunIntensity = 1.0f;

    // --- 天空 / 环境 ---
    float skyColor[3] = {0.16f, 0.20f, 0.28f};

    // --- 自发光 ---
    float emitScale = 1.0f;

    // --- 开关 ---
    bool shadows = true;

    // --- 全局光照的求法 ---
    enum class Gi { None, LightSampling, ConeTrace, RadianceCascades };
    Gi gi = Gi::ConeTrace;        // 默认走体素锥追踪

    // 光源采样（Gi::LightSampling 用）
    float emissiveIntensity = 1.0f;
    int lightSamples = 4;         // 每像素每帧抽多少个光源

    // 体素锥追踪（Gi::ConeTrace 用）
    int cones = 32;               // 半球上打几个锥（实测 9 个时圆环不匀度 366%，
                                  // 32 个降到 215%，64 个 54%）
    float coneAperture = 1.0f;    // 锥半角缩放：1 = 让 N 个锥刚好铺满半球（无重叠）。
                                  // 太小 -> 锥之间有空隙 -> 光斑；太大 -> 过模糊
    float coneMaxDist = 96.0f;    // 锥的最远距离
    int giMaxMip = 3;             // 最大 mip 层级：粗层会把空间上分离的东西混在一起
                                  // （板子和板上方的光源落进同一个块 -> 光"穿过"板子）

    // 体素锥追踪（Gi::RadianceCascades 用）
    //
    // cascade c：探测点间距 2^c 像素、射线数 K0·4^c、只负责距离区间 [L0·2^(c-1), L0·2^c)。
    // 距离翻倍时射线数翻 4 倍（半球的立体角），于是**每条射线覆盖的立体角恒定**
    // -> 不产生空隙（VCT 光斑的根源）。而探测点数掉 4 倍正好抵消，
    // 每个 cascade 的总射线数都等于 N·K0。
    int cascades = 7;             // cascade 层数
    float cascadeBase = 2.0f;     // 最细一层的距离区间上限 L0
    // 判据：射线角宽必须小于源的角尺寸 -> K0 >= 2*pi*L0^2。
    // L0=2 时 K0>=25，实测 K0=4 完全打不中单个体素光源（min 恒为 0），
    // K0=16/32 才对。
    int cascadeRays = 32;         // 最细一层的射线数 K0
    float giIntensity = 1.0f;     // GI 整体强度
    int giBounces = 0;            // 0 = 只收自发光（单次）；>=1 追加反照率弹射

    // --- 渐进累积 ---
    bool progressive = true;      // 帧间累积平均（噪点随时间收敛）
    int maxAccumFrames = 256;     // 累积多少帧后停（省 GPU/CPU）
};

// +z 面的 3D 光追渲染器
class VoxelRenderer {
public:
    virtual ~VoxelRenderer() = default;

    // 内容变了：重建加速结构。返回 false 表示失败。
    virtual bool setVolume(const VoxelVolume& v) = 0;

    virtual void setSettings(const RenderSettings& s) = 0;

    // 渲染：out 会 resize 成 X*Y，元素为 RGBA8（alpha 恒 255）。
    // progressive 打开时，反复调用会逐步收敛；几何/参数变了要先 resetAccumulation()。
    virtual void render(std::vector<RGBA8>& out) = 0;

    // 几何或光照参数变了：丢掉累积，重新开始收敛
    virtual void resetAccumulation() = 0;

    // 已累积的帧数（UI 显示进度用）
    virtual int accumFrames() const = 0;

    // 是否已经收敛（可以停止继续采样）
    virtual bool converged() const = 0;

    // 供 UI 显示用的统计
    virtual double lastMs() const = 0;
};

std::unique_ptr<VoxelRenderer> createVoxelRenderer();

}  // namespace pb::render
