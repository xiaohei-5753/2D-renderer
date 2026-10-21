// scenes/bench_scene.cpp — 实时性能基准 / 渲染器视觉测试集
//
// ## 每个场景都是"能测 + 好看"的
//
// 场景不是随便画的图，每个都针对一类视觉特征，并且**同时**是有构图的画面：
//
//   neon-city     1px 霓虹管 = 锐度探针；大面积窗户泛光；前景雾使垂直自发光
//                 传输可见。构图：夜空 + 城市剪影 + 霓虹招牌 + 雾。
//   lantern-fog   三盏灯笼在多层雾下方 —— 光必须**垂直穿过**雾才到相机。
//                 这是本渲染器最特色的能力，必须有场景专门压它。
//   stained-glass 白背光 + 8 块半透明彩色玻璃 + 黑色铅条。
//                 测半透明染色、硬阴影、相邻色块的光互染。
//   chromatic     黑底 + 8 个彩色光球，albedo 全黑、自发光饱和色。
//                 直接展示"自发光与 albedo 解耦"。若实现走错（自发光乘 albedo），
//                 这些球会全黑 —— 一眼可判。
//   grid-lab      布局严格固定的纯诊断图（点/线/网格/方块），静止，
//                 专供 tools/dir_probe 出误差放大图。没有艺术性，也不需要。
//   legacy        原来的 5 层动画基准，保证历史性能数字仍可复现。
//
// ## 三条硬纪律（改动前必读）
//
// 1. **确定性**：所有随时间变化的量都是 f(frame)，只用 sinf / 取模 / 坐标哈希。
//    没有 rand()、没有 time()、没有未初始化状态 —— 同一帧号在任何设备上一致。
// 2. **每层每帧都要被碰一次**（哪怕只改 1 个像素）。这样光照的脏层优化失效，
//    量到的才是"每帧把 N 层全部重算"的真实上限，数字才有可比性。
// 3. **CPU 侧 O(像素)**：雾按 4px 间隔画、剪影按 8px 分组，都是为了这个。
//
// ## ⚠️ 一个必须自己守的坑
//
// `Layer::setPixelRaw` **没有边界检查** —— 它直接算 `data()[y*w+x]` 就写。
// 负坐标会写到上一行（甚至越界踩坏内存）。所有写像素一律走下面的 pset()。
#include "scenes/bench_scene.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/layer.h"
#include "core/material.h"
#include "core/types.h"

namespace pb {
namespace bench {

// ---------------------------------------------------------------------------
// 通用小工具（全部确定性）
// ---------------------------------------------------------------------------
namespace {

uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// 坐标哈希：绝不调用 rand()。同一 (x,y,salt) 在任何设备上结果一致。
inline uint32_t hashU(uint32_t x, uint32_t y, uint32_t s) {
    uint32_t h = x * 374761393u + y * 668265263u + s * 2246822519u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return h;
}
inline float hash01(int x, int y, int salt) {
    return (float)hashU((uint32_t)x, (uint32_t)y, (uint32_t)salt) / 4294967295.0f;
}

// 帧相位脉冲：period 帧一个周期，返回 0..1
inline float pulse(int frame, double period) {
    return 0.5f + 0.5f * (float)std::sin(6.283185307179586 * (double)frame / period);
}

// ⚠️ 唯一允许的写像素方式。setPixelRaw 没有边界检查，负坐标会踩坏内存。
inline void pset(Layer& L, int x, int y, RGBA8 c, Material m = Material{}) {
    if (x < 0 || y < 0 || x >= L.width() || y >= L.height()) return;
    L.setPixelRaw(x, y, c, m);
}

// 自发光便利构造：albedo 与自发光分开给（这是本渲染器的核心能力）
inline Material emitMat(uint8_t r, uint8_t g, uint8_t b) {
    Material m;
    m.setEmissionFrom(r, g, b);
    return m;
}

// 1px 粗的布雷森汉姆线（像素画的"锐利探针"就靠它）
void line1px(Layer& L, int x0, int y0, int x1, int y1, RGBA8 c, Material m) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        pset(L, x0, y0, c, m);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// 圆盘填充（柔光团 / 灯笼 / 光球用）
void disc(Layer& L, int cx, int cy, int rad, RGBA8 c, Material m) {
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++)
            if (dx * dx + dy * dy <= rad * rad) pset(L, cx + dx, cy + dy, c, m);
}

// 把一层清成空气（不碰 History）
void clearLayer(Layer& L) {
    L.image().fill(RGBA8{0, 0, 0, 0});
    for (auto& mm : L.material) mm = Material{};
    L.markAllDirty();
}

// 按需要重设图层数并清空；返回实际层数
int prepare(Document& doc, int want) {
    if (want <= 0) want = 5;
    while (doc.layerCount() < want) doc.addLayer("");
    while (doc.layerCount() > want) doc.removeLayer(doc.layerCount() - 1);
    for (int i = 0; i < doc.layerCount(); i++) {
        doc.layer(i).visible = true;
        doc.layer(i).opacity = 255;
        clearLayer(doc.layer(i));
    }
    doc.setActiveIndex(0);
    return doc.layerCount();
}

// 色相 -> RGB（legacy 场景用）
void hueToRgb(double h, uint8_t& r, uint8_t& g, uint8_t& b) {
    h = h - std::floor(h);
    const double x = 1.0 - std::fabs(std::fmod(h * 6.0, 2.0) - 1.0);
    double rr = 0, gg = 0, bb = 0;
    if (h < 1.0 / 6)      { rr = 1; gg = x; }
    else if (h < 2.0 / 6) { rr = x; gg = 1; }
    else if (h < 3.0 / 6) { gg = 1; bb = x; }
    else if (h < 4.0 / 6) { gg = x; bb = 1; }
    else if (h < 5.0 / 6) { rr = x; bb = 1; }
    else                  { rr = 1; bb = x; }
    r = clamp255((int)std::lround(rr * 255.0));
    g = clamp255((int)std::lround(gg * 255.0));
    b = clamp255((int)std::lround(bb * 255.0));
}

// ===========================================================================
// neon-rain —— 雨夜霓虹街景（长焦 / 平面构图 / 5 层 / 实时动画）
// ===========================================================================
//
// ## 视觉参考与为什么这么画
//
// 参考方向（长焦平面构图 + 雨夜霓虹）：
//   * Roger Deakins 在《银翼杀手 2049》里的雨夜：整幅画面是**几层平行的平面**
//     （远景楼群 / 中景招牌 / 近景雨幕），长焦把前后压扁，竖直边缘一律保持竖直，
//     没有透视汇聚点。
//   * 城市夜雨的像素画传统：光源少而饱和（青 / 品红 / 琥珀），大面积近黑，
//     湿路面用**竖直拉长的反光条**表达，雨用 1px 竖线，雾用低对比度的水平带。
//     参考：<https://stockcake.com/i/neon-city-rain_4486204_1959779>、
//     <https://www.summerengine.com/es/asset-store/rain-soaked-neon-street-level-7af10a85>
//
// ## 构图规则（这几条决定"像不像"，比调色更重要）
//
//  1. **三层平面**：远（L0 天空 + L1 远景楼，低对比）→ 中（L2 招牌立面，最高
//     对比、最饱和）→ 近（L3 街面 + L4 雨幕，剪影 + 高光）。层与层之间靠**遮挡**
//     而不是靠大小拉开距离 —— 长焦没有近大远小。
//  2. **一条竖缝**：中景两栋楼之间留 8% 宽的空隙露出远景楼，depth 立刻出现。
//  3. **地面占 28%**：湿街是唯一大面积"漫反射 + 镜面拉丝"的地方，压住画面底部。
//  4. **只有两种斜线**：电线和雨。其余全是水平/竖直 —— 这是长焦的"平"。
//  5. **最亮的是招牌，不是天空**：天空压到近黑（比中景招牌暗一个数量级），
//     否则霓虹读不出来。
//
// ## 动画（全部是 f(frame)，确定性）
//
//  雨（4 层不同速度/长度）、招牌接触不良的闪烁、地面积水的波纹抖动、
//  远处塔顶的航空障碍灯、横幅招牌的滚动高光。
void buildNeonRain(Document& doc, int frame) {
    const int W = doc.width(), H = doc.height();
    prepare(doc, 5);
    const int horizon = H * 70 / 100;
    const int alleyX0 = W * 40 / 100, alleyX1 = W * 56 / 100;   // 竖缝：远景从这里透出来

    // 调色板（饱和度集中在招牌上；环境色一律压暗，否则霓虹读不出来）
    const RGBA8 mag{240, 72, 158, 255};
    const RGBA8 cyn{72, 232, 240, 255};
    const RGBA8 amb{250, 178, 84, 255};
    const RGBA8 red{232, 64, 72, 255};

    // ---- L0：天空（顶部深青 -> 地平线青灰，充当远景楼的背光）----
    {
        Layer& L = doc.layer(0);
        for (int y = 0; y < H; y++) {
            const float t = std::min(1.0f, (float)y / (float)std::max(1, horizon));
            float r, g, b;
            if (t < 0.55f) {
                const float k = t / 0.55f;                 // 深青 -> 中青
                r = 24 + 34 * k; g = 36 + 46 * k; b = 50 + 44 * k;
            } else {
                const float k = (t - 0.55f) / 0.45f;       // 中青 -> 地平线亮青灰
                r = 58 + 30 * k; g = 82 + 44 * k; b = 94 + 40 * k;
            }
            const RGBA8 c{clamp255((int)r), clamp255((int)g), clamp255((int)b), 255};
            for (int x = 0; x < W; x++) pset(L, x, y, c);
        }
        // 城市辉光：地平线上方一条更亮的带（雨雾把城市光散射到天上）
        const int glowH = 22;
        for (int i = 0; i < glowH; i++) {
            const int y = horizon - glowH + i;
            if (y < 0) continue;
            const float k = 0.22f + 0.78f * (float)i / (float)glowH;
            for (int x = 0; x < W; x++) {
                const RGBA8 c{(uint8_t)(96 * k), (uint8_t)(150 * k), (uint8_t)(162 * k), 255};
                pset(L, x, y, c);
            }
        }
        L.markAllDirty();
    }

    // ---- L1：远景楼群（比天空暗 -> 剪影；稀疏窗格 + 塔顶航空灯）----
    {
        Layer& L = doc.layer(1);
        for (int gx = 0; gx < W; gx += 9) {
            const float hh = hash01(gx / 9, 1, 21);
            const int bh = 46 + (int)(hh * 78.0f);          // 长焦：远景也顶到画面顶部
            const int top = std::max(-10, horizon - bh);
            const int wcol = 9 - (int)(hash01(gx / 9, 3, 25) * 3.0f);
            const bool nearer = (hash01(gx / 9, 2, 23) > 0.45f);
            const RGBA8 body = nearer ? RGBA8{52, 60, 72, 255} : RGBA8{40, 47, 57, 255};
            for (int x = gx; x < gx + wcol && x < W; x++)
                for (int y = top; y < horizon; y++) pset(L, x, y, body);
            // 窗：极暗的暖色，只负责"有城市"的信息量
            for (int wy = top + 6; wy < horizon - 4; wy += 6)
                for (int wx = gx + 2; wx < gx + wcol - 1; wx += 4) {
                    if (hash01(wx, wy, 31) > 0.78f) {
                        const int warm = 104 + (int)(hash01(wx, wy, 33) * 46.0f);
                        pset(L, wx, wy, RGBA8{(uint8_t)(warm + 30), (uint8_t)warm,
                                              (uint8_t)(warm * 3 / 5), 255});
                    }
                }
        }
        if (((frame / 26) % 2) == 0) {
            const int bx = W * 15 / 100;
            for (int y = horizon - 88; y < horizon - 86; y++)
                for (int x = bx; x < bx + 2; x++)
                    pset(L, x, y, RGBA8{120, 30, 26, 255}, emitMat(180, 36, 28));
        }
        L.markAllDirty();
    }

    // ---- L2：中景立面 + 霓虹招牌（最高对比层）----
    {
        Layer& L = doc.layer(2);
        // 墙是浅色混凝土：霓虹打上去才有颜色，没灯的地方自然暗 —— 这是"建模打光"的正常结果
        const RGBA8 face{112, 120, 132, 255}, edge{146, 156, 170, 255};
        for (int y = H * 6 / 100; y < horizon; y++)
            for (int x = 0; x < alleyX0; x++)
                pset(L, x, y, (x > alleyX0 - 4) ? edge : face);
        for (int y = H * 2 / 100; y < horizon; y++)
            for (int x = alleyX1; x < W; x++)
                pset(L, x, y, (x < alleyX1 + 4) ? edge : face);

        // 窗格：暖白 / 冷青，约 4 成亮；接触不良的偶尔灭
        // 窗户：**不规则**才是城市。整层暗、相邻两扇并成一扇、少数只亮一半。
        auto windows = [&](int x0, int x1, int y0) {
            for (int fy = y0; fy < horizon - 10; fy += 7) {
                if (hash01(fy, 1, 47) < 0.34f) continue;   // 整层不亮
                bool prevLit = false;
                for (int fx = x0; fx < x1 - 6; fx += 6) {
                    const float r = hash01(fx, fy, 41);
                    if (prevLit && r < 0.45f) { prevLit = false; continue; }  // 偶尔并窗
                    if (r < 0.3f) { prevLit = false; continue; }
                    if (hash01(fx, fy, 43) > 0.9f && ((frame / 6) % 4) == 0) continue;
                    const bool cool = hash01(fx, fy, 45) > 0.86f;
                    const int ww = (r > 0.9f) ? 4 : 3;
                    const int hh = 4;
                    // 亮度只在 40~105 之间变：窗是"被室内灯照亮的玻璃"，不是发光格
                    const uint8_t dim = (uint8_t)(52 + hash01(fx, fy, 51) * 46.0f);
                    const RGBA8 c = cool ? RGBA8{(uint8_t)(dim - 6), dim, (uint8_t)(dim + 18), 255}
                                         : RGBA8{(uint8_t)(dim + 26), (uint8_t)(dim - 4),
                                                 (uint8_t)(dim / 2 + 6), 255};
                    for (int dy = 0; dy < hh; dy++)
                        for (int dx = 0; dx < ww; dx++)
                            pset(L, fx + dx, fy + dy, c);
                    prevLit = true;
                }
            }
        };
        windows(8, alleyX0 - 6, H * 24 / 100);
        windows(alleyX1 + 8, W - 8, H * 22 / 100);

        // 闪烁：接触不良的霓虹灯（周期 + 偶发熄灭）
        auto flick = [&](int id, double period) {
            const float base = 0.78f + 0.22f * pulse(frame, period);
            const float drop = (hash01(id, frame / 3, 61) > 0.955f) ? 0.45f : 1.0f;
            return base * drop;
        };

        // 竖招牌：**填充暗色灯箱 + 亮边框 + 内部笔画**，这样在 1px 尺度下才读得出"招牌"
        auto vSign = [&](int x0, int y0, int w, int h, RGBA8 neon, float k, int seed) {
            const RGBA8 dim{(uint8_t)(neon.r * 0.52f), (uint8_t)(neon.g * 0.52f),
                            (uint8_t)(neon.b * 0.52f), 255};
            for (int y = y0; y < y0 + h; y++)
                for (int x = x0; x < x0 + w; x++) {
                    const bool border = (x == x0 || x == x0 + w - 1 || y == y0 ||
                                         y == y0 + h - 1);
                    if (border) {
                        const RGBA8 c{(uint8_t)(neon.r * k), (uint8_t)(neon.g * k),
                                      (uint8_t)(neon.b * k), 255};
                        pset(L, x, y, c, emitMat(c.r, c.g, c.b));
                    } else {
                        pset(L, x, y, dim, emitMat((uint8_t)(neon.r * 0.26f * k),
                                                   (uint8_t)(neon.g * 0.26f * k),
                                                   (uint8_t)(neon.b * 0.26f * k)));
                    }
                }
            // 招牌里的"字"：固定的 3x5 笔画表（像偏旁部首），比哈希噪声干净得多
            static const uint8_t kGlyph[6][5] = {
                {0b111, 0b001, 0b111, 0b100, 0b111},   // 目
                {0b010, 0b111, 0b010, 0b010, 0b111},   // 十
                {0b111, 0b100, 0b111, 0b001, 0b111},   // 匚
                {0b101, 0b101, 0b111, 0b001, 0b001},   // 冂
                {0b111, 0b010, 0b111, 0b010, 0b010},   // 王
                {0b100, 0b111, 0b100, 0b101, 0b011},   // ヒ
            };
            const int gw = w - 6;
            int gi = seed % 6;
            for (int gy = y0 + 4; gy + 5 < y0 + h - 2; gy += 7) {
                const uint8_t* gph = kGlyph[gi % 6];
                ++gi;
                for (int row = 0; row < 5; row++)
                    for (int col = 0; col < 3 && col < gw; col++) {
                        if ((gph[row] >> (2 - col)) & 1) {
                            const RGBA8 c{(uint8_t)(neon.r * k), (uint8_t)(neon.g * k),
                                          (uint8_t)(neon.b * k), 255};
                            pset(L, x0 + 3 + col, gy + row, c, emitMat(c.r, c.g, c.b));
                        }
                    }
            }
        };
        vSign(W * 3 / 100, H * 18 / 100, W * 5 / 100 + 8, H * 30 / 100, mag, flick(1, 41.0), 11);
        vSign(alleyX1 + W * 2 / 100, H * 12 / 100, W * 4 / 100 + 6, H * 24 / 100, cyn,
              flick(2, 57.0), 23);

        // 横招牌：横跨竖缝（长焦压扁的经典构图），琥珀色，带滚动高光
        {
            const int y0 = H * 30 / 100, hgt = 7;
            const int x0 = W * 62 / 100, x1 = W * 88 / 100;
            const float k = flick(3, 73.0);
            const RGBA8 dim{(uint8_t)(amb.r * 0.5f), (uint8_t)(amb.g * 0.5f),
                            (uint8_t)(amb.b * 0.5f), 255};
            for (int y = y0; y < y0 + hgt; y++)
                for (int x = x0; x <= x1; x++) {
                    const bool border = (y == y0 || y == y0 + hgt - 1 || x == x0 || x == x1);
                    if (border) {
                        const RGBA8 c{(uint8_t)(amb.r * k), (uint8_t)(amb.g * k),
                                      (uint8_t)(amb.b * k), 255};
                        pset(L, x, y, c, emitMat(c.r, c.g, c.b));
                    } else {
                        pset(L, x, y, dim, emitMat((uint8_t)(amb.r * 0.09f * k),
                                                   (uint8_t)(amb.g * 0.09f * k),
                                                   (uint8_t)(amb.b * 0.09f * k)));
                    }
                }
            const int sweep = (int)((frame * 2) % (x1 - x0 + 40)) - 20;
            for (int d = 0; d < 2; d++) {
                const int sx = x0 + sweep + d;
                if (sx > x0 && sx < x1)
                    for (int y = y0 + 1; y < y0 + hgt - 1; y++)
                        pset(L, sx, y, RGBA8{255, 236, 200, 255}, emitMat(255, 236, 200));
            }
        }
        // 红色警示灯（小面积高饱和，压住右侧）
        {
            const int x0 = alleyX1 + W * 7 / 100, y0 = H * 36 / 100;
            const float k = flick(4, 29.0);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    const RGBA8 c{(uint8_t)(red.r * k), (uint8_t)(red.g * k),
                                  (uint8_t)(red.b * k), 255};
                    pset(L, x0 + x, y0 + y, c, emitMat(c.r, c.g, c.b));
                }
        }
        // 店面灯带：贴着地平线上方的一条断续横光 —— 整幅画的主光源。
        // 它照亮下方墙面与湿路面，是"建模打光"里那盏真正在地面上的灯。
        for (int seg = 0; seg < 7; seg++) {
            const int sx = W * (4 + seg * 13) / 100;
            const int sw = W * (8 + (int)(hash01(seg, 3, 66) * 5.0f)) / 100;
            if (sx + sw > alleyX0 && sx < alleyX1) continue;   // 竖缝里不放
            const bool cool = (seg % 3) == 1;
            const RGBA8 base = cool ? RGBA8{150, 220, 226, 255} : RGBA8{244, 208, 158, 255};
            const float k = 0.8f + 0.2f * pulse(frame, 61.0 + seg * 7);
            for (int x = sx; x < sx + sw && x < W; x++) {
                for (int y = horizon - 5; y < horizon; y++) {
                    const float fall = 1.0f - (float)(horizon - 1 - y) / 6.0f;
                    const RGBA8 c{(uint8_t)(base.r * k * fall), (uint8_t)(base.g * k * fall),
                                  (uint8_t)(base.b * k * fall), 255};
                    pset(L, x, y, c, emitMat(c.r, c.g, c.b));
                }
            }
        }
        // 电线：画面里唯一允许的斜线（长焦的"平"靠它打破）
        for (int i = 0; i < 3; i++) {
            const int y0 = H * (12 + i * 5) / 100;
            const int y1 = H * (24 + i * 5) / 100;
            for (int x = 0; x < W; x++) {
                const float tt = (float)x / (float)W;
                const int y = (int)(y0 + (y1 - y0) * tt + 7.0f * tt * (1.0f - tt));
                pset(L, x, y, RGBA8{4, 5, 8, 255});
            }
        }
        L.markAllDirty();
    }

    // ---- L2b：竖缝里的远景（让缝读成"更远"，而不是一个黑洞）----
    {
        Layer& L = doc.layer(2);
        for (int y = H * 18 / 100; y < horizon; y++) {
            // 越往下越亮的远景雾（远处街口的光）
            const float k = (float)(y - H * 18 / 100) / (float)std::max(1, horizon - H * 18 / 100);
            const RGBA8 c{(uint8_t)(34 + 26 * k), (uint8_t)(50 + 34 * k), (uint8_t)(58 + 30 * k),
                          255};
            for (int x = alleyX0; x < alleyX1; x++) pset(L, x, y, c);
        }
        // 缝隙深处的两条细霓虹（远景招牌，只有几像素宽）
        for (int i = 0; i < 2; i++) {
            const int x = alleyX0 + 6 + i * (alleyX1 - alleyX0 - 12);
            const RGBA8 c = (i == 0) ? RGBA8{236, 96, 170, 255} : RGBA8{96, 220, 232, 255};
            for (int y = H * 34 / 100; y < H * 52 / 100; y++)
                pset(L, x, y, c, emitMat((uint8_t)(c.r * .8f), (uint8_t)(c.g * .8f),
                                         (uint8_t)(c.b * .8f)));
        }
        // 缝底部的店招（暖光从街上照进缝里）
        {
            const float k = 0.85f + 0.15f * pulse(frame, 47.0);
            for (int x = alleyX0 + 2; x < alleyX1 - 2; x++)
                for (int y = horizon - 4; y < horizon; y++) {
                    const RGBA8 c{(uint8_t)(250 * k), (uint8_t)(196 * k), (uint8_t)(140 * k),
                                  255};
                    pset(L, x, y, c, emitMat(c.r, c.g, c.b));
                }
        }
        L.markAllDirty();
    }

    // ---- L3：湿街（按列调制的竖直拉丝反光）+ 前景剪影 ----
    {
        Layer& L = doc.layer(3);
        for (int y = horizon; y < H; y++) {
            const float t = (float)(y - horizon) / (float)std::max(1, H - horizon);
            const int v = (int)(62 - 26 * t);
            for (int x = 0; x < W; x++)
                pset(L, x, y, RGBA8{(uint8_t)(v + 1), (uint8_t)(v + 3), (uint8_t)(v + 7), 255});
        }
        // 拉丝反光：**逐列**调制（不是整块），越远越亮、越往下越散、带波纹抖动
        auto smear = [&](int x0, int x1, RGBA8 c, int len, int seed) {
            for (int x = x0; x < x1; x++) {
                // 列缺口：真实水面反光是碎的，不是一整块色板
                if (hash01(x, 1, seed + 17) < 0.18f) continue;
                const float colK = 0.30f + 0.70f * hash01(x, 0, seed);
                for (int y = horizon; y < std::min(H, horizon + len); y++) {
                    const float t = (float)(y - horizon) / (float)std::max(1, len);
                    const float fall = (1.0f - t) * (1.0f - t);
                    // 波纹：随帧沿 y 抖动，制造"水面在动"
                    const int wob = (int)(1.6f * std::sin((double)y * 0.5 + frame * 0.35));
                    const int xx = x + wob;
                    const float k = fall * colK;
                    const RGBA8 cc{(uint8_t)(c.r * k), (uint8_t)(c.g * k), (uint8_t)(c.b * k),
                                   255};
                    pset(L, xx, y, cc, emitMat((uint8_t)(cc.r * 0.55f),
                                               (uint8_t)(cc.g * 0.55f),
                                               (uint8_t)(cc.b * 0.55f)));
                }
            }
        };
        smear(W * 3 / 100, W * 3 / 100 + W * 5 / 100 + 8, mag, (H - horizon) * 3 / 4, 3);
        smear(alleyX1 + W * 2 / 100, alleyX1 + W * 6 / 100 + 6, cyn, (H - horizon) * 2 / 3, 7);
        smear(W * 50 / 100, W * 92 / 100, amb, (H - horizon) * 2 / 5, 11);

        // 积水高光：横向短亮条（雨夜湿地的镜面）
        for (int i = 0; i < 7; i++) {
            const int cx = (int)(hash01(i, 0, 71) * W);
            const int cy = horizon + 6 + (int)(hash01(i, 1, 73) * (H - horizon - 10));
            const int rw = 4 + (int)(hash01(i, 2, 75) * 14.0f);
            for (int x = cx - rw; x <= cx + rw; x++) {
                const int yy = cy + (int)(1.8f * std::sin((double)(x + frame) * 0.4));
                const float k = 1.0f - std::fabs((float)(x - cx) / (float)(rw + 1));
                const RGBA8 c{(uint8_t)(120 * k), (uint8_t)(150 * k), (uint8_t)(164 * k), 255};
                pset(L, x, yy, c, emitMat(c.r, c.g, c.b));
            }
        }
        // 雨点水花（每 4 帧换一批）
        for (int i = 0; i < 30; i++) {
            const int ph = frame / 4;
            const int cx = (int)(hash01(i + ph * 37, 0, 81) * W);
            const int cy = horizon + 3 + (int)(hash01(i + ph * 37, 1, 83) * (H - horizon - 5));
            const int r = 1 + (int)(hash01(i, 2, 85) * 2.0f);
            for (int k = -r; k <= r; k++)
                pset(L, cx + k, cy, RGBA8{120, 148, 160, 255}, emitMat(40, 60, 70));
            pset(L, cx, cy - 1, RGBA8{150, 178, 190, 255}, emitMat(60, 84, 94));
        }

        // 前景剪影：栏杆（左）+ 打伞的人（中右）+ 路灯 + 垃圾桶
        const RGBA8 sil{20, 22, 28, 255};
        // 栏杆：两根立柱 + 两道横栏
        for (int x = 0; x < W * 30 / 100; x++) {
            pset(L, x, H - 30, sil);
            pset(L, x, H - 29, sil);
            pset(L, x, H - 14, sil);
        }
        for (int i = 0; i < 4; i++) {
            const int px = 2 + i * (W * 28 / 100) / 4;
            for (int y = H - 32; y < H; y++)
                for (int x = px; x < px + 3; x++) pset(L, x, y, sil);
        }
        // 打伞的人：伞弧 + 身体 + 腿，站在橙色反光前面（剪影最强的地方）
        {
            const int cx = W * 62 / 100, base = H - 1;
            const int bob = (int)(std::sin((double)frame * 0.10) * 1.0);
            const int uy = base - 38 + bob;
            for (int dx = -11; dx <= 11; dx++) {
                const int dy = (int)(10.0f * std::sqrt(std::max(0.0f, 1.0f -
                                                                  (float)(dx * dx) / 121.0f)));
                for (int x = cx + dx; x <= cx + dx + 1; x++) {
                    pset(L, x, uy - dy, sil);
                    pset(L, x, uy - dy + 1, sil);
                    pset(L, x, uy - dy + 2, sil);
                }
            }
            for (int y = uy; y < base - 6; y++)
                for (int x = cx - 3; x <= cx + 3; x++) pset(L, x, y, sil);
            for (int y = base - 6; y <= base; y++) {
                pset(L, cx - 3, y, sil);
                pset(L, cx - 2, y, sil);
                pset(L, cx + 2, y, sil);
                pset(L, cx + 3, y, sil);
            }
            for (int y = uy - 8; y < uy; y++) pset(L, cx, y, sil);   // 伞柄
        }
        // 路灯：唯一高位暖光
        {
            const int px = W * 82 / 100;
            for (int y = H * 34 / 100; y < H; y++)
                for (int x = px; x < px + 2; x++) pset(L, x, y, sil);
            for (int x = px - 12; x < px + 2; x++) pset(L, x, H * 34 / 100, sil);
            for (int y = H * 34 / 100 + 1; y < H * 34 / 100 + 3; y++)
                for (int x = px - 14; x < px - 7; x++)
                    pset(L, x, y, RGBA8{255, 210, 150, 255}, emitMat(255, 210, 150));
        }
        // 垃圾桶（右侧近景，给"街"一个尺度参照）
        for (int y = H - 16; y < H; y++)
            for (int x = W * 72 / 100; x < W * 72 / 100 + 7; x++) pset(L, x, y, sil);
        L.markAllDirty();
    }

    // ---- L4：近景雨幕 + 焦外光斑 ----
    {
        Layer& L = doc.layer(4);
        // 细雨：四档速度/长度，透明 40~110
        for (int i = 0; i < 220; i++) {
            const int speed = 6 + (int)(hash01(i, 1, 93) * 9.0f);
            const int x = (int)(hash01(i, 0, 91) * W);
            const int len = 5 + (int)(hash01(i, 2, 95) * 10.0f);
            const int y = ((int)(hash01(i, 3, 97) * H) + frame * speed) % H;
            const int a = 52 + (int)(hash01(i, 4, 99) * 82.0f);
            const RGBA8 c{150, 192, 210, (uint8_t)a};
            for (int k = 0; k < len; k++) pset(L, x, y + k, c);
        }
        // 粗雨：3 条长的，带一点倾斜（速度感）
        for (int i = 0; i < 3; i++) {
            const int speed = 18 + i * 6;
            const int x = ((int)(hash01(i, 7, 103) * W) + frame * 2) % W;
            const int y = (frame * speed + i * 80) % (H + 80) - 40;
            for (int k = 0; k < 38; k++) {
                const int xx = x - (k * 2) / 9;
                pset(L, xx, y + k, RGBA8{196, 226, 240, 88});
            }
        }
        // 焦外光斑：霓虹在长焦下的大光团（低透明、无锐边）
        struct Bokeh { int x, y, r; uint8_t rr, gg, bb, aa; };
        const Bokeh bok[5] = {{W * 10 / 100, H * 26 / 100, 11, 255, 120, 190, 30},
                              {W * 90 / 100, H * 20 / 100, 8, 120, 240, 250, 24},
                              {W * 26 / 100, H * 10 / 100, 7, 255, 180, 90, 20},
                              {W * 58 / 100, H * 62 / 100, 9, 255, 96, 160, 22},
                              {W * 74 / 100, H * 40 / 100, 6, 250, 220, 160, 18}};
        for (const Bokeh& bk : bok)
            for (int dy = -bk.r; dy <= bk.r; dy++)
                for (int dx = -bk.r; dx <= bk.r; dx++) {
                    const float d2 = (float)(dx * dx + dy * dy) / (float)(bk.r * bk.r);
                    if (d2 > 1.0f) continue;
                    const uint8_t a = (uint8_t)(bk.aa * (1.0f - d2 * 0.6f));
                    pset(L, bk.x + dx, bk.y + dy, RGBA8{bk.rr, bk.gg, bk.bb, a});
                }
        L.markAllDirty();
    }
}



//
// ## 亮度关系是这幅画成立的关键（第一版就是在这里失败的）
//
// 第一版把天空写成接近纯黑，结果**楼的剪影完全看不见** —— 黑剪影贴在黑天上，
// 只剩几个窗户亮点，画面空洞。修法是让天空充当**背光**：
//   天空：顶部近黑 -> 地平线附近明显变亮（黄昏余晖）
//   楼体：压到比天空更暗
// 楼的边缘有对比之后剪影才读得出来。这是"构图"层面的修正，
// 不是调一个参数能解决的。
// ===========================================================================
void buildNeonCity(Document& doc, int frame) {
    const int W = doc.width(), H = doc.height();
    const int n = prepare(doc, 4);
    const int horizon = H * 64 / 100;

    // ---- 层 0：天空（黄昏余晖，越近地平线越亮）+ 月亮 ----
    {
        Layer& L = doc.layer(0);
        // 多段渐变：顶部深蓝紫 -> 中段紫红 -> 地平线暖橙。
        // 只有把**大气层次**拉开，楼的剪影才立得住、画面才不空
        // （第一版是"黑天上的黑楼"，塌成一堆散点）。
        for (int y = 0; y < H; y++) {
            const float t0 = (float)y / (float)std::max(1, horizon);
            const float t = (t0 > 1.0f) ? 1.0f : t0;
            // 三段线性混合
            float r, g, b;
            if (t < 0.55f) {
                const float k = t / 0.55f;                       // 深蓝紫 -> 紫
                r = 14 + 48 * k;  g = 10 + 22 * k;  b = 40 + 40 * k;
            } else {
                const float k = (t - 0.55f) / 0.45f;             // 紫 -> 暖橙
                r = 62 + 150 * k; g = 32 + 78 * k;  b = 80 - 6 * k;
            }
            const RGBA8 c{clamp255((int)r), clamp255((int)g), clamp255((int)b), 255};
            for (int x = 0; x < W; x++) L.setPixelRaw(x, y, c, Material{});
        }
        // 城市辉光：地平线上方一条更亮的暖带（"城市灯光映在天上"）。
        // 它是剪影的背光，也是画面里最亮的大面积区域 —— 同时充当
        // "大面积柔光"的测试对象。
        for (int y = std::max(0, horizon - 16); y < horizon; y++) {
            const float k = 1.0f - (float)(horizon - y) / 16.0f;
            const RGBA8 c{clamp255((int)(232 * k)), clamp255((int)(126 * k)),
                          clamp255((int)(78 * k)), 255};
            for (int x = 0; x < W; x++) pset(L, x, y, c);
        }
        // 月亮：干净的柔光探针（低方向数下最容易出现环形/放射伪影）
        const int mx = W * 79 / 100, my = H * 19 / 100;
        for (int y = -11; y <= 11; y++)
            for (int x = -11; x <= 11; x++) {
                const int d2 = x * x + y * y;
                if (d2 > 121) continue;
                const float k = d2 <= 84 ? 1.0f : 0.40f;   // 实心 + 一圈柔边
                const RGBA8 c{(uint8_t)(196 * k), (uint8_t)(214 * k), (uint8_t)(255 * k), 255};
                pset(L, mx + x, my + y, c, emitMat(c.r, c.g, c.b));
            }
        L.markAllDirty();
    }

    // ---- 层 1：城市剪影。按 8px 分组取高度，保留像素画的块感 ----
    // 楼体比天空暗 -> 在地平线附近读得出剪影轮廓。
    {
        Layer& L = doc.layer(1);
        for (int gx = 0; gx < W; gx += 8) {
            const int bh = 20 + (int)(hash01(gx / 8, 0, 11) * 74.0f);
            const int top = horizon - bh;
            // 两种深浅 -> "前后两排楼"的层次
            const bool near = (hash01(gx / 8, 0, 13) > 0.5f);
            const RGBA8 body = near ? RGBA8{12, 11, 20, 255} : RGBA8{21, 19, 33, 255};
            for (int x = gx; x < gx + 8 && x < W; x++)
                for (int y = std::max(0, top); y < horizon; y++) pset(L, x, y, body);
        }
        // 地面：暗湿路面。它是**漫反射面**，霓虹的光会照亮它。
        for (int y = horizon; y < H; y++) {
            const int v = 30 - (y - horizon) * 12 / std::max(1, H - horizon);
            for (int x = 0; x < W; x++)
                pset(L, x, y, RGBA8{clamp255(v + 6), clamp255(v + 4), clamp255(v + 12), 255});
        }
        L.markAllDirty();
    }

    // ---- 层 2：窗户 + 五条霓虹管 ----
    {
        Layer& L = doc.layer(2);

        // 窗户：规则网格 + 确定性明灭。用 2x2 而不是单像素 ——
        // 单像素窗户在这个尺度下几乎看不见，2x2 才有"窗"的感觉。
        for (int gx = 7; gx < W - 8; gx += 9) {
            for (int gy = 26; gy < horizon - 8; gy += 8) {
                if (hash01(gx, gy, 23) > 0.55f) continue;
                const double period = 41.0 + 17.0 * hash01(gx, gy, 29);
                const float b = 0.40f + 0.60f * pulse(frame + (int)(hash01(gx, gy, 31) * 40), period);
                const bool warm = hash01(gx, gy, 37) > 0.25f;   // 暖黄为主，少量冷白
                const RGBA8 c{clamp255((int)((warm ? 255 : 210) * b)),
                              clamp255((int)((warm ? 208 : 228) * b)),
                              clamp255((int)((warm ? 132 : 255) * b)), 255};
                const Material m = emitMat(c.r, c.g, c.b);
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) pset(L, gx + dx, gy + dy, c, m);
            }
        }

        // 五条 1px 霓虹管 = 锐度探针（竖管最能暴露扩散的伤害）。
        // 扩散参数变化时，这几条是最先"糊掉"的地方 —— dir_probe 重点看这里。
        //
        // ⚠️ 位置都放在画布右半侧：bench_view 的读数面板占左上角，
        // 放在左边的管子会被面板挡住、看不到锐度变化。
        struct Tube { int x, y, len; bool vert; uint8_t r, g, b; int ph; };
        const Tube tubes[5] = {
            {156, horizon - 40, 46, false, 255,  46, 176,  0},   // 洋红横管
            {182, horizon - 82, 40, true,   62, 240, 255, 19},   // 青色竖管
            {238, horizon - 52, 56, false, 255, 198,  76, 37},   // 琥珀横管
            {262, horizon - 74, 30, true,  122, 255, 140, 53},   // 翠绿竖管
            {302, horizon - 92, 44, false, 190, 130, 255, 71},   // 紫罗兰横管
        };
        for (const Tube& t : tubes) {
            const float b = 0.70f + 0.30f * pulse(frame, 70.0 + t.ph);
            const RGBA8 c{clamp255((int)(t.r * b)), clamp255((int)(t.g * b)),
                          clamp255((int)(t.b * b)), 255};
            const Material m = emitMat(c.r, c.g, c.b);
            if (t.vert) line1px(L, t.x, t.y, t.x, t.y + t.len, c, m);
            else        line1px(L, t.x, t.y, t.x + t.len, t.y, c, m);
        }
        L.markAllDirty();
    }

    // ---- 层 3：前景雾。半透明薄片，缓慢漂移 ----
    // 它的作用：让**垂直自发光传输**可见 —— 霓虹的光必须穿过这层才到相机。
    // 把它关掉（整层 opacity=0），霓虹会立刻变亮，这就是可量化的差异。
    if (n > 3) {
        Layer& L = doc.layer(3);
        const int off = (int)((double)frame * 0.6) % 64;
        for (int y = 0; y < H; y += 4) {
            const int band = (y * 5 + off) % 64;
            const int a = 22 + band / 4;                       // 22..37
            const RGBA8 c{96, 140, 200, (uint8_t)a};
            for (int x = -64; x < W + 64; x += 4) {
                int px = (x + off) % (W + 128);
                if (px < 0) px += W + 128;
                px -= 64;                                       // 允许溢出，pset 会挡掉
                for (int k = 0; k < 4; k++) pset(L, px + k, y, c);
            }
        }
        L.markAllDirty();
    }
}

// ===========================================================================
// lantern-fog —— 垂直光路：光要穿过上方多层半透明雾
// ===========================================================================
void buildLanternFog(Document& doc, int frame) {
    const int W = doc.width(), H = doc.height();
    const int n = prepare(doc, 5);

    // ---- 层 0：夜色 + 底部石地 ----
    // 天空给一点**基底蓝**（不是纯黑）：雾要靠它才衬得出来，
    // 纯黑背景上雾几乎是看不见的，这个场景也就白做了。
    {
        Layer& L = doc.layer(0);
        for (int y = 0; y < H; y++) {
            const float t = (float)y / (float)H;
            const RGBA8 c{(uint8_t)(10 + 22 * t), (uint8_t)(14 + 30 * t),
                          (uint8_t)(26 + 44 * t), 255};
            for (int x = 0; x < W; x++) pset(L, x, y, c);
        }
        // 底部石地：暖一点的暗灰，给灯笼一个落脚面
        for (int y = H - 34; y < H; y++)
            for (int x = 0; x < W; x++)
                pset(L, x, y, RGBA8{(uint8_t)(44 + (y % 4) * 4),
                                    (uint8_t)(42 + (y % 4) * 4), 48, 255});
        L.markAllDirty();
    }

    // ---- 层 1：三盏灯笼（在雾的**下方**）----
    // 相位差 2π/3 的浮动，有节奏但不乱。
    {
        Layer& L = doc.layer(1);
        struct Lamp { int x, baseY; };
        const Lamp lamps[3] = {{W * 22 / 100, H - 70}, {W * 52 / 100, H - 96},
                               {W * 80 / 100, H - 60}};
        for (int i = 0; i < 3; i++) {
            const int y = lamps[i].baseY +
                          (int)(4.0 * std::sin(6.283185307179586 * (double)frame / 84.0 + i * 2.0944));
            // 外圈柔光（光在空气里散射）：低强度自发光，范围更大。
            // 它同时是"大面积柔光"的测试对象。
            for (int dy = -14; dy <= 14; dy++)
                for (int dx = -14; dx <= 14; dx++) {
                    const int d2 = dx * dx + dy * dy;
                    if (d2 <= 36 || d2 > 196) continue;
                    const int fall = 196 - d2;                  // 距离越远越弱
                    const uint8_t s = (uint8_t)(14 + fall / 12);
                    pset(L, lamps[i].x + dx, y + dy, RGBA8{255, 186, 104, 255},
                         emitMat((uint8_t)std::min(255, 60 + s), (uint8_t)std::min(255, 40 + s / 2),
                                 (uint8_t)std::min(255, 18 + s / 5)));
                }
            // 灯体：暖橙自发光（albedo 也暖，但发光独立）
            disc(L, lamps[i].x, y, 6, RGBA8{255, 182, 96, 255}, emitMat(255, 182, 96));
            // 灯顶挂绳（暗色，提供"硬边"对照）
            for (int k = 1; k <= 12; k++) pset(L, lamps[i].x, y - 6 - k, RGBA8{40, 34, 30, 255});
        }
        L.markAllDirty();
    }

    // ---- 层 2/3/4：三层雾，越往上越稀薄 ----
    // 每层不透明度不同（alpha 108/64/34），可以检验"逐层光场加性"。
    //
    // ⚠️ 雾必须**真的看得见**，否则这个场景就白做了 —— 它的全部意义就是
    // "光要穿过雾才到相机"。第一版雾太薄，画面上几乎察觉不到，
    // 于是既不好看、也测不出垂直传输。现在给足厚度，并且比每层更冷，
    // 与暖色灯笼形成对比。
    const uint8_t alphas[3] = {108, 64, 34};
    for (int li = 0; li < 3; li++) {
        if (2 + li >= n) break;
        Layer& L = doc.layer(2 + li);
        const int dir = (li % 2 == 0) ? 1 : -1;
        const int off = (int)((double)frame * (0.28 + 0.09 * li) * dir);
        const int ystep = 4 - li;                       // 越上层间距越大（更薄）
        for (int y = 0; y < H; y += std::max(2, ystep)) {
            const int a = alphas[li] + (y % 9) - 4;     // 轻微起伏
            const RGBA8 c{96, 122, 168, (uint8_t)std::max(10, a)};
            for (int x = -64; x < W + 64; x += 5) {
                int px = (x + off) % (W + 128);
                if (px < 0) px += W + 128;
                px -= 64;
                for (int k = 0; k < 5; k++) pset(L, px + k, y, c);
            }
        }
        L.markAllDirty();
    }
}

// ===========================================================================
// stained-glass —— 半透明染色 + 硬阴影 + 彩色光互染
// ===========================================================================
void buildStainedGlass(Document& doc, int frame) {
    const int W = doc.width(), H = doc.height();
    const int n = prepare(doc, 3);
    const int x0 = W / 2 - 84, y0 = H / 2 - 52;
    const int pw = 42, ph = 52;                 // 4 列 × 2 行

    // ---- 层 0：背后的白光块（大面积**自发光**，不靠环境光）----
    // 注意：白块必须有自发光，否则在黑色环境光下是暗的（模型正确行为）。
    {
        Layer& L = doc.layer(0);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) pset(L, x, y, RGBA8{8, 8, 12, 255});
        for (int y = y0 - 6; y < y0 + ph * 2 + 6; y++)
            for (int x = x0 - 6; x < x0 + pw * 4 + 6; x++) {
                const RGBA8 c{255, 248, 228, 255};
                pset(L, x, y, c, emitMat(255, 248, 228));
            }
        L.markAllDirty();
    }

    // ---- 层 1：8 块彩色玻璃（半透明 -> 让光染色后穿过）----
    {
        Layer& L = doc.layer(1);
        const uint8_t cols[8][3] = {
            {222,  62,  84}, {244, 182,  62}, { 82, 202, 122}, { 70, 140, 240},
            {222, 122, 202}, {120, 222, 222}, {240, 222, 100}, {158, 100, 220},
        };
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 4; c++) {
                const int gx = x0 + c * pw, gy = y0 + r * ph;
                // 半透明只用于**薄片**：大块半透明会因逐格吸收累积成实心
                const RGBA8 col{cols[r * 4 + c][0], cols[r * 4 + c][1],
                                cols[r * 4 + c][2], 158};
                for (int y = gy; y < gy + ph; y++)
                    for (int x = gx; x < gx + pw; x++) pset(L, x, y, col);
            }
        L.markAllDirty();
    }

    // ---- 层 2：铅条窗框（不透明黑 -> 硬阴影），每帧 ±1px 呼吸 ----
    {
        Layer& L = doc.layer(2);
        const int jit = (int)(1.5 * std::sin(6.283185307179586 * (double)frame / 140.0));
        const RGBA8 lead{10, 10, 14, 255};
        for (int i = 0; i <= 4; i++) {
            const int gx = x0 + i * pw + jit;
            line1px(L, gx, y0 - 6, gx, y0 + ph * 2 + 6, lead, Material{});
        }
        for (int j = 0; j <= 2; j++) {
            const int gy = y0 + j * ph;
            line1px(L, x0 - 6, gy, x0 + pw * 4 + 6, gy, lead, Material{});
        }
        L.markAllDirty();
    }
    (void)n;
}

// ===========================================================================
// chromatic —— 自发光与 albedo 解耦（"暗表面发绿光"）
// ===========================================================================
void buildChromatic(Document& doc, int frame) {
    const int W = doc.width(), H = doc.height();
    const int n = prepare(doc, 3);

    // ---- 层 0：纯黑背景 ----
    {
        Layer& L = doc.layer(0);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) pset(L, x, y, RGBA8{0, 0, 0, 255});
        L.markAllDirty();
    }

    // ---- 层 1：8 个光球。albedo 全黑、自发光是饱和色 ----
    // 如果实现把自发光乘上了 albedo，这些球会**全黑** —— 一眼可判的回归探针。
    // 球外包一圈低强度同色 halo：既让画面好看，又制造**相邻异色光源**，
    // 于是"彩色光互染"也被这个场景压到了。
    {
        Layer& L = doc.layer(1);
        const uint8_t cols[8][3] = {
            {255,  62,  62}, {255, 158,  62}, {255, 240,  62}, {122, 255,  62},
            { 62, 255, 178}, { 62, 198, 255}, {142, 100, 255}, {255,  82, 202},
        };
        for (int i = 0; i < 8; i++) {
            const double a = i * 0.7853981633974483;   // 45° 一个
            const int cx = W / 2 + (int)(0.34 * W * std::cos(a));
            const int cy = H / 2 + (int)(0.34 * H * std::sin(a));
            const float pulseI = pulse(frame + i * 11, 96.0);
            const int rad = 7 + (int)(2.0f * pulseI);
            // halo：同色、低强度，半径更大
            const uint8_t hr = (uint8_t)(cols[i][0] / 4), hg = (uint8_t)(cols[i][1] / 4),
                          hb = (uint8_t)(cols[i][2] / 4);
            for (int dy = -rad - 5; dy <= rad + 5; dy++)
                for (int dx = -rad - 5; dx <= rad + 5; dx++) {
                    const int d2 = dx * dx + dy * dy;
                    if (d2 <= rad * rad || d2 > (rad + 5) * (rad + 5)) continue;
                    pset(L, cx + dx, cy + dy, RGBA8{0, 0, 0, 255}, emitMat(hr, hg, hb));
                }
            // 核心：满强度
            disc(L, cx, cy, rad, RGBA8{0, 0, 0, 255},
                 emitMat(cols[i][0], cols[i][1], cols[i][2]));
        }
        L.markAllDirty();
    }

    // ---- 层 2：缓慢旋转的半透明薄膜（薄片，不是大块）----
    if (n > 2) {
        Layer& L = doc.layer(2);
        const double rot = (double)frame * 0.9;
        for (int i = 0; i < 3; i++) {
            const double a = rot * 0.017453292519943295 + i * 2.0944;
            const int x1 = W / 2 + (int)(0.44 * W * std::cos(a));
            const int y1 = H / 2 + (int)(0.44 * H * std::sin(a));
            line1px(L, x1, y1, W - x1, H - y1, RGBA8{176, 198, 255, 76}, Material{});
            line1px(L, x1 + 1, y1, W - x1 + 1, H - y1, RGBA8{176, 198, 255, 60}, Material{});
        }
        L.markAllDirty();
    }
}

// ===========================================================================
// grid-lab —— 纯诊断图（静止）。专供 dir_probe 出误差放大图。
// ===========================================================================
void buildGridLab(Document& doc) {
    const int W = doc.width(), H = doc.height();
    const int n = prepare(doc, 2);

    // 层 0：白底（高 albedo、无自发光）
    {
        Layer& L = doc.layer(0);
        L.image().fill(RGBA8{255, 255, 255, 255});
        for (auto& m : L.material) m = Material{};
        L.markAllDirty();
    }
    // 层 1：四种形态，位置严格固定。
    // ⚠️ 全部避开画布左上角 —— bench_view 的读数面板盖在那里，
    // 放左上角的探针会被挡住、看不到伪影。
    {
        Layer& L = doc.layer(1);
        const RGBA8 black{0, 0, 0, 255};
        const Material em = emitMat(255, 255, 255);
        const int lx = W * 30 / 100, rx = W * 72 / 100;
        const int ty = H * 28 / 100, by = H * 74 / 100;

        // 左上区：孤立点（δ 源，辐条最强）
        pset(L, lx, ty, black, em);

        // 右上区：1px 竖线（细长光源 —— 扩散最容易伤害它）
        for (int i = -16; i <= 16; i++) pset(L, rx, ty + i, black, em);

        // 左下区：细网格（周期性结构，最容易暴露角度混叠）
        for (int x = lx - 24; x <= lx + 24; x += 4)
            for (int y = by - 24; y <= by + 24; y++) pset(L, x, y, black, em);
        for (int y = by - 24; y <= by + 24; y += 4)
            for (int x = lx - 24; x <= lx + 24; x++) pset(L, x, y, black, em);

        // 右下区：实心方块（宽光源，辐条应明显更轻 —— 对照）
        for (int y = by - 12; y < by + 12; y++)
            for (int x = rx - 12; x < rx + 12; x++) pset(L, x, y, black, em);
        L.markAllDirty();
    }
    (void)n;
}

// ===========================================================================
// legacy —— 原来的 5 层动画基准（逐字保留，保证历史数字可复现）
// ===========================================================================
void legacyStep(Document& doc, int frame) {
    const int n = doc.layerCount();
    const int W = doc.width(), H = doc.height();
    const double t = (double)frame;
    if (n <= 0) return;

    // --- 层 0：滚动的渐变背景（实心，制造遮挡与投影）---
    {
        Layer& L = doc.layer(0);
        for (int y = 0; y < H; y += 2) {
            const int wave = (int)(24.0 * std::sin((double)y * 0.05 + t * 0.06));
            const int shade = 18 + ((y * 3 + (int)(t * 1.5)) % 34) + wave / 3;
            const RGBA8 col{clamp255(14 + y / 8), clamp255(shade), clamp255(30 + y / 4), 255};
            for (int x = 0; x < W; x++) L.setPixelRaw(x, y, col, Material{});
            for (int x = 0; x < W; x++) L.setPixelRaw(x, y + 1, col, Material{});
        }
        L.markAllDirty();
    }

    // --- 层 1：来回弹的彩色实心块（不透明，负责挡光/投影）---
    if (n > 1) {
        Layer& L = doc.layer(1);
        clearLayer(L);
        for (int i = 0; i < 6; i++) {
            const double ph = t * 0.05 + i * 1.05;
            const int bx = (int)(W * 0.5 + (W * 0.34) * std::sin(ph * 1.3 + i));
            const int by = (int)(H * 0.5 + (H * 0.32) * std::cos(ph * 0.9 + i * 0.7));
            const int sz = 10 + (i % 3) * 4;
            uint8_t r, g, b;
            hueToRgb(ph * 0.16 + i * 0.13, r, g, b);
            const RGBA8 col{r, g, b, 255};
            for (int y = -sz; y <= sz; y++) {
                for (int x = -sz; x <= sz; x++) {
                    if (x * x + y * y > sz * sz) continue;
                    const int px = bx + x, py = by + y;
                    if (px < 0 || py < 0 || px >= W || py >= H) continue;
                    L.setPixelRaw(px, py, col, Material{});
                }
            }
        }
        L.markAllDirty();
    }

    // --- 层 2：半透明彩色光带（逐格吸收 + 染色）---
    if (n > 2) {
        Layer& L = doc.layer(2);
        clearLayer(L);
        for (int i = 0; i < 5; i++) {
            const double ph = t * 0.04 + i * 1.2566;
            uint8_t r, g, b;
            hueToRgb(ph * 0.2 + (double)i / 5.0, r, g, b);
            const RGBA8 col{r, g, b, 96};
            for (int x = 0; x < W; x++) {
                const int y = (int)(H * 0.5 +
                                    (H * 0.42) * std::sin((double)x * 0.018 + ph * 2.2));
                for (int d = -3; d <= 3; d++) {
                    const int py = y + d;
                    if (py < 0 || py >= H) continue;
                    L.setPixelRaw(x, py, col, Material{});
                }
            }
        }
        L.markAllDirty();
    }

    // --- 层 3：大量漂移的自发光粒子（自发光 + 光场的主要来源）---
    if (n > 3) {
        Layer& L = doc.layer(3);
        clearLayer(L);
        const int count = 220;
        for (int i = 0; i < count; i++) {
            const double seed = (double)i * 2.399963;
            const double sp = 0.4 + (double)(i % 7) * 0.13;
            const int px = (int)std::fmod(
                (double)W + 40.0 + seed * 37.0 + t * sp * (i % 2 ? 1.0 : -1.0),
                (double)W + 80.0) - 40;
            const int py = (int)std::fmod(
                (double)H + 20.0 + seed * 91.0 + t * sp * 0.6 * ((i % 3) ? 1.0 : -1.0),
                (double)H + 40.0) - 20;
            uint8_t r, g, b;
            hueToRgb(0.55 + 0.35 * std::sin(seed * 0.7 + t * 0.01), r, g, b);
            const RGBA8 col{r, g, b, 150};
            Material m;
            m.setEmissionScaled(255, 255, 255, 200);
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx * dx + dy * dy > 2) continue;
                    const int qx = px + dx, qy = py + dy;
                    if (qx < 0 || qy < 0 || qx >= W || qy >= H) continue;
                    L.setPixelRaw(qx, qy, col, m);
                }
            }
        }
        L.markAllDirty();
    }

    // --- 层 4：横扫的高亮光斑 ---
    if (n > 4) {
        Layer& L = doc.layer(4);
        clearLayer(L);
        const double ph = t * 0.035;
        for (int i = 0; i < 3; i++) {
            const double a2 = ph + i * 2.0944;
            const int cx = (int)(W * 0.5 + (W * 0.4) * std::cos(a2));
            const int cy = (int)(H * 0.5 + (H * 0.4) * std::sin(a2));
            uint8_t r, g, b;
            hueToRgb(a2 * 0.25 + 0.5, r, g, b);
            const RGBA8 col{r, g, b, 120};
            Material m;
            m.setEmissionScaled(r, g, b, 235);
            for (int dy = -14; dy <= 14; dy++) {
                for (int dx = -14; dx <= 14; dx++) {
                    if (dx * dx + dy * dy > 196) continue;
                    const int qx = cx + dx, qy = cy + dy;
                    if (qx < 0 || qy < 0 || qx >= W || qy >= H) continue;
                    L.setPixelRaw(qx, qy, col, m);
                }
            }
        }
        L.markAllDirty();
    }

    // --- 其余图层：错相位的滚动条纹 ---
    for (int i = 5; i < n; i++) {
        Layer& L = doc.layer(i);
        clearLayer(L);
        const double ph = t * 0.02 + i * 0.8;
        for (int y = 0; y < H; y++) {
            if (((y + (int)(t * 1.2 * ((i % 3) + 1))) % 12) >= 4) continue;
            uint8_t r, g, b;
            hueToRgb(ph * 0.1 + (double)i * 0.17, r, g, b);
            const RGBA8 col{r, g, b, 130};
            for (int x = 0; x < W; x++) L.setPixelRaw(x, y, col, Material{});
        }
        L.markAllDirty();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 场景表
// ---------------------------------------------------------------------------
const SceneDesc kScenes[(int)SceneId::Count] = {
    {"neon-city", "1px 霓虹锐度 + 大面积泛光 + 垂直自发光传输", 4, 32, 0.10f, 8, false, false},
    {"lantern-fog", "垂直光路：光要穿过上方多层半透明雾", 5, 32, 0.05f, 8, false, false},
    {"stained-glass", "半透明染色 + 硬阴影 + 彩色光互染", 3, 32, 0.05f, 8, false, false},
    {"chromatic", "自发光与 albedo 解耦（暗表面发绿光）", 3, 32, 0.10f, 8, false, false},
    {"neon-rain", "雨夜霓虹街景：长焦平面构图 / 5 层 / 实时雨幕（性能 + 审美）", 5, 64, 0.08f, 8, false, false},
    {"grid-lab", "定量诊断：点/线/网格/方块（静止，供 dir_probe）", 2, 32, 0.10f, 8, false, false},
    {"legacy", "旧的 5 层动画基准（历史数字可复现）", 5, 128, 0.00f, 8, false, false},
    // 低配档。用途很具体：**验证集显能不能玩**。
    //   R=32 是"还能叫光线追踪"的下限（184 个方向）
    //   单层是因为逐层扫描成本严格线性（每层 +0.25ms@R8 / +1.2ms@R32）
    //   扩散是因为它让你不必为了性能去砍半径 —— 砍半径比砍层数伤画质得多
    {"lowend", "低配档：单层 + R=32 + 扩散（集显可玩，测帧率）", 1, 32, 0.10f, 8, false, true},
};

const char* sceneName(SceneId id) {
    const int i = (int)id;
    return (i >= 0 && i < (int)SceneId::Count) ? kScenes[i].name : "?";
}

bool sceneByName(const std::string& name, SceneId& out) {
    // 兼容旧名字：--diffscene 原来指向那个"看辐条"的静态场景，现在就是 grid-lab
    if (name == "diffscene") { out = SceneId::GridLab; return true; }
    for (int i = 0; i < (int)SceneId::Count; i++) {
        if (name == kScenes[i].name) { out = (SceneId)i; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------------
void buildDoc(Document& doc, SceneId id, int layers) {
    // 尺寸**不在这里改**：光照的 GPU 纹理在初始化时按当时的画布尺寸分配，
    // late resize 会让两侧不一致。尺寸由调用方在初始化光照之前定好。
    const int want = (layers > 0) ? layers : kScenes[(int)id].layers;
    const int n = prepare(doc, want);
    doc.setActiveIndex(0);

    switch (id) {
        case SceneId::GridLab:
            buildGridLab(doc);
            break;
        case SceneId::NeonCity:
            buildNeonCity(doc, 0);
            break;
        case SceneId::LanternFog:
            buildLanternFog(doc, 0);
            break;
        case SceneId::StainedGlass:
            buildStainedGlass(doc, 0);
            break;
        case SceneId::Chromatic:
            buildChromatic(doc, 0);
            break;
        case SceneId::NeonRain:
            buildNeonRain(doc, 0);
            break;
        case SceneId::LowEnd:
        case SceneId::Legacy:
        default:
            // lowend 复用 legacy 的动画内容，只是层数被定成 1 层
            legacyStep(doc, 0);
            break;
    }
    // 场景名与用途也写进图层名，方便在画板的图层面板里看出来
    if (doc.layerCount() > 0) doc.layer(0).name = kScenes[(int)id].name;
    printf("[bench] 场景 %s：%d 层，%dx%d —— %s\n", kScenes[(int)id].name, n, doc.width(),
           doc.height(), kScenes[(int)id].purpose);
}
void stepDoc(Document& doc, SceneId id, int frame) {
    switch (id) {
        case SceneId::GridLab:
            break;   // 完全静止：dir_probe 需要可复现
        case SceneId::NeonCity:
            buildNeonCity(doc, frame);
            break;
        case SceneId::LanternFog:
            buildLanternFog(doc, frame);
            break;
        case SceneId::StainedGlass:
            buildStainedGlass(doc, frame);
            break;
        case SceneId::Chromatic:
            buildChromatic(doc, frame);
            break;
        case SceneId::NeonRain:
            buildNeonRain(doc, frame);
            break;
        case SceneId::LowEnd:
        case SceneId::Legacy:
        default:
            legacyStep(doc, frame);
            break;
    }
}

// 注：这里**不提供 App 包装** —— 本文件刻意只依赖 Document，
// 这样 bench_view（没有 App）和 paintboard 才能共用同一份场景。
// 需要 App 的调用方（app/main.cpp）直接调 buildDoc(a.doc, ...) 即可。

}  // namespace bench
}  // namespace pb
