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
//
// ## 为什么 Layer 自己带绘制原语与脏矩形
//
// 改造前 `Canvas` 一个类同时扮演两个角色："可绘制的表面"和"送去光照的那张图"。
// 有了图层之后这两个角色必须分开：
//   * **可绘制的表面**是单个图层 —— 工具、洪水填充、History 全都只碰它
//   * **送去光照的图**是所有图层压平的结果 —— 由 Canvas 扮演（见 composite.h）
// 两边都保留同一套原语，是为了让工具代码不需要知道自己写的是图层还是别的什么；
// 代价是一份有意的、受控的重复（两个类的语义不同，硬抽公共基类反而会绑死它们）。
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

// 一个可绘制的像素表面：albedo + 逐像素材质 + 脏矩形。
//
// alpha 语义与单层时代完全一致（这也是光照模型的枢纽）：
//   a == 0     空气，光直接穿过
//   0 < a < 1  半透明介质：让光穿过，同时按 emission 自发光（霓虹管、玻璃）
//   a == 255   实心不透明：挡住光，画面直接显示自身颜色（墙、实体）
struct Layer {
    std::string name;
    Image albedo;                     // RGB = 颜色, A = 遮挡
    std::vector<Material> material;   // 逐像素光学属性
    bool visible = true;
    uint8_t opacity = 255;            // 整层不透明度
    BlendMode blend = BlendMode::Normal;
    bool locked = false;

    // 内容版本号：每次内容被改动都自增。图层面板靠它判断缩略图要不要重传 GPU，
    // 逐层光照靠它判断"这一层要不要重新照亮"。
    //
    // ⚠️ 它必须由**所有**改动路径维护，否则会出一个很隐蔽的 bug：漏掉一条路径，
    // 那一类笔画就永远不会被认作"变了"。我就在这里栽过 —— 一开始只在
    // clear/fill/fillRect/drawLine 这些"整块"操作里自增，忘了 setPixelRaw /
    // markDirty，而铅笔笔画走的正是 `stampBrush -> setPixelRaw + markDirty`，
    // 于是 revision 恒定不变、光照永远不重算，表现就是**画完之后画布全黑**
    // （笔迹在文档里，撤销栈也有记录，只是从没被送去光照）。
    //
    // 所以现在统一在 markDirty / markPixelDirty / markAllDirty 里自增：
    // 任何"标脏"的语义就是"内容变了"，版本号跟它走最不容易漏。
    uint32_t revision = 1;

    void resize(int w, int h, RGBA8 clearColor = RGBA8{0, 0, 0, 0}) {
        albedo.resize(w, h);
        albedo.fill(clearColor);
        material.assign((size_t)std::max(0, w) * (size_t)std::max(0, h), Material{});
        markAllDirty();
    }

    int width() const { return albedo.width(); }
    int height() const { return albedo.height(); }
    bool empty() const { return albedo.empty(); }
    size_t pixelCount() const { return albedo.pixelCount(); }

    // --- 像素访问 ---
    Image& image() { return albedo; }
    const Image& image() const { return albedo; }

    Material* materialData() { return material.data(); }
    const Material* materialData() const { return material.data(); }

    Material& materialAt(int x, int y) {
        return material[(size_t)y * (size_t)albedo.width() + (size_t)x];
    }
    const Material& materialAt(int x, int y) const {
        return material[(size_t)y * (size_t)albedo.width() + (size_t)x];
    }

    // --- 脏矩形 ---
    //
    // 标记脏 = 内容变了，所以这里同时自增 revision（见上面关于 revision 的说明）。
    // 这是"版本号"的唯一维护点，所有绘制原语都经由它，不会漏。
    void markDirty(const Rect& r) {
        if (r.empty()) return;
        dirty_.add(r);
        revision++;
    }
    void markPixelDirty(int x, int y) {
        dirty_.add(x, y);
        revision++;
    }
    void markAllDirty() {
        dirty_.add(0, 0, albedo.width() - 1, albedo.height() - 1);
        revision++;
    }
    const Rect& dirty() const { return dirty_; }
    bool hasDirty() const { return !dirty_.empty(); }
    Rect takeDirty() {
        Rect r = dirty_;
        dirty_.reset();
        return r;
    }
    void clearDirty() { dirty_.reset(); }

    // --- 绘制原语（与 Canvas 的那一套语义完全相同）---
    bool setPixel(int x, int y, RGBA8 c, Material m = Material{}) {
        if (x < 0 || y < 0 || x >= width() || y >= height()) return false;
        setPixelRaw(x, y, c, m);
        markPixelDirty(x, y);
        return true;
    }

    void setPixelRaw(int x, int y, RGBA8 c, Material m = Material{}) {
        const size_t i = (size_t)y * (size_t)albedo.width() + (size_t)x;
        albedo.data()[i] = c;
        material[i] = m;
    }

    void clear(RGBA8 c = RGBA8{0, 0, 0, 0}, Material m = Material{}) {
        albedo.fill(c);
        for (auto& e : material) e = m;
        markAllDirty();
    }

    void fill(RGBA8 c, Material m = Material{}) { clear(c, m); }

    void fillRect(int x0, int y0, int x1, int y1, RGBA8 c, Material m = Material{}) {
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(width() - 1, x1);
        y1 = std::min(height() - 1, y1);
        if (x1 < x0 || y1 < y0) return;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) setPixelRaw(x, y, c, m);
        markDirty(Rect{x0, y0, x1, y1});
    }

    void rectOutline(int x0, int y0, int x1, int y1, RGBA8 c, Material m = Material{}) {
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        x0 = std::max(0, x0);
        y0 = std::max(0, y0);
        x1 = std::min(width() - 1, x1);
        y1 = std::min(height() - 1, y1);
        if (x1 < x0 || y1 < y0) return;
        for (int x = x0; x <= x1; x++) {
            setPixelRaw(x, y0, c, m);
            setPixelRaw(x, y1, c, m);
        }
        for (int y = y0; y <= y1; y++) {
            setPixelRaw(x0, y, c, m);
            setPixelRaw(x1, y, c, m);
        }
        markDirty(Rect{x0, y0, x1, y1});
    }

    // 端点规范化后画线：从左往右拖和从右往左拖必须得到同一条线
    // （裸 Bresenham 在误差平局时固定偏向一侧，这是它的固有性质，见 canvas.cpp）
    void drawLine(int x0, int y0, int x1, int y1, RGBA8 c, Material m = Material{});
    void drawEllipse(int x0, int y0, int x1, int y1, bool filled, RGBA8 c,
                     Material m = Material{});
    int floodFill(int x, int y, RGBA8 c, Material m = Material{});

private:
    Rect dirty_;
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

    // 当前层是否是"可写的"。锁定层与隐藏层不接受笔触：
    // 往看不见的层上画是纯粹的挫败来源，所以宁可挡住。
    bool activeWritable() const {
        const Layer& L = active();
        return L.visible && !L.locked;
    }

    // ------------------------------------------------------------------
    // 结构改动：这些会改变"哪些图层参与合成"，所以调用方必须重算整张合成结果
    // ------------------------------------------------------------------

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
        Layer copy = layers()[(size_t)i];
        copy.name = layers()[(size_t)i].name + " 副本";
        copy.markAllDirty();
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
        Layer l = std::move(layers()[(size_t)from]);
        layers_.erase(layers_.begin() + from);
        layers_.insert(layers_.begin() + to, std::move(l));
        active_ = to;
        return true;
    }

    // 把第 i 层合并到它下面一层（i 必须 >= 1）。合并后删除第 i 层。
    bool mergeDown(int i) {
        if (i < 1 || i >= layerCount()) return false;
        Layer& src = layers()[(size_t)i];
        Layer& dst = layers()[(size_t)i - 1];
        const float srcOp = src.opacity / 255.0f;
        for (int y = 0; y < h_; y++) {
            for (int x = 0; x < w_; x++) {
                const size_t k = (size_t)y * (size_t)w_ + (size_t)x;
                const RGBA8 s = src.albedo.data()[k];
                const float sa = s.a / 255.0f * srcOp;
                if (sa <= 0.0f) continue;   // 源是全透明：保留底下那层（含其材质）
                const RGBA8 d = dst.albedo.data()[k];
                // 标准 source-over（非预乘）
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
        // 合并后整层都可能变（源层的不透明度被折进去了），且尺寸不变，
        // 所以直接标整层脏就够了——这里没有"只影响局部"的快捷路径。
        dst.markAllDirty();
        layers_.erase(layers_.begin() + i);
        active_ = std::clamp(i - 1, 0, layerCount() - 1);
        return true;
    }

    // 把所有图层的脏矩形并起来并清空。
    // 合成步骤用它决定"重算哪一块"，而不是每帧重算整张图。
    Rect takeDirtyUnion() {
        Rect u;
        for (Layer& L : layers_) u.add(L.takeDirty());
        return u;
    }
    bool hasAnyDirty() const {
        for (const Layer& L : layers_)
            if (L.hasDirty()) return true;
        return false;
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
