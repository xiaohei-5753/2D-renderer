// tools/core_test.cpp — core 层的 headless 测试
//
// core 层刻意不依赖 OpenGL / GLFW，所以可以在没有窗口、没有 GPU 的环境下直接验证
// 工具、撤销重做、调色板的**行为**是否正确。这同时也是我验证改动的手段：
// 交互式画板的界面我看不到，但它的编辑语义必须能被自动化证明。
//
// 退出码 0 = 全部通过。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/console.h"
#include "core/history.h"
#include "core/material.h"
#include "core/palette.h"
#include "core/tool.h"
#include "core/types.h"

using namespace pb;

namespace {

int g_pass = 0;
int g_fail = 0;
std::string g_section;

void section(const char* name) {
    g_section = name;
    printf("\n--- %s ---\n", name);
}

void check(bool ok, const char* what, int line) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s  (core_test.cpp:%d)\n", what, line);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

const RGBA8 kWhite{255, 255, 255, 255};
const RGBA8 kRed{255, 0, 0, 255};
const RGBA8 kGreen{0, 255, 0, 255};
const RGBA8 kAir{0, 0, 0, 0};

int countSet(const Canvas& c) {
    int n = 0;
    const Image& img = c.image();
    for (size_t i = 0; i < img.pixelCount(); i++)
        if (img.data()[i].a > 0) n++;
    return n;
}

int countSetInRow(const Canvas& c, int y) {
    int n = 0;
    for (int x = 0; x < c.width(); x++)
        if (c.image().at(x, y).a > 0) n++;
    return n;
}

int countSetInCol(const Canvas& c, int x) {
    int n = 0;
    for (int y = 0; y < c.height(); y++)
        if (c.image().at(x, y).a > 0) n++;
    return n;
}

bool samePixel(const Canvas& a, const Canvas& b, int x, int y) {
    return a.image().at(x, y) == b.image().at(x, y) &&
           a.materialAt(x, y) == b.materialAt(x, y);
}

bool sameCanvas(const Canvas& a, const Canvas& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            if (!samePixel(a, b, x, y)) return false;
    return true;
}

Canvas makeCanvas(int w, int h) {
    Canvas c(w, h);
    c.clear(kAir, Material{});
    return c;
}

// ---------------------------------------------------------------------------
// 测试：绘制原语
// ---------------------------------------------------------------------------

void testLines() {
    section("绘制原语 / 直线（对称 Bresenham 的方向约定）");

    {
        Canvas c = makeCanvas(32, 32);
        c.drawLine(2, 5, 12, 5, kWhite);
        CHECK(countSet(c) == 11);            // 2..12 含端点共 11 个
        CHECK(countSetInRow(c, 5) == 11);
        CHECK(countSetInCol(c, 2) == 1);
        CHECK(countSetInCol(c, 12) == 1);
    }
    {
        Canvas c = makeCanvas(32, 32);
        c.drawLine(7, 1, 7, 9, kWhite);
        CHECK(countSet(c) == 9);
        CHECK(countSetInCol(c, 7) == 9);
    }
    {
        Canvas c = makeCanvas(32, 32);
        c.drawLine(0, 0, 10, 10, kWhite);
        CHECK(countSet(c) == 11);
        // 45 度线每个 x 恰好一个点
        bool onePerCol = true;
        for (int x = 0; x <= 10; x++)
            if (countSetInCol(c, x) != 1) onePerCol = false;
        CHECK(onePerCol);
    }
    {
        // 反向画同一条线必须得到同一组像素。
        // 这依赖 Canvas::drawLine 里的端点规范化：裸的 Bresenham 在误差平局时会偏向
        // 固定一侧，对调端点会画出线的另一侧。
        Canvas a = makeCanvas(32, 32);
        Canvas b = makeCanvas(32, 32);
        a.drawLine(3, 4, 25, 17, kWhite);
        b.drawLine(25, 17, 3, 4, kWhite);
        CHECK(sameCanvas(a, b));
    }
    {
        // 越界坐标不应该写出画布外，也不应该崩溃
        Canvas c = makeCanvas(16, 16);
        c.drawLine(-20, -20, 40, 40, kWhite);
        CHECK(countSet(c) == 16);   // 对角线恰好穿过 16 行
    }
}

void testRect() {
    section("绘制原语 / 矩形");

    {
        Canvas c = makeCanvas(16, 16);
        c.rectOutline(1, 1, 5, 5, kWhite);
        CHECK(countSet(c) == 16);   // 5x5 周长 = 5*4-4
    }
    {
        Canvas c = makeCanvas(16, 16);
        c.fillRect(2, 2, 4, 4, kWhite);
        CHECK(countSet(c) == 9);    // 3x3
    }
    {
        // 坐标反着传也要正确（内部会 swap）
        Canvas a = makeCanvas(16, 16);
        Canvas b = makeCanvas(16, 16);
        a.fillRect(2, 2, 6, 8, kWhite);
        b.fillRect(6, 8, 2, 2, kWhite);
        CHECK(sameCanvas(a, b));
    }
    {
        // 超出边界的矩形要被裁掉而不是越界写
        Canvas c = makeCanvas(16, 16);
        c.fillRect(-5, -5, 100, 100, kWhite);
        CHECK(countSet(c) == 16 * 16);
    }
}

void testEllipse() {
    section("绘制原语 / 椭圆");

    {
        // 实心圆的面积应接近 pi*r^2
        Canvas c = makeCanvas(32, 32);
        c.drawEllipse(0, 0, 20, 20, true, kWhite);
        const int n = countSet(c);
        // r = 10.5（半整数），面积约 346；给一个宽松但有效的窗口
        CHECK(n >= 300 && n <= 380);
        // 圆心在，四角不在
        CHECK(c.image().at(10, 10).a > 0);
        CHECK(c.image().at(0, 0).a == 0);
        CHECK(c.image().at(20, 0).a == 0);
        CHECK(c.image().at(0, 20).a == 0);
        CHECK(c.image().at(20, 20).a == 0);
    }
    {
        // 空心圆：四重对称 + 全部落在包围盒内 + 四角不画
        Canvas c = makeCanvas(32, 32);
        c.drawEllipse(0, 0, 20, 20, false, kWhite);
        CHECK(countSet(c) > 0);

        bool symmetric = true;
        bool inBox = true;
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                const bool on = c.image().at(x, y).a > 0;
                if (!on) continue;
                if (x > 20 || y > 20) inBox = false;
                // 关于中心 (10,10) 的四重对称
                if (c.image().at(20 - x, y).a == 0) symmetric = false;
                if (c.image().at(x, 20 - y).a == 0) symmetric = false;
                if (c.image().at(20 - x, 20 - y).a == 0) symmetric = false;
            }
        }
        CHECK(inBox);
        CHECK(symmetric);
        CHECK(c.image().at(0, 0).a == 0);
    }
    {
        // 极扁椭圆（高 1）：这是朴素中点算法会画不完整的情况，Zingl 的修正专门处理它
        Canvas c = makeCanvas(32, 8);
        c.drawEllipse(1, 3, 29, 4, false, kWhite);
        CHECK(countSet(c) > 0);
        // 最左和最右必须被画到
        bool hasLeft = false, hasRight = false;
        for (int x = 0; x < 32; x++) {
            if (c.image().at(x, 3).a > 0 || c.image().at(x, 4).a > 0) {
                if (!hasLeft) hasLeft = true;
                hasRight = true;
            }
        }
        CHECK(hasLeft);
        CHECK(hasRight);
        CHECK(c.image().at(1, 3).a > 0 || c.image().at(1, 4).a > 0);
    }
    {
        // 单点退化
        Canvas c = makeCanvas(8, 8);
        c.drawEllipse(4, 4, 4, 4, false, kWhite);
        CHECK(countSet(c) == 1);
    }
}

void testFloodFill() {
    section("绘制原语 / 洪水填充");

    {
        Canvas c = makeCanvas(20, 20);
        c.rectOutline(2, 2, 12, 12, kWhite);
        const int filled = c.floodFill(7, 7, kRed);
        CHECK(filled == 81);   // 内部 9x9
        // 边框没被改
        CHECK(c.image().at(2, 2) == kWhite);
        // 外面也没被波及
        CHECK(c.image().at(0, 0).a == 0);
        CHECK(c.image().at(19, 19).a == 0);
    }
    {
        // 填充同色应该什么都不做（也避免死循环）
        Canvas c = makeCanvas(8, 8);
        c.fill(kRed, Material{});
        CHECK(c.floodFill(4, 4, kRed) == 0);
    }
    {
        // 越界起点不应崩溃
        Canvas c = makeCanvas(8, 8);
        CHECK(c.floodFill(-1, 4, kRed) == 0);
        CHECK(c.floodFill(100, 4, kRed) == 0);
    }
}

// ---------------------------------------------------------------------------
// 测试：撤销 / 重做
// ---------------------------------------------------------------------------

void testHistoryBasics() {
    section("撤销重做 / 基本正确性");

    Canvas c = makeCanvas(32, 32);
    Canvas original = c;

    History h;
    h.beginStroke(c);
    c.drawLine(1, 1, 20, 20, kWhite);
    CHECK(h.endStroke(c, "直线"));
    const Canvas afterStroke = c;

    CHECK(h.canUndo());
    CHECK(!h.canRedo());
    CHECK(h.undoSteps() == 1);

    CHECK(h.undo(c));
    CHECK(sameCanvas(c, original));      // 逐像素 + 材质完全还原
    CHECK(h.canRedo());
    CHECK(h.redoSteps() == 1);

    CHECK(h.redo(c));
    CHECK(sameCanvas(c, afterStroke));
    CHECK(h.undoSteps() == 1);
    CHECK(!h.canRedo());
}

void testHistoryNoChange() {
    section("撤销重做 / 无变化不污染栈");

    Canvas c = makeCanvas(16, 16);
    History h;

    h.beginStroke(c);
    CHECK(!h.endStroke(c, "空操作"));   // 没改任何像素
    CHECK(!h.canUndo());
    CHECK(h.undoSteps() == 0);

    // 写回原值也应该算作"无变化"
    h.beginStroke(c);
    c.setPixel(3, 3, kAir, Material{});
    CHECK(!h.endStroke(c, "写回原值"));
    CHECK(!h.canUndo());
}

void testHistoryPatchSize() {
    section("撤销重做 / 只存变化矩形（内存）");

    Canvas c = makeCanvas(256, 256);
    History h;
    h.beginStroke(c);
    c.setPixel(100, 100, kWhite, Material{});
    CHECK(h.endStroke(c, "单像素"));

    // 1x1 补丁 = 一个像素的颜色 + 材质。
    // 用 sizeof 而不是写死数字：Material 从 4 字节长到 6 字节（自发光变成独立 RGB）
    // 时这个断言就失效过，写死数字的测试反而会误导。
    const size_t perPixel = sizeof(RGBA8) + sizeof(Material);
    CHECK(h.bytes() == perPixel);

    // 一小段线也只应是那个外接矩形
    h.beginStroke(c);
    c.drawLine(10, 10, 20, 10, kWhite, Material{});
    CHECK(h.endStroke(c, "短线"));
    const size_t b = h.bytes();
    CHECK(b == perPixel * (1 + 11));
}

void testHistoryStack() {
    section("撤销重做 / 多步与分支");

    Canvas c = makeCanvas(32, 32);
    Canvas original = c;
    History h;

    // 5 笔，每笔一个不同位置的像素
    for (int i = 0; i < 5; i++) {
        h.beginStroke(c);
        c.setPixel(i, 0, kWhite, Material{});
        CHECK(h.endStroke(c, "点"));
    }
    CHECK(h.undoSteps() == 5);

    // 全部撤销
    for (int i = 0; i < 5; i++) CHECK(h.undo(c));
    CHECK(sameCanvas(c, original));
    CHECK(!h.canUndo());
    CHECK(h.redoSteps() == 5);

    // 全部重做
    for (int i = 0; i < 5; i++) CHECK(h.redo(c));
    CHECK(!h.canRedo());
    for (int i = 0; i < 5; i++) CHECK(c.image().at(i, 0) == kWhite);

    // 撤销两步后做新操作 -> 重做链必须失效
    CHECK(h.undo(c));
    CHECK(h.undo(c));
    CHECK(h.redoSteps() == 2);
    h.beginStroke(c);
    c.setPixel(20, 20, kGreen, Material{});
    CHECK(h.endStroke(c, "新分支"));
    CHECK(!h.canRedo());
    CHECK(h.redoSteps() == 0);
    // 新分支保留，旧的前 3 步仍在撤销栈里
    CHECK(h.undoSteps() == 4);
}

void testHistoryMaxSteps() {
    section("撤销重做 / 步数上限裁剪");

    Canvas c = makeCanvas(16, 16);
    History h;
    h.maxSteps = 8;

    for (int i = 0; i < 20; i++) {
        h.beginStroke(c);
        c.setPixel(i % 16, i / 16, kWhite, Material{});
        h.endStroke(c, "点");
    }
    CHECK(h.undoSteps() == 8);
    // 还能正常撤销 8 次
    int undone = 0;
    while (h.undo(c)) undone++;
    CHECK(undone == 8);
}

// ---------------------------------------------------------------------------
// 测试：工具
// ---------------------------------------------------------------------------

void testToolPencil() {
    section("工具 / 铅笔连续性（快速拖动不留断点）");

    Canvas c = makeCanvas(32, 32);
    ToolOptions o;
    o.tool = ToolId::Pencil;
    o.color = kWhite;
    o.brushSize = 1;

    ToolSession s;
    s.begin(c, o, 0, 0);
    // 模拟一次跨越较远的鼠标移动：如果不做插值，只会有两个点
    s.update(c, o, 20, 10);
    s.end();

    CHECK(countSet(c) > 2);
    // dx > dy 的线，每一列都应该有像素
    bool everyCol = true;
    for (int x = 0; x <= 20; x++)
        if (countSetInCol(c, x) == 0) everyCol = false;
    CHECK(everyCol);
}

void testToolBrushShape() {
    section("工具 / 笔刷大小与形状");

    {
        Canvas c = makeCanvas(32, 32);
        ToolOptions o;
        o.tool = ToolId::Pencil;
        o.color = kWhite;
        o.brushSize = 5;
        o.brushRound = false;
        stampBrush(c, 10, 10, o, o.color, Material{});
        CHECK(countSet(c) == 25);   // 5x5 方
    }
    {
        Canvas c = makeCanvas(32, 32);
        ToolOptions o;
        o.tool = ToolId::Pencil;
        o.color = kWhite;
        o.brushSize = 5;
        o.brushRound = true;
        stampBrush(c, 10, 10, o, o.color, Material{});
        const int n = countSet(c);
        CHECK(n > 12 && n < 25);    // 圆比同尺寸方小
        CHECK(c.image().at(10, 10).a > 0);   // 圆心在
        CHECK(c.image().at(8, 8).a == 0);    // 角上不在
    }
    {
        // 笔刷在画布边缘要被裁掉而不是越界
        Canvas c = makeCanvas(16, 16);
        ToolOptions o;
        o.tool = ToolId::Pencil;
        o.color = kWhite;
        o.brushSize = 7;
        stampBrush(c, 0, 0, o, o.color, Material{});
        CHECK(countSet(c) == 16);   // 4x4 象限
    }
}

void testToolShapePreview() {
    section("工具 / 形状预览回滚（不该留下上一次的预览）");

    Canvas c = makeCanvas(32, 32);
    ToolOptions o;
    o.tool = ToolId::Rect;
    o.color = kWhite;
    o.shapeFilled = false;

    ToolSession s;
    s.begin(c, o, 2, 2);
    s.update(c, o, 6, 6);      // 先预览一个 2..6 的矩形
    CHECK(c.image().at(6, 6).a > 0);

    s.update(c, o, 20, 20);    // 再拉到 2..20：旧预览必须被回滚
    s.end();

    CHECK(c.image().at(20, 20).a > 0);   // 新矩形在
    // (6,6) 现在是新矩形内部，描边模式下不应被画上
    CHECK(c.image().at(6, 6).a == 0);
    // 旧矩形的右边 x=6 也只在新矩形内部 -> 没有竖线残留
    CHECK(countSetInCol(c, 6) == 2);     // 只有上边(y=2)和下边(y=20)
}

void testToolEraserAndPicker() {
    section("工具 / 橡皮与取色");

    Canvas c = makeCanvas(16, 16);
    c.fillRect(0, 0, 7, 7, kRed, Material{});
    CHECK(countSet(c) == 64);

    ToolOptions o;
    o.tool = ToolId::Eraser;
    o.brushSize = 1;
    ToolSession s;
    s.begin(c, o, 3, 3);
    s.end();
    CHECK(c.image().at(3, 3).a == 0);
    CHECK(countSet(c) == 63);

    // 取色器：不改画布，且工具本身声明为"不修改"
    CHECK(!ToolSession::toolModifies(ToolId::Picker));
    RGBA8 got;
    Material gm;
    CHECK(pickPixel(c, 1, 1, got, gm));
    CHECK(got == kRed);
    CHECK(!pickPixel(c, -1, 1, got, gm));
}

void testToolDither() {
    section("工具 / 抖动（确定性 + 覆盖率）");

    // Bayer 4x4 在 50% 时应当恰好保留一半
    int n = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (ditherTest(x, y, 50)) n++;
    CHECK(n == 128);

    int all = 0, none = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            if (ditherTest(x, y, 100)) all++;
            if (ditherTest(x, y, 0)) none++;
        }
    CHECK(all == 64);
    CHECK(none == 0);

    // 图案必须是位置确定的（同一坐标重复查询结果一致）
    bool stable = true;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (ditherTest(x, y, 37) != ditherTest(x, y, 37)) stable = false;
    CHECK(stable);

    // 通过工具走一遍
    Canvas c = makeCanvas(16, 16);
    ToolOptions o;
    o.tool = ToolId::Dither;
    o.color = kWhite;
    o.brushSize = 16;
    o.ditherLevel = 50;
    stampBrush(c, 8, 8, o, o.color, Material{});
    CHECK(countSet(c) > 0 && countSet(c) < 16 * 16);
}

void testToolShade() {
    section("工具 / 加光减光只作用于已有的像素");

    Canvas c = makeCanvas(8, 8);
    c.setPixel(2, 2, RGBA8{100, 100, 100, 255}, Material{});

    ToolOptions o;
    o.tool = ToolId::Lighten;
    o.brushSize = 3;
    o.shadeAmount = 128;

    shadeBrush(c, 2, 2, o, true);
    CHECK(c.image().at(2, 2).r > 100);
    // 空气像素不能被"加光"出来
    CHECK(c.image().at(1, 1).a == 0);

    const uint8_t bright = c.image().at(2, 2).r;

    ToolOptions od = o;
    od.tool = ToolId::Darken;
    shadeBrush(c, 2, 2, od, false);
    CHECK(c.image().at(2, 2).r < bright);
}

void testToolFillViaSession() {
    section("工具 / 油漆桶（经会话）");

    Canvas c = makeCanvas(20, 20);
    c.rectOutline(2, 2, 12, 12, kWhite, Material{});

    ToolOptions o;
    o.tool = ToolId::Fill;
    o.color = kGreen;
    ToolSession s;
    s.begin(c, o, 7, 7);
    s.end();
    CHECK(s.modified());
    CHECK(c.image().at(7, 7) == kGreen);
    // 边框颜色未变
    CHECK(c.image().at(2, 2) == kWhite);
}

void testToolHistoryIntegration() {
    section("工具 + 撤销重做 / 端到端");

    Canvas c = makeCanvas(32, 32);
    Canvas original = c;
    History h;
    ToolOptions o;
    o.tool = ToolId::Pencil;
    o.color = kWhite;
    o.brushSize = 3;

    ToolSession s;
    h.beginStroke(c);
    s.begin(c, o, 5, 5);
    s.update(c, o, 15, 15);
    s.end();
    CHECK(h.endStroke(c, "铅笔笔画"));
    CHECK(!sameCanvas(c, original));

    const Canvas drawn = c;
    CHECK(h.undo(c));
    CHECK(sameCanvas(c, original));
    CHECK(h.redo(c));
    CHECK(sameCanvas(c, drawn));
}

// ---------------------------------------------------------------------------
// 测试：调色板
// ---------------------------------------------------------------------------

void testPalette() {
    section("调色板");

    CHECK(builtinPaletteCount() >= 3);

    Palette p;
    CHECK(std::string(p.presetName()) == "PICO-8");
    CHECK(p.size() == 16);
    CHECK(p.currentIndex() == 0);
    // 索引 0 是黑色
    CHECK(p.current().r == 0 && p.current().g == 0 && p.current().b == 0);
    CHECK(p.current().a == 255);

    // 索引 8 是 PICO-8 的红色 FF004D
    p.setCurrent(8);
    CHECK(p.current().r == 0xFF && p.current().g == 0x00 && p.current().b == 0x4D);

    // 夹取
    p.setCurrent(999);
    CHECK(p.currentIndex() == 15);
    p.setCurrent(-5);
    CHECK(p.currentIndex() == 0);

    // 循环切换，含负数
    p.setCurrent(0);
    p.step(-1);
    CHECK(p.currentIndex() == 15);
    p.step(1);
    CHECK(p.currentIndex() == 0);

    // 切换预设
    p.loadPreset(1);
    CHECK(std::string(p.presetName()) == "DB16");
    CHECK(p.size() == 16);
    CHECK(p.currentIndex() == 0);

    p.loadPreset(2);
    CHECK(std::string(p.presetName()) == "Game Boy");
    CHECK(p.size() == 4);

    // 越界预设被夹取
    p.loadPreset(999);
    CHECK(p.presetIndex() == builtinPaletteCount() - 1);

    // 自定义颜色
    Palette q;
    const RGBA8 custom{1, 2, 3, 255};
    const int idx = q.addColor(custom);
    CHECK(idx == 16);
    CHECK(q.size() == 17);
    CHECK(q.currentIndex() == 16);
    CHECK(q.current() == custom);
    CHECK(q.contains(custom));
    CHECK(!q.contains(RGBA8{9, 9, 9, 255}));
}

void testMaterial() {
    section("材质通道");

    Material m;
    CHECK(m.emissionLevel() == 0);
    CHECK(!m.emissive());
    CHECK(m.ior == 0);
    CHECK(!m.reflective());
    CHECK(!m.solid());

    m.setIorValue(1.5f);
    CHECK(m.ior == 50);
    CHECK(m.iorValue() > 1.49f && m.iorValue() < 1.51f);

    m.setIorValue(0.5f);   // 夹到 1.0
    CHECK(m.iorValue() > 0.99f && m.iorValue() < 1.01f);

    m.setIorValue(99.0f);  // 夹到 3.55
    CHECK(m.iorValue() > 3.54f && m.iorValue() < 3.56f);

    m.setReflective(true);
    CHECK(m.reflective());
    m.setSolid(true);
    CHECK(m.solid());
    m.setReflective(false);
    CHECK(!m.reflective());
    CHECK(m.solid());      // 清一个标志不该影响另一个

    // 材质参与撤销重做的比对。
    // 自发光故意设成 (10,200,30)，与 albedo（白色）**完全不同** ——
    // 用来证明"表面颜色"与"发光颜色"是两个独立的量。
    Canvas c = makeCanvas(8, 8);
    History h;
    h.beginStroke(c);
    Material glow;
    glow.setEmissionFrom(10, 200, 30);
    c.setPixel(1, 1, kWhite, glow);
    CHECK(h.endStroke(c, "自发光"));
    CHECK(c.materialAt(1, 1).emG == 200);
    CHECK(c.materialAt(1, 1).emR == 10);
    CHECK(c.image().at(1, 1) == kWhite);   // albedo 不受自发光影响

    CHECK(h.undo(c));
    CHECK(c.materialAt(1, 1).emissionLevel() == 0);   // 回到笔画前：空气
    CHECK(c.image().at(1, 1).a == 0);

    CHECK(h.redo(c));
    CHECK(c.materialAt(1, 1).emG == 200);  // 重做后恢复
    CHECK(c.image().at(1, 1) == kWhite);
}

void testHistoryExternalChange() {
    section("撤销重做 / 越带改动后重做链作废");

    Canvas c = makeCanvas(16, 16);
    History h;
    h.beginStroke(c);
    c.setPixel(5, 5, kRed, Material{});
    CHECK(h.endStroke(c, "点"));

    CHECK(h.undo(c));
    CHECK(h.canRedo());

    // 外部直接改了画布（相当于载入文件），此时重做已不可信
    c.setPixel(9, 9, kGreen, Material{});
    h.notifyExternalChange();
    CHECK(!h.canRedo());

    // 撤销栈不受影响
    CHECK(!h.canUndo());
}

void testCanvasDirtyRect() {
    section("画布脏矩形");

    Canvas c = makeCanvas(64, 64);
    c.clearDirty();
    CHECK(!c.hasDirty());

    c.setPixel(10, 20, kWhite, Material{});
    CHECK(c.hasDirty());
    CHECK(c.dirty().x0 == 10 && c.dirty().y0 == 20);
    CHECK(c.dirty().x1 == 10 && c.dirty().y1 == 20);

    c.setPixel(30, 5, kWhite, Material{});
    CHECK(c.dirty().x0 == 10 && c.dirty().y0 == 5);
    CHECK(c.dirty().x1 == 30 && c.dirty().y1 == 20);

    const Rect taken = c.takeDirty();
    CHECK(taken.x0 == 10 && taken.y1 == 20);
    CHECK(!c.hasDirty());   // 取出后清空

    // 越界写入不应产生脏矩形
    CHECK(!c.setPixel(-1, 0, kWhite, Material{}));
    CHECK(!c.setPixel(64, 0, kWhite, Material{}));
    CHECK(!c.hasDirty());
}

void testResize() {
    section("画布尺寸变更");

    Canvas c = makeCanvas(16, 16);
    c.fill(kWhite, Material{});
    c.resize(32, 24);
    CHECK(c.width() == 32 && c.height() == 24);
    CHECK(c.pixelCount() == 32 * 24);
    CHECK(countSet(c) == 0);            // 内容被清空
    CHECK(c.dirty().x1 == 31 && c.dirty().y1 == 23);   // 全图变脏
    CHECK(c.materialData() != nullptr);
}

}  // namespace

int main() {
    setupConsoleUtf8();
    printf("=== core 层 headless 测试 ===\n");

    testLines();
    testRect();
    testEllipse();
    testFloodFill();
    testHistoryBasics();
    testHistoryNoChange();
    testHistoryPatchSize();
    testHistoryStack();
    testHistoryMaxSteps();
    testToolPencil();
    testToolBrushShape();
    testToolShapePreview();
    testToolEraserAndPicker();
    testToolDither();
    testToolShade();
    testToolFillViaSession();
    testToolHistoryIntegration();
    testPalette();
    testMaterial();
    testHistoryExternalChange();
    testCanvasDirtyRect();
    testResize();

    printf("\n========================================\n");
    printf("通过 %d，失败 %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("全部通过。\n");
        return 0;
    }
    printf("有失败项。\n");
    return 1;
}
