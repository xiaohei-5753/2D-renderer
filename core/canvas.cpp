// core/canvas.cpp
#include "canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace pb {

// ---------------------------------------------------------------------------
// 尺寸与清空
// ---------------------------------------------------------------------------

void Canvas::resize(int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    img_.resize(w, h);
    mat_.assign((size_t)w * (size_t)h, Material{});
    markAllDirty();
}

void Canvas::clear(RGBA8 c, Material m) {
    img_.fill(c);
    for (auto& e : mat_) e = m;
    markAllDirty();
}

void Canvas::fill(RGBA8 c, Material m) {
    img_.fill(c);
    for (auto& e : mat_) e = m;
    markAllDirty();
}

// ---------------------------------------------------------------------------
// 基础图元
// ---------------------------------------------------------------------------

bool Canvas::setPixel(int x, int y, RGBA8 c, Material m) {
    if (x < 0 || y < 0 || x >= img_.width() || y >= img_.height()) return false;
    setPixelRaw(x, y, c, m);
    markPixelDirty(x, y);
    return true;
}

void Canvas::fillRect(int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(img_.width() - 1, x1);
    y1 = std::min(img_.height() - 1, y1);
    if (x1 < x0 || y1 < y0) return;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) setPixelRaw(x, y, c, m);
    markDirty(Rect{x0, y0, x1, y1});
}

void Canvas::rectOutline(int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(img_.width() - 1, x1);
    y1 = std::min(img_.height() - 1, y1);
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

void Canvas::drawLine(int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
    // 端点规范化。
    //
    // Bresenham 的误差项在"平局"（err 恰好等于 dx）时会固定偏向一侧，所以把起点和终点
    // 对调画出来的是同一条直线的**另一侧**像素——这是经典 Bresenham 的固有性质，不是 bug。
    // 但对画板而言，从左往右拖和从右往左拖必须得到同样的线，否则用户会看到"同一笔画两次
    // 结果不一样"。先按 (x, y) 字典序把端点排好，光栅化结果就与拖拽方向无关了。
    if (x1 < x0 || (x1 == x0 && y1 < y0)) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }

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
        px += x1;
        py += y1;
        if (px >= 0 && px < W && py >= 0 && py < H) {
            setPixelRaw(px, py, c, m);
            markPixelDirty(px, py);
        }
        err += dy;
        if (err >= dx) {
            y++;
            err -= dx;
        }
    }
}

// ---------------------------------------------------------------------------
// 椭圆
// ---------------------------------------------------------------------------

namespace {

inline void plotEllipse(Canvas& cv, int x, int y, RGBA8 c, Material m) {
    cv.setPixel(x, y, c, m);
}

// 空心椭圆：Zingl 的中点椭圆算法（"A Rasterizing Algorithm for Drawing Curves"，
// public domain）。它用四个象限对称前进，并且专门处理了 a=1 这类极扁椭圆的
// "过早停止"问题——朴素的中点算法在这些情况下会画不完整。
void ellipseOutline(Canvas& cv, int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
    long a = std::labs((long)x1 - (long)x0);
    long b = std::labs((long)y1 - (long)y0);
    long b1 = b & 1;
    double dx = 4 * (1.0 - (double)a) * (double)b * (double)b;
    double dy = 4 * (double)(b1 + 1) * (double)a * (double)a;
    double err = dx + dy + (double)b1 * (double)a * (double)a;

    if (x0 > x1) {
        x0 = x1;
        x1 = (int)(x0 + a);
    }
    if (y0 > y1) y0 = y1;
    y0 += (int)((b + 1) / 2);
    y1 = (int)(y0 - b1);
    a *= 8 * a;
    b1 = 8 * b * b;

    do {
        plotEllipse(cv, x1, y0, c, m);  // I   象限
        plotEllipse(cv, x0, y0, c, m);  // II
        plotEllipse(cv, x0, y1, c, m);  // III
        plotEllipse(cv, x1, y1, c, m);  // IV
        const double e2 = 2 * err;
        if (e2 <= dy) {
            y0++;
            y1--;
            err += dy += (double)a;
        }
        if (e2 >= dx || 2 * err > dy) {
            x0++;
            x1--;
            err += dx += (double)b1;
        }
    } while (x0 <= x1);

    // 极扁椭圆（a=1）会过早退出循环，这里补上两端
    while (y0 - y1 < b) {
        plotEllipse(cv, x0 - 1, y0, c, m);
        plotEllipse(cv, x1 + 1, y0++, c, m);
        plotEllipse(cv, x0 - 1, y1, c, m);
        plotEllipse(cv, x1 + 1, y1--, c, m);
    }
}

// 实心椭圆：逐行解析求 x 跨度。用"倍化坐标"避免半整数半径的舍入问题：
//   点在椭圆内 <=>  ((2x-cx2)/rx)^2 + ((2y-cy2)/ry)^2 <= 1
// 其中 cx2 = x0+x1, cy2 = y0+y1, rx = x1-x0, ry = y1-y0。
void ellipseFilled(Canvas& cv, int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    const double cx2 = (double)x0 + (double)x1;
    const double cy2 = (double)y0 + (double)y1;
    const double rx = (double)(x1 - x0);
    const double ry = (double)(y1 - y0);

    if (rx <= 0.0 && ry <= 0.0) {
        cv.setPixel(x0, y0, c, m);
        return;
    }

    const int W = cv.width(), H = cv.height();
    for (int y = std::max(0, y0); y <= std::min(H - 1, y1); y++) {
        double u = 0.0;
        if (ry > 0.0) u = (2.0 * (double)y - cy2) / ry;
        const double t = 1.0 - u * u;
        if (t < 0.0) continue;
        const double halfExtent = rx * std::sqrt(t);
        int xa = (int)std::ceil((cx2 - halfExtent) / 2.0);
        int xb = (int)std::floor((cx2 + halfExtent) / 2.0);
        xa = std::max(0, xa);
        xb = std::min(W - 1, xb);
        if (xb < xa) continue;
        for (int x = xa; x <= xb; x++) cv.setPixelRaw(x, y, c, m);
        cv.markDirty(Rect{xa, y, xb, y});
    }
}

}  // namespace

void Canvas::drawEllipse(int x0, int y0, int x1, int y1, bool filled, RGBA8 c, Material m) {
    if (filled) {
        ellipseFilled(*this, x0, y0, x1, y1, c, m);
        return;
    }
    ellipseOutline(*this, x0, y0, x1, y1, c, m);
}

// ---------------------------------------------------------------------------
// 洪水填充
// ---------------------------------------------------------------------------

int Canvas::floodFill(int x, int y, RGBA8 c, Material m) {
    const int W = img_.width(), H = img_.height();
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;

    const RGBA8 target = img_.at(x, y);
    const Material tmat = materialAt(x, y);
    // 目标色与源完全相同就没有必要填（也避免下面的终止条件失效）
    if (target == c && tmat == m) return 0;

    std::vector<CoordI> stack;
    stack.reserve(1024);
    stack.push_back(CoordI{x, y});

    int count = 0;
    while (!stack.empty()) {
        const CoordI p = stack.back();
        stack.pop_back();
        if (p.x < 0 || p.y < 0 || p.x >= W || p.y >= H) continue;
        if (img_.at(p.x, p.y) != target) continue;
        if (materialAt(p.x, p.y) != tmat) continue;

        setPixelRaw(p.x, p.y, c, m);
        markPixelDirty(p.x, p.y);
        count++;

        stack.push_back(CoordI{p.x + 1, p.y});
        stack.push_back(CoordI{p.x - 1, p.y});
        stack.push_back(CoordI{p.x, p.y + 1});
        stack.push_back(CoordI{p.x, p.y - 1});
    }
    return count;
}

}  // namespace pb
