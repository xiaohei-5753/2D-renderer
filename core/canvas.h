// core/canvas.h — 压平用的合成表面
//
// 改造前这个类同时扮演两个角色："可绘制的表面"和"送去光照的那张图"。
// 有了图层之后两个角色分开了：
//   * **可绘制的表面**是 core/layer.h 里的 Layer（工具、History 只碰它）
//   * **送去光照的图**是所有图层压平的结果，就是这里的 Canvas
//
// 于是 Canvas 现在只做三件事：持有一份 albedo + material、
// 在合成（core/composite.h）与上传之间当缓冲、用脏矩形告诉 GPU 只传哪一块。
// 它仍然保留了完整的绘制原语 —— 工具之外的代码（测试、离线渲染工具）用得着，
// 而且与 Layer 的语义完全一致，读代码时不需要在两种约定之间切换。
//
// albedo 的 alpha 语义（这也是光照模型的枢纽）：
//   a == 0     空气，光直接穿过
//   0 < a < 1  半透明介质：让光穿过，同时按 emission 自发光（霓虹管、玻璃）
//   a == 255   实心不透明：挡住光，画面直接显示自身颜色（墙、实体）
//
// 不依赖 OpenGL：gfx 层只通过 image()/materialData()/takeDirty() 取数据，
// 所以这一层可以 headless 测试。
#pragma once

#include <cstdint>
#include <vector>

#include "image.h"
#include "material.h"
#include "types.h"

namespace pb {

class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h) { resize(w, h); }

    // 改变尺寸会丢弃内容（albedo 清成全透明、material 清成默认），并标记整块为脏
    void resize(int w, int h);
    void clear(RGBA8 c = RGBA8{0, 0, 0, 0}, Material m = Material{});

    int width() const { return img_.width(); }
    int height() const { return img_.height(); }
    bool empty() const { return img_.empty(); }
    size_t pixelCount() const { return img_.pixelCount(); }

    // --- albedo ---
    Image& image() { return img_; }
    const Image& image() const { return img_; }

    // --- material ---
    std::vector<Material>& materials() { return mat_; }
    const std::vector<Material>& materials() const { return mat_; }
    Material* materialData() { return mat_.data(); }
    const Material* materialData() const { return mat_.data(); }
    Material& materialAt(int x, int y) { return mat_[(size_t)y * (size_t)img_.width() + (size_t)x]; }
    const Material& materialAt(int x, int y) const {
        return mat_[(size_t)y * (size_t)img_.width() + (size_t)x];
    }

    // --- 脏矩形 ---
    void markDirty(const Rect& r) { dirty_.add(r); }
    void markPixelDirty(int x, int y) { dirty_.add(x, y); }
    void markAllDirty() { dirty_.add(0, 0, img_.width() - 1, img_.height() - 1); }
    const Rect& dirty() const { return dirty_; }
    bool hasDirty() const { return !dirty_.empty(); }
    // 取出脏矩形并清空（渲染层上传纹理时用这个）
    Rect takeDirty() {
        Rect r = dirty_;
        dirty_.reset();
        return r;
    }
    // 只看不清
    void clearDirty() { dirty_.reset(); }

    // --- 绘制原语 ---
    // 单像素写入；越界被忽略。会标记脏。
    bool setPixel(int x, int y, RGBA8 c, Material m = Material{});

    // 不做脏标记与越界检查的裸写入，供工具在已知范围内批量操作
    void setPixelRaw(int x, int y, RGBA8 c, Material m = Material{}) {
        const size_t i = (size_t)y * (size_t)img_.width() + (size_t)x;
        img_.data()[i] = c;
        mat_[i] = m;
    }

    void fill(RGBA8 c, Material m = Material{});
    void fillRect(int x0, int y0, int x1, int y1, RGBA8 c, Material m = Material{});
    void rectOutline(int x0, int y0, int x1, int y1, RGBA8 c, Material m = Material{});

    // 对称 Bresenham 直线（沿用原 line_cvs 的做法：把线段变换到第一象限再统一画，
    // 避免每个分支重复循环体）。会标记脏。
    void drawLine(int x0, int y0, int x1, int y1, RGBA8 c, Material m = Material{});

    // 实心/空心椭圆（中点椭圆算法）。会标记脏。
    void drawEllipse(int x0, int y0, int x1, int y1, bool filled, RGBA8 c,
                     Material m = Material{});

    // 洪水填充（四连通，4 邻域）。返回填充的像素数。
    // 以 (x,y) 处当前颜色为基准，只在颜色相同且 material 也相同的连通区内扩散。
    int floodFill(int x, int y, RGBA8 c, Material m = Material{});

private:
    Image img_;
    std::vector<Material> mat_;
    Rect dirty_;
};

}  // namespace pb
