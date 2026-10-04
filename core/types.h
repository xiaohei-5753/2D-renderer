// core/types.h — 项目通用的基础类型
//
// 这个头文件不依赖 OpenGL、GLFW 或任何第三方库，core 层与 gfx 层都包含它。
#pragma once

#include <algorithm>
#include <cstdint>

namespace pb {

// 一个像素：albedo 颜色 + 覆盖率/不透明度。
//
// alpha 同时承担两个语义，这一点对光传输通道很关键：
//   a == 0   空气，光直接穿过
//   0 < a < 1  半透明介质：既让光穿过，又按 rgb*a*emit 自发光（霓虹管、玻璃）
//   a == 1   实心不透明：光被挡住，画面直接显示自身 albedo（墙、实体）
struct RGBA8 {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};

inline bool operator==(const RGBA8& x, const RGBA8& y) {
    return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
}
inline bool operator!=(const RGBA8& x, const RGBA8& y) { return !(x == y); }

// 含端点的整数矩形。沿用原代码的"脏矩形"约定：x1 < x0 表示空。
struct Rect {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

    bool empty() const { return x1 < x0 || y1 < y0; }
    int width() const { return empty() ? 0 : x1 - x0 + 1; }
    int height() const { return empty() ? 0 : y1 - y0 + 1; }

    void reset() { x0 = 0; y0 = 0; x1 = -1; y1 = -1; }

    void add(int x, int y) {
        if (empty()) { x0 = x1 = x; y0 = y1 = y; return; }
        x0 = std::min(x0, x); y0 = std::min(y0, y);
        x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    void add(int ax0, int ay0, int ax1, int ay1) {
        if (ax1 < ax0 || ay1 < ay0) return;
        add(ax0, ay0);
        add(ax1, ay1);
    }
    void add(const Rect& r) {
        if (r.empty()) return;
        add(r.x0, r.y0, r.x1, r.y1);
    }
    void clipTo(int w, int h) {
        if (empty()) return;
        x0 = std::max(0, x0); y0 = std::max(0, y0);
        x1 = std::min(w - 1, x1); y1 = std::min(h - 1, y1);
    }
};

struct CoordI {
    int x = 0, y = 0;
};

struct Vec2 {
    double x = 0.0, y = 0.0;
};

}  // namespace pb
