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

    // --- 采样 ---
    int samples = 1;          // 每像素主光采样数（>1 走软阴影）
    float sunRadius = 0.0f;   // 主光角半径（0 = 硬阴影）
};

// +z 面的 3D 光追渲染器
class VoxelRenderer {
public:
    virtual ~VoxelRenderer() = default;

    // 内容变了：重建加速结构。返回 false 表示失败。
    virtual bool setVolume(const VoxelVolume& v) = 0;

    virtual void setSettings(const RenderSettings& s) = 0;

    // 渲染：out 会 resize 成 X*Y，元素为 RGBA8（alpha 恒 255）
    virtual void render(std::vector<RGBA8>& out) = 0;

    // 供 UI 显示用的统计
    virtual double lastMs() const = 0;
};

std::unique_ptr<VoxelRenderer> createVoxelRenderer();

}  // namespace pb::render
