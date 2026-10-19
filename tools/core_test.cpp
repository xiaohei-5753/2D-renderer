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
#include "core/composite.h"
#include "core/console.h"
#include "core/history.h"
#include "core/layer.h"
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

int countSet(const Layer& c) {
    int n = 0;
    const Image& img = c.image();
    for (size_t i = 0; i < img.pixelCount(); i++)
        if (img.data()[i].a > 0) n++;
    return n;
}

int countSetInRow(const Layer& c, int y) {
    int n = 0;
    for (int x = 0; x < c.width(); x++)
        if (c.image().at(x, y).a > 0) n++;
    return n;
}

int countSetInCol(const Layer& c, int x) {
    int n = 0;
    for (int y = 0; y < c.height(); y++)
        if (c.image().at(x, y).a > 0) n++;
    return n;
}

bool samePixel(const Layer& a, const Layer& b, int x, int y) {
    return a.image().at(x, y) == b.image().at(x, y) &&
           a.materialAt(x, y) == b.materialAt(x, y);
}

bool sameCanvas(const Layer& a, const Layer& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            if (!samePixel(a, b, x, y)) return false;
    return true;
}

Layer makeCanvas(int w, int h) {
    Layer c; c.resize(w, h);
    c.clear(kAir, Material{});
    return c;
}

// ---------------------------------------------------------------------------
// 测试：绘制原语
// ---------------------------------------------------------------------------

void testLines() {
    section("绘制原语 / 直线（对称 Bresenham 的方向约定）");

    {
        Layer c = makeCanvas(32, 32);
        c.drawLine(2, 5, 12, 5, kWhite);
        CHECK(countSet(c) == 11);            // 2..12 含端点共 11 个
        CHECK(countSetInRow(c, 5) == 11);
        CHECK(countSetInCol(c, 2) == 1);
        CHECK(countSetInCol(c, 12) == 1);
    }
    {
        Layer c = makeCanvas(32, 32);
        c.drawLine(7, 1, 7, 9, kWhite);
        CHECK(countSet(c) == 9);
        CHECK(countSetInCol(c, 7) == 9);
    }
    {
        Layer c = makeCanvas(32, 32);
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
        Layer a = makeCanvas(32, 32);
        Layer b = makeCanvas(32, 32);
        a.drawLine(3, 4, 25, 17, kWhite);
        b.drawLine(25, 17, 3, 4, kWhite);
        CHECK(sameCanvas(a, b));
    }
    {
        // 越界坐标不应该写出画布外，也不应该崩溃
        Layer c = makeCanvas(16, 16);
        c.drawLine(-20, -20, 40, 40, kWhite);
        CHECK(countSet(c) == 16);   // 对角线恰好穿过 16 行
    }
}

void testRect() {
    section("绘制原语 / 矩形");

    {
        Layer c = makeCanvas(16, 16);
        c.rectOutline(1, 1, 5, 5, kWhite);
        CHECK(countSet(c) == 16);   // 5x5 周长 = 5*4-4
    }
    {
        Layer c = makeCanvas(16, 16);
        c.fillRect(2, 2, 4, 4, kWhite);
        CHECK(countSet(c) == 9);    // 3x3
    }
    {
        // 坐标反着传也要正确（内部会 swap）
        Layer a = makeCanvas(16, 16);
        Layer b = makeCanvas(16, 16);
        a.fillRect(2, 2, 6, 8, kWhite);
        b.fillRect(6, 8, 2, 2, kWhite);
        CHECK(sameCanvas(a, b));
    }
    {
        // 超出边界的矩形要被裁掉而不是越界写
        Layer c = makeCanvas(16, 16);
        c.fillRect(-5, -5, 100, 100, kWhite);
        CHECK(countSet(c) == 16 * 16);
    }
}

void testEllipse() {
    section("绘制原语 / 椭圆");

    {
        // 实心圆的面积应接近 pi*r^2
        Layer c = makeCanvas(32, 32);
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
        Layer c = makeCanvas(32, 32);
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
        Layer c = makeCanvas(32, 8);
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
        Layer c = makeCanvas(8, 8);
        c.drawEllipse(4, 4, 4, 4, false, kWhite);
        CHECK(countSet(c) == 1);
    }
}

void testFloodFill() {
    section("绘制原语 / 洪水填充");

    {
        Layer c = makeCanvas(20, 20);
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
        Layer c = makeCanvas(8, 8);
        c.fill(kRed, Material{});
        CHECK(c.floodFill(4, 4, kRed) == 0);
    }
    {
        // 越界起点不应崩溃
        Layer c = makeCanvas(8, 8);
        CHECK(c.floodFill(-1, 4, kRed) == 0);
        CHECK(c.floodFill(100, 4, kRed) == 0);
    }
}

// ---------------------------------------------------------------------------
// 测试：撤销 / 重做
// ---------------------------------------------------------------------------

void testHistoryBasics() {
    section("撤销重做 / 基本正确性");

    Document doc(32, 32);
    Layer& c = doc.active();
    const Layer original = c;

    History h;
    h.beginStroke(doc);
    c.drawLine(1, 1, 20, 20, kWhite);
    CHECK(h.commit(doc, "直线"));
    const Layer afterStroke = c;

    CHECK(h.canUndo());
    CHECK(!h.canRedo());
    CHECK(h.undoSteps() == 1);

    CHECK(h.undo(doc));
    CHECK(sameCanvas(doc.active(), original));   // 逐像素 + 材质完全还原
    CHECK(h.canRedo());
    CHECK(h.redoSteps() == 1);

    CHECK(h.redo(doc));
    CHECK(sameCanvas(doc.active(), afterStroke));
    CHECK(h.undoSteps() == 1);
    CHECK(!h.canRedo());
}

void testHistoryNoChange() {
    section("撤销重做 / 无变化不污染栈");

    Document doc(16, 16);
    Layer& c = doc.active();
    History h;

    h.beginStroke(doc);
    CHECK(!h.commit(doc, "空操作"));   // 没改任何像素
    CHECK(!h.canUndo());
    CHECK(h.undoSteps() == 0);

    // 写回原值也应该算作"无变化"
    h.beginStroke(doc);
    c.setPixel(3, 3, kAir, Material{});
    CHECK(!h.commit(doc, "写回原值"));
    CHECK(!h.canUndo());
}

void testHistoryPatchSize() {
    section("撤销重做 / 内存占用是有界的（补丁不是无限增长）");

    // ⚠️ 这里要说清楚现在的真实语义，别让断言误导后来的人。
    //
    // 老实现的补丁是"变化像素的外接矩形"，所以一笔单像素只占 1 个像素。
    // 改成图层之后不行了：撤销与重做都要**完全对称**（重做必须能精确还原，
    // 而不是在撤销的那一刻从画布现采状态 —— 那条路一旦有绕过 History 的改动就会采坏），
    // 于是每个步骤要同时存"改前"和"改后"两份。
    // 而要判断"到底哪些像素变了"，就得先把整层抓下来做差。
    //
    // 结论：现在每次提交存的是**两份整层快照**，与改动多少像素无关。
    // 代价可以用一次算清（见下面的断言），上限仍然有界：
    //   一份快照 = 像素数 × (sizeof(RGBA8) + sizeof(Material))
    //   一步     = 2 份
    //   画布 320×180、128 步 ≈ 2 × 57KB × 128 ≈ 14.6 MB
    // 也就是说对像素画的常见尺寸完全够用，而且 History::maxBytes 会兜底裁剪。
    //
    // 如果哪天真的需要省内存，正确的方向是存"变化外接矩形内的两份"：
    // 先用整层快照做差求出矩形，再把两份都快照裁剪到那个矩形。那需要让 Blob
    // 自己知道怎么裁剪 —— 这次没有做，因为先要保证语义正确。
    const size_t perPixel = sizeof(RGBA8) + sizeof(Material);
    const size_t bytesPerStep = 2 * (size_t)256 * 256 * perPixel;

    Document doc(256, 256);
    Layer& c = doc.active();
    History h;
    h.beginStroke(doc);
    c.setPixel(100, 100, kWhite, Material{});
    CHECK(h.commit(doc, "单像素"));
    CHECK(h.bytes() == bytesPerStep);

    // 第二次提交的增量同样是固定的 2 份，与画的是 1 个像素还是 11 个像素无关
    h.beginStroke(doc);
    c.drawLine(10, 10, 20, 10, kWhite, Material{});
    CHECK(h.commit(doc, "短线"));
    CHECK(h.bytes() == 2 * bytesPerStep);

    // 没改动的提交不占任何内存
    h.beginStroke(doc);
    CHECK(!h.commit(doc, "空操作"));
    CHECK(h.bytes() == 2 * bytesPerStep);
}

void testHistoryStack() {
    section("撤销重做 / 多步与分支");

    Document doc(32, 32);
    Layer& c = doc.active();
    const Layer original = c;
    History h;

    // 5 笔，每笔一个不同位置的像素
    for (int i = 0; i < 5; i++) {
        h.beginStroke(doc);
        c.setPixel(i, 0, kWhite, Material{});
        CHECK(h.commit(doc, "点"));
    }
    CHECK(h.undoSteps() == 5);

    // 全部撤销
    for (int i = 0; i < 5; i++) CHECK(h.undo(doc));
    CHECK(sameCanvas(doc.active(), original));
    CHECK(!h.canUndo());
    CHECK(h.redoSteps() == 5);

    // 全部重做
    for (int i = 0; i < 5; i++) CHECK(h.redo(doc));
    CHECK(!h.canRedo());
    for (int i = 0; i < 5; i++) CHECK(c.image().at(i, 0) == kWhite);

    // 撤销两步后做新操作 -> 重做链必须失效
    CHECK(h.undo(doc));
    CHECK(h.undo(doc));
    CHECK(h.redoSteps() == 2);
    h.beginStroke(doc);
    c.setPixel(20, 20, kGreen, Material{});
    CHECK(h.commit(doc, "新分支"));
    CHECK(!h.canRedo());
    CHECK(h.redoSteps() == 0);
    // 新分支保留，旧的前 3 步仍在撤销栈里
    CHECK(h.undoSteps() == 4);
}

void testHistoryMaxSteps() {
    section("撤销重做 / 步数上限裁剪");

    Document doc(16, 16);
    Layer& c = doc.active();
    History h;
    h.maxSteps = 8;

    for (int i = 0; i < 20; i++) {
        h.beginStroke(doc);
        c.setPixel(i % 16, i / 16, kWhite, Material{});
        h.commit(doc, "点");
    }
    CHECK(h.undoSteps() == 8);
    // 还能正常撤销 8 次
    int undone = 0;
    while (h.undo(doc)) undone++;
    CHECK(undone == 8);
}

// ---------------------------------------------------------------------------
// 测试：工具
// ---------------------------------------------------------------------------

void testToolPencil() {
    section("工具 / 铅笔连续性（快速拖动不留断点）");

    Layer c = makeCanvas(32, 32);
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
        Layer c = makeCanvas(32, 32);
        ToolOptions o;
        o.tool = ToolId::Pencil;
        o.color = kWhite;
        o.brushSize = 5;
        o.brushRound = false;
        stampBrush(c, 10, 10, o, o.color, Material{});
        CHECK(countSet(c) == 25);   // 5x5 方
    }
    {
        Layer c = makeCanvas(32, 32);
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
        Layer c = makeCanvas(16, 16);
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

    Layer c = makeCanvas(32, 32);
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

    Layer c = makeCanvas(16, 16);
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
    Layer c = makeCanvas(16, 16);
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

    Layer c = makeCanvas(8, 8);
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

    Layer c = makeCanvas(20, 20);
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

    Document doc(32, 32);
    Layer& c = doc.active();
    const Layer original = c;
    History h;
    ToolOptions o;
    o.tool = ToolId::Pencil;
    o.color = kWhite;
    o.brushSize = 3;

    ToolSession s;
    h.beginStroke(doc);
    s.begin(c, o, 5, 5);
    s.update(c, o, 15, 15);
    s.end();
    CHECK(h.commit(doc, "铅笔笔画"));
    CHECK(!sameCanvas(doc.active(), original));

    const Layer drawn = c;
    CHECK(h.undo(doc));
    CHECK(sameCanvas(doc.active(), original));
    CHECK(h.redo(doc));
    CHECK(sameCanvas(doc.active(), drawn));
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

    // ⚠️ 加入色板后，这个新条目必须带上一份**自己的材质**，而不是落进
    // "mats_ 比 colors_ 短"的空隙里退回默认值。漏掉的话选色会拿到白色、
    // 自发光会被清零（选色 = 同时选材质，这里曾经断链）。
    CHECK(q.mat(idx).r == 1);
    CHECK(q.mat(idx).g == 2);
    CHECK(q.mat(idx).b == 3);
    CHECK(q.mat(idx).a == 255);        // 色板条目一律不透明
    CHECK(q.mat(idx).eR == 0);         // 新条目默认不发光
    CHECK(q.mat(idx).eG == 0);
    CHECK(q.mat(idx).eB == 0);

    // 再验证一次"平行数组没被写坏"：追加第二个自定义色，两个条目各归各的
    const RGBA8 custom2{200, 100, 50, 255};
    const int idx2 = q.addColor(custom2);
    CHECK(idx2 == 17);
    CHECK(q.mat(idx2).r == 200 && q.mat(idx2).g == 100 && q.mat(idx2).b == 50);
    CHECK(q.mat(idx).r == 1);          // 前一个条目没被覆盖

    // ------------------------------------------------------------------
    // 删除色板项必须**同时**删除材质表里对应那一项
    //
    // mats_ 与 colors_ 是平行数组：mat(i) 的 r/g/b 取自颜色表、eR/eG/eB/a
    // 取自材质表。只删颜色表的话，从被删位置起每一项的自发光/透明度都会
    // 变成**前一项**的 —— 表现为"删一个色块，后面所有色块的发光全错位"。
    // 这里给三个条目配三份可区分的材质，删中间那个，逐个核对。
    // ------------------------------------------------------------------
    {
        Palette p2;
        p2.loadPreset(0);
        p2.addColor(RGBA8{10, 11, 12, 255});     // 16
        p2.addColor(RGBA8{20, 21, 22, 255});     // 17
        p2.addColor(RGBA8{30, 31, 32, 255});     // 18

        Palette::Mat7 m16 = p2.mat(16);
        m16.eR = 16; m16.a = 200;
        p2.setMat(16, m16);
        Palette::Mat7 m17 = p2.mat(17);
        m17.eG = 17; m17.a = 150;
        p2.setMat(17, m17);
        Palette::Mat7 m18 = p2.mat(18);
        m18.eB = 18; m18.a = 100;
        p2.setMat(18, m18);

        CHECK(p2.removeColor(17));
        CHECK(p2.size() == 18);

        // 前一项不动
        CHECK(p2.mat(16).eR == 16 && p2.mat(16).eG == 0);
        CHECK(p2.mat(16).a == 200);
        // 原来的 18 挪到 17，材质必须**跟着颜色一起**挪过来
        CHECK(p2.mat(17).r == 30 && p2.mat(17).g == 31 && p2.mat(17).b == 32);
        CHECK(p2.mat(17).eB == 18);
        CHECK(p2.mat(17).eG == 0);        // 不能残留被删项的 eG=17
        CHECK(p2.mat(17).a == 100);       // 不能残留被删项的 a=150
        CHECK(p2.currentIndex() <= 17);
    }
}

void testMaterial() {
    section("材质通道");

    Material m;
    CHECK(m.emissionLevel() == 0);
    CHECK(!m.emissive());
    CHECK(m.ior == 0);

    m.setIorValue(1.5f);
    CHECK(m.ior == 50);
    CHECK(m.iorValue() > 1.49f && m.iorValue() < 1.51f);

    m.setIorValue(0.5f);   // 夹到 1.0
    CHECK(m.iorValue() > 0.99f && m.iorValue() < 1.01f);

    m.setIorValue(99.0f);  // 夹到 3.55
    CHECK(m.iorValue() > 3.54f && m.iorValue() < 3.56f);

    // 材质参与撤销重做的比对。
    // 自发光故意设成 (10,200,30)，与 albedo（白色）**完全不同** ——
    // 用来证明"表面颜色"与"发光颜色"是两个独立的量。
    Document doc(8, 8);
    Layer& c = doc.active();
    History h;
    h.beginStroke(doc);
    Material glow;
    glow.setEmissionFrom(10, 200, 30);
    c.setPixel(1, 1, kWhite, glow);
    CHECK(h.commit(doc, "自发光"));
    CHECK(c.materialAt(1, 1).emG == 200);
    CHECK(c.materialAt(1, 1).emR == 10);
    CHECK(c.image().at(1, 1) == kWhite);   // albedo 不受自发光影响

    CHECK(h.undo(doc));
    CHECK(c.materialAt(1, 1).emissionLevel() == 0);   // 回到笔画前：空气
    CHECK(c.image().at(1, 1).a == 0);

    CHECK(h.redo(doc));
    CHECK(c.materialAt(1, 1).emG == 200);  // 重做后恢复
    CHECK(c.image().at(1, 1) == kWhite);

    // ------------------------------------------------------------------
    // setEmissionScaled：自发光 = 表面颜色 × 强度
    //
    // 这是材质预设的语义（"霓虹管是蓝的，它发的光就是蓝的"）。
    // 重点是**保色相**：曾经把三通道都取成同一个值（如都用最大通道），
    // 琥珀色会被压成灰色，彩色霓虹全部消失。
    // ------------------------------------------------------------------
    {
        Material e;
        // 琥珀色 (255,200,0)，强度 170（霓虹管那一档）
        e.setEmissionScaled(255, 200, 0, 170);
        CHECK(e.emR == 170);
        CHECK(e.emG == 133);                    // 200*170/255
        CHECK(e.emB == 0);
        // 色相必须保住：R > G > B，而不是三者相等（那才是被压成灰）
        CHECK(e.emR > e.emG);
        CHECK(e.emG > e.emB);

        // 强度 0 = 不发光（空气/半透明/彩色玻璃/实心墙都靠这一条，
        // 不需要调用方加分支）
        e.setEmissionScaled(255, 200, 0, 0);
        CHECK(!e.emissive());
        CHECK(e.emissionLevel() == 0);

        // 强度 255 = 完全等于表面色（"跟随表面颜色"）
        e.setEmissionScaled(41, 173, 255, 255);
        CHECK(e.emR == 41 && e.emG == 173 && e.emB == 255);

        // 黑色表面无论强度多大都不发光
        e.setEmissionScaled(0, 0, 0, 255);
        CHECK(!e.emissive());
    }
}

void testHistoryExternalChange() {
    section("撤销重做 / 越带改动后重做链作废");

    Document doc(16, 16);
    Layer& c = doc.active();
    History h;
    h.beginStroke(doc);
    c.setPixel(5, 5, kRed, Material{});
    CHECK(h.commit(doc, "点"));

    CHECK(h.undo(doc));
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

    Layer c = makeCanvas(64, 64);
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

    Layer c = makeCanvas(16, 16);
    c.fill(kWhite, Material{});
    c.resize(32, 24);
    CHECK(c.width() == 32 && c.height() == 24);
    CHECK(c.pixelCount() == 32 * 24);
    CHECK(countSet(c) == 0);            // 内容被清空
    CHECK(c.dirty().x1 == 31 && c.dirty().y1 == 23);   // 全图变脏
    CHECK(c.materialData() != nullptr);
}

// ---------------------------------------------------------------------------
// 测试：多图层文档
// ---------------------------------------------------------------------------

void testDocument() {
    section("多图层 / 文档与图层操作");

    Document doc(16, 16);
    CHECK(doc.layerCount() == 1);
    CHECK(doc.activeIndex() == 0);
    CHECK(doc.width() == 16 && doc.height() == 16);
    CHECK(doc.layer(0).pixelCount() == 16 * 16);
    CHECK(doc.layer(0).material.size() == 16 * 16);

    // 新建层插在**当前层之上**并选中它
    const int a = doc.addLayer("测试层");
    CHECK(doc.layerCount() == 2);
    CHECK(a == 1);
    CHECK(doc.activeIndex() == 1);
    CHECK(doc.layer(1).name == "测试层");

    // 复制
    doc.setActiveIndex(0);
    const int d = doc.duplicateLayer(0);
    CHECK(doc.layerCount() == 3);
    CHECK(d == 1);
    CHECK(doc.layer(1).name.find("副本") != std::string::npos);

    // 不允许删空
    CHECK(doc.removeLayer(0));
    CHECK(doc.removeLayer(0));
    CHECK(doc.layerCount() == 1);
    CHECK(!doc.removeLayer(0));      // 只剩一层时拒绝

    // 上下移动
    doc.addLayer("A");
    doc.addLayer("B");
    CHECK(doc.layerCount() == 3);
    const std::string top = doc.layer(2).name;
    CHECK(doc.moveLayer(2, 0));
    CHECK(doc.layer(0).name == top);
    CHECK(doc.activeIndex() == 0);
    CHECK(!doc.moveLayer(1, 1));     // 原地移动是空操作

    // 越界索引被夹取，不应崩
    doc.setActiveIndex(999);
    CHECK(doc.activeIndex() == doc.layerCount() - 1);
    doc.setActiveIndex(-5);
    CHECK(doc.activeIndex() == 0);

    // 改尺寸会重置成一个空层
    doc.resize(8, 8);
    CHECK(doc.layerCount() == 1);
    CHECK(doc.width() == 8 && doc.height() == 8);
}

// 结构版本号：**层的排列变了**必须能被光照侧看到
//
// 踩过的 bug：逐层光照的缓存键只有"每层的内容版本 + 可见性 + 不透明度"，
// 不含图层身份。交换两层位置时，每个位置上的这三个量都没变，于是光照判定
// "什么都没变"直接早退 —— 画面停在上一次的图层顺序上，直到用户再画一笔。
// 所以 Document 维护一个"排列版本号"，排列一变它必变。
void testDocumentStructureVersion() {
    section("多图层 / 结构版本号（排列变化必须可见）");

    Document doc(8, 8);
    const uint32_t v0 = doc.structureVersion();
    CHECK(v0 == doc.structureVersion());   // 只读不写：不该变

    doc.addLayer("A");
    const uint32_t v1 = doc.structureVersion();
    CHECK(v1 != v0);

    doc.addLayer("B");
    const uint32_t v2 = doc.structureVersion();
    CHECK(v2 != v1);

    // 移动：层数不变、每层内容与元数据都不变 —— 只有结构号能反映这件事
    CHECK(doc.moveLayer(2, 0));
    const uint32_t v3 = doc.structureVersion();
    CHECK(v3 != v2);

    // 原地移动是空操作：不该让光照白算一遍
    CHECK(!doc.moveLayer(1, 1));
    CHECK(doc.structureVersion() == v3);

    // 只剩一层时拒绝删除，也不该改版本号
    CHECK(doc.removeLayer(0));
    CHECK(doc.removeLayer(0));
    const uint32_t v4 = doc.structureVersion();
    CHECK(!doc.removeLayer(0));
    CHECK(doc.structureVersion() == v4);

    // 复制 / 合并 / 改尺寸都要变
    doc.addLayer("C");
    const uint32_t v5 = doc.structureVersion();
    doc.duplicateLayer(0);
    const uint32_t v6 = doc.structureVersion();
    CHECK(v6 != v5);
    CHECK(doc.mergeDown(1));
    const uint32_t v7 = doc.structureVersion();
    CHECK(v7 != v6);
    doc.resize(4, 4);
    CHECK(doc.structureVersion() != v7);
}

void testLayerMerge() {
    section("多图层 / 合并向下（source-over）");

    Document doc(4, 4);
    // 底层：不透明的红
    doc.layer(0).albedo.fill(RGBA8{200, 0, 0, 255});
    doc.layer(0).material.assign(16, Material{});

    // 上层：除了一个像素外全透明
    const int up = doc.addLayer("上");
    doc.layer(up).albedo.fill(RGBA8{0, 0, 0, 0});
    doc.layer(up).material.assign(16, Material{});
    Material glow;
    glow.setEmissionFrom(0, 200, 0);
    doc.layer(up).albedo.at(1, 1) = RGBA8{0, 0, 200, 128};
    doc.layer(up).materialAt(1, 1) = glow;

    CHECK(doc.mergeDown(up));
    CHECK(doc.layerCount() == 1);

    // 未被覆盖处保持底层的红
    CHECK(doc.layer(0).albedo.at(0, 0) == (RGBA8{200, 0, 0, 255}));
    // 半透明蓝叠在不透明红上 -> 结果不透明，且红蓝都参与了混合
    const RGBA8 m = doc.layer(0).albedo.at(1, 1);
    CHECK(m.a == 255);
    CHECK(m.r > 0 && m.b > 0);
    // 材质采用上层的（"最上层有覆盖的像素，其材质胜出"）
    CHECK(doc.layer(0).materialAt(1, 1).emG == 200);
    // 未被覆盖处的材质保持底层的（自发光为 0）
    CHECK(doc.layer(0).materialAt(0, 0).emissionLevel() == 0);

    // 层数不足时拒绝
    CHECK(!doc.mergeDown(0));
    CHECK(!doc.mergeDown(5));
}

// ---------------------------------------------------------------------------
// 测试：图层合成与层间光照通道
// ---------------------------------------------------------------------------

void testFlatten() {
    section("多图层 / 压平合成");

    Document doc(4, 4);
    doc.layer(0).albedo.fill(RGBA8{200, 0, 0, 255});      // 底：不透明红
    doc.layer(0).material.assign(16, Material{});
    const int up = doc.addLayer("上");
    doc.layer(up).albedo.fill(RGBA8{0, 0, 0, 0});         // 上：只一个半透明蓝像素
    doc.layer(up).material.assign(16, Material{});
    Material glow;
    glow.setEmissionFrom(0, 200, 0);
    doc.layer(up).albedo.at(2, 2) = RGBA8{0, 0, 200, 128};
    doc.layer(up).materialAt(2, 2) = glow;

    Image flat(4, 4);
    std::vector<Material> mats(16);
    flattenDocument(doc, flat, mats, Rect{0, 0, 3, 3});

    CHECK(flat.at(0, 0) == (RGBA8{200, 0, 0, 255}));      // 未被覆盖：底层红
    const RGBA8 m = flat.at(2, 2);
    CHECK(m.a == 255);                                    // 半透明叠不透明 -> 不透明
    CHECK(m.r > 0 && m.b > 0);                            // 红蓝都参与了混合
    CHECK(mats[2 * 4 + 2].emG == 200);                    // 材质取最上层有覆盖者
    CHECK(mats[0].emissionLevel() == 0);                  // 未覆盖处材质来自底层

    // 隐藏图层不参与合成
    doc.layer(up).visible = false;
    flattenDocument(doc, flat, mats, Rect{0, 0, 3, 3});
    CHECK(flat.at(2, 2) == (RGBA8{200, 0, 0, 255}));
    CHECK(mats[2 * 4 + 2].emissionLevel() == 0);

    // 整层不透明度 0 也不参与
    doc.layer(up).visible = true;
    doc.layer(up).opacity = 0;
    flattenDocument(doc, flat, mats, Rect{0, 0, 3, 3});
    CHECK(flat.at(2, 2) == (RGBA8{200, 0, 0, 255}));
}

// ---------------------------------------------------------------------------
// 测试：图层化的撤销重做
// ---------------------------------------------------------------------------

// 判断文档里"除第 skip 层外"的全部内容是否与快照一致
bool otherLayersMatch(const Document& doc, const std::vector<Layer>& snapshot, int skip) {
    for (int i = 0; i < doc.layerCount(); i++) {
        if (i == skip) continue;
        if (i >= (int)snapshot.size()) return false;
        if (!sameCanvas(doc.layer(i), snapshot[(size_t)i])) return false;
    }
    return true;
}

void testHistoryLayers() {
    section("撤销重做 / 笔画只影响当前图层");

    Document doc(8, 8);
    doc.active().clear(kAir, Material{});
    const int up = doc.addLayer("上");
    doc.setActiveIndex(up);
    doc.active().clear(kAir, Material{});
    CHECK(doc.layerCount() == 2);

    std::vector<Layer> before(doc.layers().begin(), doc.layers().end());

    // 在上层画一条线
    History h;
    h.beginStroke(doc);
    doc.active().drawLine(0, 0, 7, 7, kWhite, Material{});
    CHECK(h.commit(doc, "线"));
    CHECK(doc.active().image().at(3, 3) == kWhite);

    CHECK(h.undo(doc));
    CHECK(doc.layer(1).image().at(3, 3).a == 0);          // 上层被还原
    CHECK(otherLayersMatch(doc, before, 1));              // 下层一根汗毛都没动
    CHECK(doc.activeIndex() == 1);                        // 当前图层不变

    CHECK(h.redo(doc));
    CHECK(doc.active().image().at(3, 3) == kWhite);
    CHECK(otherLayersMatch(doc, before, 1));
}

void testHistoryStructural() {
    section("撤销重做 / 结构性操作（新建 · 删除 · 移动 · 合并）");

    Document doc(4, 4);
    doc.layer(0).albedo.fill(RGBA8{200, 0, 0, 255});
    doc.layer(0).material.assign(16, Material{});
    History h;

    // --- 新建图层：撤销要把它去掉 ---
    h.beginAll(doc);
    const int up = doc.addLayer("上");
    doc.layer(up).albedo.at(1, 1) = RGBA8{0, 0, 255, 255};
    CHECK(h.commit(doc, "新建"));
    CHECK(doc.layerCount() == 2);

    CHECK(h.undo(doc));
    CHECK(doc.layerCount() == 1);                     // 结构回到 1 层
    CHECK(doc.layer(0).albedo.at(1, 1) == (RGBA8{200, 0, 0, 255}));
    CHECK(h.redo(doc));
    CHECK(doc.layerCount() == 2);

    // --- 删除图层：名字与元数据都要能回来 ---
    doc.layer(1).name = "有名有姓";
    doc.layer(1).opacity = 77;
    h.beginAll(doc);
    CHECK(doc.removeLayer(1));
    CHECK(h.commit(doc, "删除"));
    CHECK(doc.layerCount() == 1);

    CHECK(h.undo(doc));
    CHECK(doc.layerCount() == 2);
    CHECK(doc.layer(1).name == "有名有姓");           // 元数据也回来了
    CHECK(doc.layer(1).opacity == 77);
    CHECK(doc.layer(1).albedo.at(1, 1) == (RGBA8{0, 0, 255, 255}));

    // --- 移动图层 ---
    History h2;
    h2.beginAll(doc);
    CHECK(doc.moveLayer(1, 0));
    CHECK(h2.commit(doc, "移动"));
    CHECK(doc.layer(0).name == "有名有姓");
    CHECK(h2.undo(doc));
    CHECK(doc.layer(1).name == "有名有姓");

    // 合并前把上层的不透明度调回 255：这一段要验证的是"合并结果的混合比例"，
    // 不透明度对混合的影响已经在别处覆盖了，混在一起会让断言难以阅读。
    doc.layer(1).opacity = 255;

    // --- 合并向下 ---
    History h3;
    h3.beginAll(doc);
    CHECK(doc.mergeDown(1));
    CHECK(h3.commit(doc, "合并"));
    CHECK(doc.layerCount() == 1);
    // 上层不透明的蓝盖住了底层的红
    CHECK(doc.layer(0).albedo.at(1, 1).b == 255);
    CHECK(h3.undo(doc));
    CHECK(doc.layerCount() == 2);
    CHECK(doc.layer(0).albedo.at(1, 1) == (RGBA8{200, 0, 0, 255}));
    CHECK(doc.layer(1).albedo.at(1, 1).b == 255);
    CHECK(doc.layer(1).name == "有名有姓");     // 合并撤销后名字也要回来
}

void testHistoryStructuralNoChange() {
    section("撤销重做 / 结构性操作没改动就不入栈");

    Document doc(4, 4);
    History h;
    h.beginAll(doc);
    CHECK(!h.commit(doc, "什么都没做"));    // 层数与内容都没变
    CHECK(h.undoSteps() == 0);

    // 改可见性**不**走 History，但要能识别为"内容没变"
    h.beginAll(doc);
    doc.layer(0).visible = false;
    CHECK(!h.commit(doc, "只改可见性"));    // 可见性不在像素差里
    CHECK(h.undoSteps() == 0);
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
    testDocument();
    testDocumentStructureVersion();
    testLayerMerge();
    testFlatten();
    testHistoryLayers();
    testHistoryStructural();
    testHistoryStructuralNoChange();

    printf("\n========================================\n");
    printf("通过 %d，失败 %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf("全部通过。\n");
        return 0;
    }
    printf("有失败项。\n");
    return 1;
}
