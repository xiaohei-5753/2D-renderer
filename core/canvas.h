// core/canvas.h — 画板文档
//
// 持有像素数据与"自上次同步以来哪些像素变了"的脏矩形。
// 不依赖 OpenGL：gfx 层只通过 image()/takeDirty() 取数据，所以这一层可以 headless 测试。
#pragma once

#include <cstdint>

#include "image.h"
#include "types.h"

namespace pb {

class Canvas {
public:
    Canvas() = default;
    Canvas(int w, int h) { resize(w, h); }

    // 改变尺寸会丢弃内容，并把整块画布标记为脏
    void resize(int w, int h);
    void clear(RGBA8 c = RGBA8{0, 0, 0, 0});

    int width() const { return img_.width(); }
    int height() const { return img_.height(); }

    Image& image() { return img_; }
    const Image& image() const { return img_; }

    // --- 脏矩形 ---
    void markDirty(const Rect& r) { dirty_.add(r); }
    void markPixelDirty(int x, int y) { dirty_.add(x, y); }
    void markAllDirty() { dirty_.add(0, 0, img_.width() - 1, img_.height() - 1); }
    const Rect& dirty() const { return dirty_; }
    bool hasDirty() const { return !dirty_.empty(); }
    // 取出脏矩形并清空（渲染层上传纹理时用这个）
    Rect takeDirty() { Rect r = dirty_; dirty_.reset(); return r; }

    // --- 绘制原语 ---
    // 单像素写入；越界被忽略。会标记脏。
    bool setPixel(int x, int y, RGBA8 c);
    // 不做脏标记的裸写入，供批量操作（如 fill）自己合并脏矩形
    void setPixelRaw(int x, int y, RGBA8 c) { img_.at(x, y) = c; }

    void fillRect(int x0, int y0, int x1, int y1, RGBA8 c);
    void fill(RGBA8 c);

    // 对称 Bresenham 直线（沿用原 line_cvs 的做法：把线段变换到第一象限再统一画，
    // 避免每个分支重复循环体）。会标记脏。
    void drawLine(int x0, int y0, int x1, int y1, RGBA8 c);

private:
    Image img_;
    Rect dirty_;
};

}  // namespace pb
