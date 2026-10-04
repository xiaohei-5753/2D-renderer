// core/canvas.cpp
#include "canvas.h"

#include <algorithm>

namespace pb {

void Canvas::resize(int w, int h) {
    img_.resize(w, h);
    markAllDirty();
}

void Canvas::clear(RGBA8 c) {
    img_.fill(c);
    markAllDirty();
}

bool Canvas::setPixel(int x, int y, RGBA8 c) {
    if (!img_.setSafe(x, y, c)) return false;
    markPixelDirty(x, y);
    return true;
}

void Canvas::fill(RGBA8 c) {
    img_.fill(c);
    markAllDirty();
}

void Canvas::fillRect(int x0, int y0, int x1, int y1, RGBA8 c) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(img_.width() - 1, x1);
    y1 = std::min(img_.height() - 1, y1);
    if (x1 < x0 || y1 < y0) return;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) img_.at(x, y) = c;
    markDirty(Rect{x0, y0, x1, y1});
}

void Canvas::drawLine(int x0, int y0, int x1, int y1, RGBA8 c) {
    // 变换到第一象限：把"往哪个方向走"折叠成符号位，循环体只写一份。
    // 符号位含义：bit0 = y 取反，bit1 = x 取反，bit2 = 交换 x/y
    //
    // 注意方向约定（沿用原 line_cvs）：差向量取的是「起点 - 终点」，
    // 循环里再把终点加回去，于是画出的点从终点走向起点。
    // 若写成「终点 - 起点」，加回终点后坐标会整体平移出画布。
    int dx = x0 - x1;
    int dy = y0 - y1;
    int sym = 0;
    if (dy < 0) { dy = -dy; sym |= 1; }
    if (dx < 0) { dx = -dx; sym |= 2; }
    if (dy > dx) { std::swap(dy, dx); sym |= 4; }

    const int W = img_.width(), H = img_.height();
    int err = 0, y = 0;
    for (int x = 0; x <= dx; x++) {
        int px = x, py = y;
        if (sym & 4) std::swap(px, py);
        if (sym & 2) px = -px;
        if (sym & 1) py = -py;
        px += x1; py += y1;
        if (px >= 0 && px < W && py >= 0 && py < H) {
            img_.at(px, py) = c;
            markPixelDirty(px, py);
        }
        err += dy;
        if (err >= dx) { y++; err -= dx; }
    }
}

}  // namespace pb
