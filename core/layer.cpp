// core/layer.cpp — 图层的绘制原语
//
// 这些算法的说明见 core/canvas.cpp（两边是同一套语义，注释只在那里写一份，
// 这里保留必要的提醒）。两个类刻意不共用代码：它们的"角色"不同
// （Layer = 可绘制表面，Canvas = 送去光照的压平结果），
// 硬抽公共基类会把"谁可以画"这件事绑死，得不偿失。
#include "layer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace pb {

namespace {

// 空心椭圆：Zingl 的中点椭圆算法（public domain）。
// 它用四个象限对称前进，并专门处理了 a=1 这类极扁椭圆的"过早停止"问题。
void ellipseOutline(Layer& L, int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
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
        L.setPixel(x1, y0, c, m);
        L.setPixel(x0, y0, c, m);
        L.setPixel(x0, y1, c, m);
        L.setPixel(x1, y1, c, m);
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

    while (y0 - y1 < b) {
        L.setPixel(x0 - 1, y0, c, m);
        L.setPixel(x1 + 1, y0++, c, m);
        L.setPixel(x0 - 1, y1, c, m);
        L.setPixel(x1 + 1, y1--, c, m);
    }
}

// 实心椭圆：逐行解析求 x 跨度。倍化坐标避免半整数半径的舍入问题。
void ellipseFilled(Layer& L, int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    const double cx2 = (double)x0 + (double)x1;
    const double cy2 = (double)y0 + (double)y1;
    const double rx = (double)(x1 - x0);
    const double ry = (double)(y1 - y0);

    if (rx <= 0.0 && ry <= 0.0) {
        L.setPixel(x0, y0, c, m);
        return;
    }

    const int W = L.width(), H = L.height();
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
        for (int x = xa; x <= xb; x++) L.setPixelRaw(x, y, c, m);
        L.markDirty(Rect{xa, y, xb, y});
    }
}

}  // namespace

void Layer::drawLine(int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
    // 端点规范化（与 Canvas::drawLine 同一理由）：让光栅化结果与拖拽方向无关
    if (x1 < x0 || (x1 == x0 && y1 < y0)) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }

    int dx = x0 - x1;
    int dy = y0 - y1;
    int sym = 0;
    if (dy < 0) { dy = -dy; sym |= 1; }
    if (dx < 0) { dx = -dx; sym |= 2; }
    if (dy > dx) { std::swap(dy, dx); sym |= 4; }

    const int W = width(), H = height();
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
    revision++;
}

void Layer::drawEllipse(int x0, int y0, int x1, int y1, bool filled, RGBA8 c, Material m) {
    if (filled) ellipseFilled(*this, x0, y0, x1, y1, c, m);
    else ellipseOutline(*this, x0, y0, x1, y1, c, m);
    revision++;
}

int Layer::floodFill(int x, int y, RGBA8 c, Material m) {
    const int W = width(), H = height();
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;

    const RGBA8 target = albedo.at(x, y);
    const Material tmat = materialAt(x, y);
    if (target == c && tmat == m) return 0;

    std::vector<CoordI> stack;
    stack.reserve(1024);
    stack.push_back(CoordI{x, y});

    int count = 0;
    while (!stack.empty()) {
        const CoordI p = stack.back();
        stack.pop_back();
        if (p.x < 0 || p.y < 0 || p.x >= W || p.y >= H) continue;
        if (albedo.at(p.x, p.y) != target) continue;
        if (materialAt(p.x, p.y) != tmat) continue;

        setPixelRaw(p.x, p.y, c, m);
        markPixelDirty(p.x, p.y);
        count++;

        stack.push_back(CoordI{p.x + 1, p.y});
        stack.push_back(CoordI{p.x - 1, p.y});
        stack.push_back(CoordI{p.x, p.y + 1});
        stack.push_back(CoordI{p.x, p.y - 1});
    }
    revision++;
    return count;
}

}  // namespace pb
