// core/layer.h — 图层与文档
//
// ## 多图层的整体结构（决定：逐层光照 + 层间互相影响，即"模型 B / 2.5D"）
//
// 管线是**先逐层照亮、再合成**，而不是先合成再照亮：
//
//     每个 Layer 各自做平面内光追   →  每层得到"受光后的颜色"
//                                      ↓
//              按 alpha 从下往上正常合成  →  最终画面
//
// 这样图层排序 / 不透明度 / 隐藏这些行为仍然是普通 alpha 叠加、可预期的，
// 层间影响**只发生在"照亮"那一步**，不会让图层语义变得诡异。
//
// ## 为什么"层间互相影响"必须用额外的机制（这是本项目的难点）
//
// 光照目前是"光在画布平面（XY）内直线传播"，而图层是沿**视线方向（Z）**堆叠的。
// 一条反直觉但很重要的推论：**一个无限薄的层，不会挡住另一层平面内的光。**
// 所以"上层给下层投影"这个直觉，物理上并不自然成立。
//
// 模型 B 的做法是显式补上这个垂直方向的关系（详见 core/composite.h 的说明）：
//   * occlAbove[i]：第 i 层上方各层的累计遮盖 -> 给该层的平面内光线额外衰减（投影）
//   * emitFor_i   ：其他层的光透过中间层到达第 i 层的量 -> 当作额外光源（层间照亮）
// 它是**风格化**的（不是严格物理），但良定义、可预期，而且保留了现有的平面观感。
//
// 如果效果不好，备选是模型 A：把层堆当成 W x H x N 的 3D 体素网格做真 3D 光追
// （成本与现在同量级，因为 3D DDA 一条线最多穿 W+H+N 个体素），
// 但光照会变成 3D 的，现有霓虹在平面内传播的观感会改变。
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "image.h"
#include "material.h"
#include "types.h"

namespace pb {

// 图层混合模式。
//
// 目前只做 Normal —— 按"图层间互相影响"的定位，真正的层间关系由光照那一步承担，
// 而不是靠混合模式堆叠。后续要加再在这里扩展 + 在 composite 里加分支。
enum class BlendMode { Normal };

struct Layer {
    std::string name;
    Image albedo;                     // RGB = 颜色, A = 遮挡（与单层时语义一致）
    std::vector<Material> material;   // 逐像素光学属性
    bool visible = true;
    uint8_t opacity = 255;            // 整层不透明度
    BlendMode blend = BlendMode::Normal;
    bool locked = false;

    void resize(int w, int h, RGBA8 clearColor = RGBA8{0, 0, 0, 0}) {
        albedo.resize(w, h);
        albedo.fill(clearColor);
        material.assign((size_t)std::max(0, w) * (size_t)std::max(0, h), Material{});
    }

    int width() const { return albedo.width(); }
    int height() const { return albedo.height(); }
    bool empty() const { return albedo.empty(); }
    size_t pixelCount() const { return albedo.pixelCount(); }

    Material& materialAt(int x, int y) {
        return material[(size_t)y * (size_t)albedo.width() + (size_t)x];
    }
    const Material& materialAt(int x, int y) const {
        return material[(size_t)y * (size_t)albedo.width() + (size_t)x];
    }
};

// 文档 = 一组图层。索引 0 是**最底层**（与显示顺序一致：先画底，再往上叠）。
class Document {
public:
    Document() = default;
    Document(int w, int h) { resize(w, h); }

    // 改变尺寸会丢弃所有内容，并重置为单个空层
    void resize(int w, int h) {
        w_ = std::max(0, w);
        h_ = std::max(0, h);
        layers_.clear();
        layers_.emplace_back();
        layers_.back().resize(w_, h_);
        layers_.back().name = "图层 1";
        active_ = 0;
    }

    int width() const { return w_; }
    int height() const { return h_; }
    bool empty() const { return layers_.empty() || w_ <= 0 || h_ <= 0; }

    int layerCount() const { return (int)layers_.size(); }

    Layer& layer(int i) { return layers_[(size_t)std::clamp(i, 0, layerCount() - 1)]; }
    const Layer& layer(int i) const {
        return layers_[(size_t)std::clamp(i, 0, layerCount() - 1)];
    }
    std::vector<Layer>& layers() { return layers_; }
    const std::vector<Layer>& layers() const { return layers_; }

    int activeIndex() const { return active_; }
    void setActiveIndex(int i) {
        if (layers_.empty()) return;
        active_ = std::clamp(i, 0, layerCount() - 1);
    }
    Layer& active() { return layer(active_); }
    const Layer& active() const { return layer(active_); }

    // 在当前层**之上**插入一个空层并选中它，返回新层索引
    int addLayer(const std::string& name = std::string()) {
        Layer l;
        l.resize(w_, h_);
        l.name = name.empty() ? defaultName() : name;
        const int at = std::clamp(active_ + 1, 0, layerCount());
        layers_.insert(layers_.begin() + at, std::move(l));
        active_ = at;
        return at;
    }

    int duplicateLayer(int i) {
        if (layers_.empty()) return -1;
        i = std::clamp(i, 0, layerCount() - 1);
        Layer copy = layers_[(size_t)i];
        copy.name = layers_[(size_t)i].name + " 副本";
        layers_.insert(layers_.begin() + i + 1, std::move(copy));
        active_ = i + 1;
        return active_;
    }

    // 至少保留 1 层：不允许删空
    bool removeLayer(int i) {
        if (layerCount() <= 1) return false;
        i = std::clamp(i, 0, layerCount() - 1);
        layers_.erase(layers_.begin() + i);
        active_ = std::clamp(active_, 0, layerCount() - 1);
        return true;
    }

    bool moveLayer(int from, int to) {
        if (layerCount() <= 1) return false;
        from = std::clamp(from, 0, layerCount() - 1);
        to = std::clamp(to, 0, layerCount() - 1);
        if (from == to) return false;
        Layer l = std::move(layers_[(size_t)from]);
        layers_.erase(layers_.begin() + from);
        layers_.insert(layers_.begin() + to, std::move(l));
        active_ = to;
        return true;
    }

    // 把第 i 层合并到它下面一层（i 必须 >= 1）。合并后删除第 i 层。
    bool mergeDown(int i) {
        if (i < 1 || i >= layerCount()) return false;
        Layer& src = layers_[(size_t)i];
        Layer& dst = layers_[(size_t)i - 1];
        for (int y = 0; y < h_; y++) {
            for (int x = 0; x < w_; x++) {
                const size_t k = (size_t)y * (size_t)w_ + (size_t)x;
                const RGBA8 s = src.albedo.data()[k];
                if (s.a == 0) continue;   // 源是全透明：保留底下那层（含其材质）
                const RGBA8 d = dst.albedo.data()[k];
                // 标准 source-over（非预乘）
                const float sa = s.a / 255.0f;
                const float da = d.a / 255.0f * (1.0f - sa);
                const float oa = sa + da;
                RGBA8 out;
                if (oa <= 0.0f) {
                    out = RGBA8{0, 0, 0, 0};
                } else {
                    auto mixc = [&](uint8_t sc, uint8_t dc) {
                        const float v = (sc / 255.0f * sa + dc / 255.0f * da) / oa;
                        return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255);
                    };
                    out = RGBA8{mixc(s.r, d.r), mixc(s.g, d.g), mixc(s.b, d.b),
                                (uint8_t)std::clamp((int)std::lround(oa * 255.0f), 0, 255)};
                }
                dst.albedo.data()[k] = out;
                // 材质：源有覆盖就取源的（与"最上层有覆盖的像素材质胜出"一致）
                dst.material[k] = src.material[k];
            }
        }
        layers_.erase(layers_.begin() + i);
        active_ = std::clamp(i - 1, 0, layerCount() - 1);
        return true;
    }

private:
    std::string defaultName() const {
        return "图层 " + std::to_string(layerCount() + 1);
    }

    int w_ = 0, h_ = 0;
    std::vector<Layer> layers_;
    int active_ = 0;
};

}  // namespace pb
