// app/panels.cpp — 画板的 UI 面板
//
// 布局（固定布局 + 可拖拽调宽 / 折叠；内置的 ImGui 发布版不含 docking）：
//
//   ┌──────────────── 主菜单栏 ────────────────┐
//   │ 工具栏 │◀▶│      画布      │◀▶│ 属性面板 │
//   │ (左栏) │  │ (裸 GL 绘制)   │  │ (右栏)  │
//   ├──────────────── 状态栏 ──────────────────┤
//
// ⚠️ 一个非常重要的坑：画布是一个 ImGui child 窗口，鼠标悬停在它上面时
//    io.WantCaptureMouse 会变成 **true**。所以**绝对不能**用 WantCaptureMouse
//    来判断"要不要把鼠标给画布"，否则永远画不上东西。
//    正确的判据是「画布 child 是不是当前被悬停的窗口」—— ImGui::IsWindowHovered()
//    在被别的窗口/弹出层/正在拖拽的控件挡住时会返回 false，正好就是我们想要的语义。
//    键盘快捷键则用 ui.wantsKeyboard() 来闸（避免在输入框里打字时触发工具切换）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <windows.h>
#endif

#include "app/app.h"
#include "app/panels.h"
#include "core/composite.h"
#include "core/console.h"
#include "core/image.h"
#include "gfx/gl_texture.h"
#include "imgui.h"
#include "ui/widgets.h"

namespace pb::app {

namespace {

ImU32 toU32(RGBA8 c, uint8_t forceA = 255) {
    return IM_COL32(c.r, c.g, c.b, forceA);
}

bool g_openNewCanvas = false;
bool g_openAbout = false;
bool g_openShortcuts = false;
int g_newW = 320, g_newH = 180;

// 自动化截图用：强制展开 / 折叠右侧栏的分区。
//
// 为什么需要它：ImGui 的折叠状态存在 .ini 里，功能一多右侧栏就长到要滚动，
// 于是"我改了图层面板"这件事在截图里根本看不到（被挤到屏幕外面了）。
// 有了这个开关，`--frames N --out x.png --open 图层` 就能把要验证的那一块
// 稳定地放到画面里，而不是靠人去滚。
// 留空 = 用 .ini 里的用户状态（正常交互时的行为）。
std::string g_forceOpen;
std::string g_forceClosed;

// ⚠️ 强制状态只能**施加一次**，不能每帧都施加。
//
// 这里踩过一个很难看的坑：一开始用的是
//     ImGui::SetNextItemOpen(..., ImGuiCond_Always);
// 结果**面板再也点不开了** —— Always 的含义是"每一帧都把状态设成这个值"，
// 用户点一下展开，下一帧立刻被按回去，表现就是点击完全没反应。
// 而且默认折叠的「材质」分区首当其冲（它的默认值就是 false），
// 于是具体症状就是"点材质栏无法展开"。
//
// 正确做法：只在**第一次**施加，之后完全交给 ImGui（它会把用户的选择存进 .ini）。
// 用户若想下次仍强制，再传一次 --open / --close 即可。
std::set<std::string> g_forcedOnce;

// 每个分区标题栏的屏幕矩形（配合 scripts/collapse_test.ps1 之类做自动点击验证）。
// 记录它几乎不花代价，但换来"折叠交互可以被自动化证明"，很值。
struct HeaderRect {
    std::string name;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool open = false;
};
std::vector<HeaderRect> g_headers;

bool sectionOpen(const char* name, bool defaultOpen, bool& alreadyForced) {
    const std::string n = name;
    alreadyForced = g_forcedOnce.count(n) > 0;
    if (alreadyForced) return defaultOpen;   // 本轮不再干预
    bool v = defaultOpen;
    if (!g_forceOpen.empty() && g_forceOpen.find(n) != std::string::npos) v = true;
    else if (!g_forceClosed.empty() && g_forceClosed.find(n) != std::string::npos) v = false;
    return v;
}

// 记录上一次 CollapsingHeader 的矩形（必须在它之后立刻调用）。
// 展开状态由调用方传入 —— ImGui 没有"查询上一个 Tree 节点是否展开"的公开 API，
// 但 CollapsingHeader 的返回值本身就是它，直接拿来用即可。
void recordHeader(const char* name, bool open) {
    HeaderRect h;
    h.name = name;
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    h.x0 = mn.x;
    h.y0 = mn.y;
    h.x1 = mx.x;
    h.y1 = mx.y;
    h.open = open;
    g_headers.push_back(h);
}

// 分区标题栏的提交。返回"默认是否展开"，调用方拿去当 CollapsingHeader 的 flags。
//
// ## 这里踩过一个很难看、也很典型的坑（用户报了"点材质栏无法展开"）
//
// 第一版为了给「材质」一个"默认折叠"的初始状态，每帧都调
//     ImGui::SetNextItemOpen(open, ImGuiCond_Always);
// 结果**所有分区都点不动了** —— Always 的含义是"每一帧都把状态设成这个值"，
// 用户点一下展开，下一帧立刻被按回去，表现就是点击完全没反应。
// 「材质」默认是折叠的，所以症状最明显：它永远展不开。
//
// 正确的做法是**不主动设置状态**，而是用 ImGui 自己的
// `ImGuiTreeNodeFlags_DefaultOpen`：它只在"用户还没有做过选择"时生效，
// 用户一旦点过（或被 .ini 记住过）就完全由用户说了算。
// `--open` / `--close` 也只在**第一次**提交那个分区时施加一次，之后不干预。
//
// 顺带说清另一个写法上的坑：`if (ImGui::CollapsingHeader(...))` 里
// CollapsingHeader **每一帧都必须调用**（它的返回值才是展开状态）。
// 写成 `if (open) CollapsingHeader(...)` 会让折叠后的控件再也不被提交，
// 状态永远翻不回来。
bool sectionHeader(const char* name, bool defaultOpen) {
    bool already = false;
    const bool forced = sectionOpen(name, defaultOpen, already);
    if (!already) {
        g_forcedOnce.insert(name);
        // 只有用户显式传了 --open / --close（forced 与默认值不一致）时才需要
        // 主动设一次初始状态，而且只设一次。
        if (forced != defaultOpen) ImGui::SetNextItemOpen(forced, ImGuiCond_Once);
    }
    // 默认展开交给 ImGuiTreeNodeFlags_DefaultOpen：它不会覆盖用户的选择
    return defaultOpen;
}

// 供 main 的自动点击验证使用。
//
// ⚠️ 关键：鼠标位置必须在 **ImGui::NewFrame() 之后**注入。
// GLFW 后端在 NewFrame 里会发现鼠标不在窗口上（自动化跑的是隐藏窗口），
// 于是拿真实光标位置去 AddMousePosEvent —— 把我们在 NewFrame 之前设的位置**冲掉**。
// 这也正是第一版脚本点击"派发了但 hovered=0"的原因。
bool sectionHeaderRect(const char* name, float& cx, float& cy) {
    for (const HeaderRect& h : g_headers) {
        if (h.name != name) continue;
        cx = (h.x0 + h.x1) * 0.5f;
        cy = (h.y0 + h.y1) * 0.5f;
        return true;
    }
    return false;
}

bool sectionHeaderOpen(const char* name) {
    for (const HeaderRect& h : g_headers)
        if (h.name == name) return h.open;
    return false;
}

}  // namespace

// 上面两个函数的对外入口（见 panels.h 的说明）
bool panelSectionPoint(const char* name, float& cx, float& cy) {
    return sectionHeaderRect(name, cx, cy);
}
bool panelSectionOpen(const char* name) { return sectionHeaderOpen(name); }
void beginPanelHeaderCapture() { g_headers.clear(); }

// 见上面 g_forceOpen 的说明
void setUiSections(const std::string& forceOpen, const std::string& forceClosed) {
    g_forceOpen = forceOpen;
    g_forceClosed = forceClosed;
    g_forcedOnce.clear();   // 新的一轮强制
}



namespace {

const ImGuiWindowFlags kWorkspaceFlags =
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
    ImGuiWindowFlags_NoScrollWithMouse;

// ---------------------------------------------------------------------------
// 画布叠加层：像素网格 + 笔刷光标
// ---------------------------------------------------------------------------

void drawCanvasOverlay(App& a) {
    if (!a.vp || !a.canvasVisible) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin(a.canvasX, a.canvasY);
    const ImVec2 end(a.canvasX + a.canvasW, a.canvasY + a.canvasH);
    const double ppc = a.vp->pixelsPerCell();

    // 像素网格：放大到一定程度才画，否则线比像素还密，只会糊成一片
    if (a.showPixelGrid && ppc >= 5.0) {
        const ImU32 gridCol = IM_COL32(255, 255, 255, 34);
        const int cw = a.doc.width(), chh = a.doc.height();

        // 只画落在可见范围内的线
        double uMin = 0.0, vMin = 0.0, uMax = 1.0, vMax = 1.0;
        a.vp->canvasToViewUV(0.0, 0.0, uMin, vMax);                       // 画布左上
        a.vp->canvasToViewUV((double)cw, (double)chh, uMax, vMin);        // 画布右下

        for (int x = 0; x <= cw; x++) {
            double u = 0.0, v = 0.0;
            a.vp->canvasToViewUV((double)x, 0.0, u, v);
            if (u < uMin - 0.01 || u > uMax + 0.01) continue;
            const float sx = origin.x + (float)(u * a.canvasW);
            dl->AddLine(ImVec2(sx, origin.y), ImVec2(sx, end.y), gridCol);
        }
        for (int y = 0; y <= chh; y++) {
            double u = 0.0, v = 0.0;
            a.vp->canvasToViewUV(0.0, (double)y, u, v);
            if (v < vMin - 0.01 || v > vMax + 0.01) continue;
            const float sy = origin.y + (float)((1.0 - v) * a.canvasH);
            dl->AddLine(ImVec2(origin.x, sy), ImVec2(end.x, sy), gridCol);
        }
    }

    // 笔刷光标：一个贴住画布像素格子的方框
    if (a.canvasHovered) {
        int cx = 0, cy = 0;
        a.vp->screenToCanvas(a.mx, a.my, cx, cy);
        const int size = std::max(1, a.opts.brushSize);
        const int half = (size - 1) / 2;

        double u0 = 0.0, v0 = 0.0, u1 = 0.0, v1 = 0.0;
        a.vp->canvasToViewUV((double)(cx - half), (double)(cy - half), u0, v0);
        a.vp->canvasToViewUV((double)(cx - half + size), (double)(cy - half + size), u1, v1);

        const ImVec2 p0(origin.x + (float)(u0 * a.canvasW),
                        origin.y + (float)((1.0 - v0) * a.canvasH));
        const ImVec2 p1(origin.x + (float)(u1 * a.canvasW),
                        origin.y + (float)((1.0 - v1) * a.canvasH));

        // 深色描边 + 亮色内框：无论画布是黑是白都能看清
        dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 200), 0.0f, 0, 3.0f);
        dl->AddRect(p0, p1, toU32(a.opts.color, 255), 0.0f, 0, 1.0f);
    }
}

}  // namespace

const char* lightModeName(LightMode m) {
    switch (m) {
        case LightMode::Sweep: return "扫描 GI";
        case LightMode::PathTrace: return "路径追踪";
        case LightMode::Trace25D: return "2.5D 光追";
        default: return "?";
    }
}

void requestNewCanvasDialog() { g_openNewCanvas = true; }

void actSetLightMode(App& a, LightMode mode) {
    if (a.lightMode == mode) return;
    a.lightMode = mode;
    a.lightDirty = true;
    // 两种模式的样本语义完全不同，切换时必须清空累积，否则会短暂显示混合结果
    if (a.pt) a.pt->resetAccumulation();
    a.lastAction = std::string("照明模式 ") + lightModeName(mode);
}

void actLightSettingsChanged(App& a) {
    a.lightDirty = true;
    if (a.pt) a.pt->resetAccumulation();
}

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------

void actSetTool(App& a, ToolId t) { a.opts.tool = t; }

void actSetPaletteColor(App& a, int index) {
    a.palette.setCurrent(index);
    const RGBA8 c = a.palette.current();
    // 色板只提供 albedo 的 RGB。遮挡 alpha 是独立的笔刷属性，这里刻意**不**覆盖它，
    // 否则每次点色板都会把用户设好的半透明度重置掉。
    a.opts.color.r = c.r;
    a.opts.color.g = c.g;
    a.opts.color.b = c.b;
    pushRecentColor(a, a.opts.color);
}

void pushRecentColor(App& a, RGBA8 c) {
    int found = -1;
    for (int i = 0; i < a.recentCount; i++) {
        if (a.recent[i] == c) {
            found = i;
            break;
        }
    }
    if (found == 0) return;   // 已经是最新，不用动

    int last = 0;
    if (found > 0) {
        last = found;   // 提到最前：把 [0, found-1] 整体后移
    } else {
        if (a.recentCount < App::kRecentMax) a.recentCount++;
        last = a.recentCount - 1;
    }
    for (int i = last; i > 0; i--) a.recent[i] = a.recent[i - 1];
    a.recent[0] = c;
}

void actUndo(App& a) {
    if (a.history.undo(a.doc)) {
        a.lightDirty = true;
        a.layerPanelDirty = true;
        if (a.pt) a.pt->resetAccumulation();
        a.lastAction = "撤销";
    }
}

void actRedo(App& a) {
    if (a.history.redo(a.doc)) {
        a.lightDirty = true;
        a.layerPanelDirty = true;
        if (a.pt) a.pt->resetAccumulation();
        a.lastAction = "重做";
    }
}

void actClearCanvas(App& a) {
    // 清的是**当前图层**，不是整份文档 —— 与"工具只作用于当前图层"保持一致。
    // 但它是一次整层改动，所以走 beginAll：逐层取差在这里没有意义。
    a.history.beginAll(a.doc);
    a.doc.active().clear(RGBA8{0, 0, 0, 0}, Material{});
    a.history.commit(a.doc, "清空图层");
    a.lightDirty = true;
    a.layerPanelDirty = true;
    if (a.pt) a.pt->resetAccumulation();
    a.lastAction = "清空当前图层";
}

void actResetCamera(App& a) {
    if (a.vp) a.vp->fitIntegerZoom();
}

void actNewCanvas(App& a, int w, int h) {
    if (w < 1 || h < 1) return;
    // Document::resize 会丢弃所有图层并重置为单个空层
    a.doc.resize(w, h);
    a.history.reset();
    // LightSweep / PathTrace 的纹理和 SSBO 都是按初始尺寸一次性分配的，
    // 改尺寸必须重新 init（init 内部会 shutdown 再重建）
    if (a.sweep) a.sweep->init(w, h);
    if (a.pt) a.pt->init(w, h);
    if (a.vp) a.vp->setCanvasSize(w, h);
    a.lightDirty = true;
    a.layerPanelDirty = true;
    resetLightingCache();
    markAllLayersForLight(a);
    a.lastAction = "新建画布";
    a.cameraInitialized = false;   // 新画布重新做一次整数倍适配
    printf("[画布] 新建 %dx%d\n", w, h);
}

void actSavePng(App& a, bool lit) {
    char path[256];
    std::snprintf(path, sizeof(path), "out/paintboard_%03d%s.png", a.saveCounter,
                  lit ? "_lit" : "");
    a.saveCounter++;

#ifdef _WIN32
    _mkdir("out");
#endif

    const int W = a.doc.width(), H = a.doc.height();
    if (W <= 0 || H <= 0) return;

    if (!lit) {
        // 导出的是**压平后的结果**：图层是编辑期的概念，导出的应该就是用户看到的那张图。
        // 这里的 alpha 也是覆盖率 —— 直接写出 albedo 会让半透明像素看起来"没画"，
        // 所以用 core 的压平（它做的是标准 source-over）。
        Image flat(W, H);
        std::vector<Material> mats((size_t)W * (size_t)H);
        flattenDocument(a.doc, flat, mats, Rect{0, 0, W - 1, H - 1});
        if (writePNG(path, flat)) {
            a.lastAction = std::string("已保存 ") + path;
            printf("[保存] %s（压平后的画布）\n", path);
        }
        return;
    }

    if (!a.sweep) return;
    Image litImg(W, H);
    if (!gfx::readbackTexture(a.sweep->resultTexture(), W, H, litImg.data())) {
        a.lastAction = "受光图回读失败";
        return;
    }
    for (size_t i = 0; i < litImg.pixelCount(); i++) litImg.data()[i].a = 255;
    if (writePNG(path, litImg)) {
        a.lastAction = std::string("已保存 ") + path;
        printf("[保存] %s（逐层光照 + 合成后的结果）\n", path);
    }
}

void actZoomAtCursor(App& a, double factor) {
    if (!a.ctx || !a.vp) return;
    double cx = 0.0, cy = 0.0;
    a.vp->screenToCanvasF(a.mx, a.my, cx, cy);
    a.vp->zoomBy(factor);
    float du = 1.0f, dv = 1.0f;
    a.vp->viewUV(du, dv);
    // 反解相机，使光标下的画布点在缩放前后保持不动
    const double u = (a.mx - a.vp->viewX()) / a.vp->viewW();
    const double v = 1.0 - (a.my - a.vp->viewY()) / a.vp->viewH();
    a.vp->camera().x = cx / (double)a.vp->canvasWidth() - (double)du * (u - 0.5);
    a.vp->camera().y = cy / (double)a.vp->canvasHeight() - (double)dv * (v - 0.5);
}

// ---------------------------------------------------------------------------
// 主菜单栏
// ---------------------------------------------------------------------------

void drawMainMenuBar(App& a) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("新建...", "Ctrl+N")) g_openNewCanvas = true;
        ImGui::Separator();
        if (ImGui::MenuItem("导出 PNG（原始）", "Ctrl+S")) actSavePng(a, false);
        if (ImGui::MenuItem("导出 PNG（受光）", "Ctrl+Shift+S")) actSavePng(a, true);
        ImGui::Separator();
        if (ImGui::MenuItem("退出", "Esc")) a.requestQuit = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("编辑")) {
        if (ImGui::MenuItem("撤销", "Ctrl+Z", false, a.history.canUndo())) actUndo(a);
        if (ImGui::MenuItem("重做", "Ctrl+Y", false, a.history.canRedo())) actRedo(a);
        ImGui::Separator();
        if (ImGui::MenuItem("清空画布", nullptr, false, true)) actClearCanvas(a);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("视图")) {
        ImGui::MenuItem("显示像素网格", "G", &a.showPixelGrid);
        ImGui::Separator();
        if (ImGui::MenuItem("完整显示画布（留边）", nullptr,
                            a.vp && a.vp->fitMode() == gfx::FitMode::Contain)) {
            if (a.vp) a.vp->setFitMode(gfx::FitMode::Contain);
        }
        if (ImGui::MenuItem("铺满面板（裁切）", nullptr,
                            a.vp && a.vp->fitMode() == gfx::FitMode::Cover)) {
            if (a.vp) a.vp->setFitMode(gfx::FitMode::Cover);
        }
        ImGui::Separator();
        ImGui::MenuItem("左侧工具栏", nullptr, &a.leftOpen);
        ImGui::MenuItem("右侧属性面板", nullptr, &a.rightOpen);
        ImGui::Separator();
        if (ImGui::MenuItem("放大", nullptr)) actZoomAtCursor(a, 1.0 / 1.25);
        if (ImGui::MenuItem("缩小", nullptr)) actZoomAtCursor(a, 1.25);
        if (ImGui::MenuItem("适应窗口", "Ctrl+0")) actResetCamera(a);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("光照")) {
        if (ImGui::MenuItem("开启光照", "Tab", &a.lightOn)) a.lightDirty = true;
        ImGui::Separator();
        if (ImGui::MenuItem("扫描 GI（实时）", nullptr,
                            a.lightMode == LightMode::Sweep)) {
            a.lightMode = LightMode::Sweep;
            a.lightDirty = true;
        }
        if (ImGui::MenuItem("路径追踪（渐进）", nullptr,
                            a.lightMode == LightMode::PathTrace)) {
            a.lightMode = LightMode::PathTrace;
            a.lightDirty = true;
        }
        if (ImGui::MenuItem("2.5D 光追（柱顶曲面）", nullptr,
                            a.lightMode == LightMode::Trace25D)) {
            a.lightMode = LightMode::Trace25D;
            a.lightDirty = true;
            a.dz25Dirty = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("快捷键一览")) g_openShortcuts = true;
        if (ImGui::MenuItem("关于")) g_openAbout = true;
        ImGui::EndMenu();
    }

    // 右侧显示当前状态摘要
    {
        const float right = ImGui::GetWindowWidth() - 320.0f;
        if (right > 200.0f) {
            ImGui::SameLine(right);
            ImGui::TextDisabled("%s | %s | %.0f fps", lightModeName(a.lightMode),
                                a.lightOn ? "光照开" : "光照关", a.fps);
        }
    }

    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------------------
// 左栏：工具栏
// ---------------------------------------------------------------------------

void drawToolbarPanel(App& a) {
    ImGui::SeparatorText("工具");

    struct Entry { ToolId id; const char* label; const char* key; };
    static const Entry kTools[] = {
        {ToolId::Pencil, "铅笔", "P"},   {ToolId::Line, "直线", "L"},
        {ToolId::Rect, "矩形", "R"},     {ToolId::Ellipse, "椭圆", "E"},
        {ToolId::Fill, "油漆桶", "F"},   {ToolId::Dither, "抖动", "D"},
        {ToolId::Eraser, "橡皮", "X"},   {ToolId::Picker, "取色", "K"},
        {ToolId::Lighten, "加光", "I"},  {ToolId::Darken, "减光", "O"},
    };
    const int kCount = (int)(sizeof(kTools) / sizeof(kTools[0]));

    const float availW = ImGui::GetContentRegionAvail().x;
    const float btnW = std::max(a.ui(52.0f),
                                (availW - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    const ImVec2 btnSize(btnW, a.ui(26.0f));

    for (int i = 0; i < kCount; i++) {
        const Entry& e = kTools[i];
        char tip[96];
        std::snprintf(tip, sizeof(tip), "%s  (%s)", e.label, e.key);
        if (ui::toolButton(e.label, e.label, a.opts.tool == e.id, tip, btnSize))
            actSetTool(a, e.id);
        if ((i & 1) == 0) ImGui::SameLine();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("工具选项");

    ui::labeledSliderInt("##brush", &a.opts.brushSize, 1, 64, "笔刷大小", "%d px");

    ImGui::Checkbox("圆形笔刷", &a.opts.brushRound);
    ImGui::SameLine();
    ImGui::Checkbox("填充图形", &a.opts.shapeFilled);

    if (a.opts.tool == ToolId::Dither) {
        ui::labeledSliderInt("##dither", &a.opts.ditherLevel, 0, 100, "抖动密度", "%d%%");
    }
    if (a.opts.tool == ToolId::Lighten || a.opts.tool == ToolId::Darken) {
        ui::labeledSliderInt("##shade", &a.opts.shadeAmount, 1, 255, "强度");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("颜色");

    // 前景/背景双色块（Aseprite 风格）
    {
        const float sw = a.ui(34.0f);
        ui::colorSwatch("fg", toU32(a.opts.color), ImVec2(sw, sw), false, "前景色");
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("前景 #%02X%02X%02X", a.opts.color.r, a.opts.color.g, a.opts.color.b);
        ImGui::TextDisabled("a=%d  em=%d", a.opts.color.a, a.opts.material.emissionLevel());
        ImGui::EndGroup();
    }
    // 遮挡 alpha / 自发光 / 折射率都在右侧「材质」面板里编辑。
    // 这里刻意不再放一份滑块：同一个值有两个入口，早晚会出现"改了这边忘了那边"。
    ImGui::TextDisabled("遮挡与自发光见右侧「材质」面板");

    ImGui::Spacing();
    if (ImGui::Button("撤销", ImVec2(btnW, 0))) actUndo(a);
    ImGui::SameLine();
    if (ImGui::Button("重做", ImVec2(btnW, 0))) actRedo(a);
    ImGui::TextDisabled("撤销栈 %zu 步 / %.1f KB", a.history.undoSteps(),
                        a.history.bytes() / 1024.0);
}

// ---------------------------------------------------------------------------
// 中栏：画布
// ---------------------------------------------------------------------------

void drawCanvasPanel(App& a) {
    // 内容区矩形就是画布的绘制目标。
    // 注意不能用交互式 item 占据这块区域——那会让 ImGui 捕获鼠标，画布就画不上了。
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();

    a.canvasX = pos.x;
    a.canvasY = pos.y;
    a.canvasW = std::max(1.0f, size.x);
    a.canvasH = std::max(1.0f, size.y);
    a.canvasVisible = a.canvasW > 4.0f && a.canvasH > 4.0f;

    // 判据：画布是不是当前被悬停的窗口。
    // IsWindowHovered() 在别的窗口/弹出层盖住、或有控件正在被拖拽时返回 false。
    a.canvasHovered = a.canvasVisible && ImGui::IsWindowHovered();

    if (a.vp) {
        a.vp->setViewRect((double)a.canvasX, (double)a.canvasY, (double)a.canvasW,
                          (double)a.canvasH);
        // 鼠标位置换算成画布坐标，供状态栏与叠加层使用。
        //
        // 读的是**当前图层**而不是压平结果：状态栏的价值在于"告诉我马上要画上去/取到的
        // 是什么值"，而压平结果里混着下面几层，反而不是工具会读到的那个像素。
        a.vp->screenToCanvas(a.mx, a.my, a.cursorX, a.cursorY);
        if (!a.doc.empty()) {
            const int cx = std::clamp(a.cursorX, 0, a.doc.width() - 1);
            const int cy = std::clamp(a.cursorY, 0, a.doc.height() - 1);
            a.cursorColor = a.doc.active().albedo.at(cx, cy);
            a.cursorMaterial = a.doc.active().materialAt(cx, cy);
        }
    }

    drawCanvasOverlay(a);
}

// ---------------------------------------------------------------------------
// 右栏：属性面板
// ---------------------------------------------------------------------------

void drawPropertiesPanel(App& a) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));

    // SetNextItemOpen 必须紧贴在对应的 CollapsingHeader 之前，所以四个分区各自
    // 调一次 sectionHeader（不能抽成循环：中间还有 Spacing）。
    // 默认展开状态：材质分区参数最多、最长，默认折起来 ——
    // 否则右侧栏一开就要滚动才能看到「图层」，而图层是这一版的主角。
    //
    // ⚠️ 注意 `if (...CollapsingHeader(...))` 这个写法：**每一帧都要调用**
    // CollapsingHeader，它的返回值才是"是否展开"。不能写成
    // `if (open) CollapsingHeader(...)` —— 那样一旦折叠就再也不提交这个控件，
    // 状态永远无法翻转（同类的坑在 ImGui 里很常见）。
    const bool palDefault = sectionHeader("调色板", true);
    const bool palOpen = ImGui::CollapsingHeader(
        "调色板", palDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (palOpen) drawPaletteSection(a);
    recordHeader("调色板", palOpen);
    ImGui::Spacing();
    const bool matDefault = sectionHeader("材质", false);
    const bool matOpen = ImGui::CollapsingHeader(
        "材质", matDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (matOpen) drawMaterialSection(a);
    recordHeader("材质", matOpen);
    ImGui::Spacing();
    const bool lightDefault = sectionHeader("光照", true);
    const bool lightOpen = ImGui::CollapsingHeader(
        "光照", lightDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (lightOpen) drawLightingSection(a);
    recordHeader("光照", lightOpen);
    ImGui::Spacing();
    const bool layerDefault = sectionHeader("图层", true);
    const bool layerOpen = ImGui::CollapsingHeader(
        "图层", layerDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (layerOpen) drawLayerSection(a);
    recordHeader("图层", layerOpen);

    ImGui::PopStyleVar();
}
void drawPaletteSection(App& a) {
    // ------------------------------------------------------------------
    // 1. 预设下拉
    // ------------------------------------------------------------------
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##preset", a.palette.presetName())) {
        for (int i = 0; i < builtinPaletteCount(); i++) {
            const PalettePreset& p = builtinPalette(i);
            const bool selected = (i == a.palette.presetIndex());
            char label[96];
            std::snprintf(label, sizeof(label), "%s  (%d 色)", p.name, p.count);
            if (ImGui::Selectable(label, selected)) {
                a.palette.loadPreset(i);
                actSetPaletteColor(a, 0);
                a.lastAction = std::string("色板 ") + a.palette.presetName();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const int n = a.palette.size();

    // ------------------------------------------------------------------
    // 2. 色块网格（按面板宽度自动换行，NES 的 55 色也排得下）
    // ------------------------------------------------------------------
    const float sw = a.ui(22.0f);
    const float gap = a.ui(3.0f);
    const float availW = ImGui::GetContentRegionAvail().x;
    const int perRow = std::max(1, (int)((availW + gap) / (sw + gap)));
    const int rows = (n + perRow - 1) / perRow;
    const float gridH = (float)rows * (sw + gap);
    const float maxGridH = a.ui(210.0f);

    // 留 2px 内边距：选中环是画在色块外面的，不留边会被 child 的裁剪区切掉
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(a.ui(3.0f), a.ui(3.0f)));
    ImGui::BeginChild("##swatches", ImVec2(0.0f, std::min(gridH + a.ui(6.0f), maxGridH)),
                      ImGuiChildFlags_None);
    for (int i = 0; i < n; i++) {
        const RGBA8 c = a.palette.color(i);
        char tip[64];
        std::snprintf(tip, sizeof(tip), "#%02X%02X%02X", c.r, c.g, c.b);
        ImGui::PushID(i);
        if (ui::colorSwatch("sw", IM_COL32(c.r, c.g, c.b, 255), ImVec2(sw, sw),
                            i == a.palette.currentIndex(), tip)) {
            actSetPaletteColor(a, i);
        }
        ImGui::PopID();
        if (((i + 1) % perRow) != 0 && i + 1 < n) ImGui::SameLine(0.0f, gap);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // ------------------------------------------------------------------
    // 3. 自由拾色 + 十六进制输入
    // ------------------------------------------------------------------
    // ColorEdit3 一次覆盖两个需求：它的颜色按钮弹出拾色器，而 DisplayHex 让它
    // 直接显示一个可编辑的 #RRGGBB 输入框。比另外手写一个十六进制输入框更不容易出错。
    ImGui::Spacing();
    float col[3] = {a.opts.color.r / 255.0f, a.opts.color.g / 255.0f,
                    a.opts.color.b / 255.0f};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::ColorEdit3("##freecolor", col,
                          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB |
                              ImGuiColorEditFlags_PickerHueBar)) {
        a.opts.color.r = (uint8_t)std::lround(col[0] * 255.0f);
        a.opts.color.g = (uint8_t)std::lround(col[1] * 255.0f);
        a.opts.color.b = (uint8_t)std::lround(col[2] * 255.0f);
        pushRecentColor(a, a.opts.color);
    }

    // ------------------------------------------------------------------
    // 4. 加入色板 / 删除选中色
    // ------------------------------------------------------------------
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
                     0.5f;
    if (ImGui::Button("加入色板", ImVec2(bw, 0.0f))) {
        // 色板条目约定为不透明（a=255）：a 是笔刷的遮挡属性，不该混进色板，
        // 否则同一个 RGB 会因为 alpha 不同被当成两种颜色。
        RGBA8 entry = a.opts.color;
        entry.a = 255;
        int idx = -1;
        for (int i = 0; i < n; i++) {
            if (a.palette.color(i) == entry) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            idx = a.palette.addColor(entry);
            a.lastAction = "已加入色板";
        } else {
            a.palette.setCurrent(idx);
            a.lastAction = "该颜色已在色板中";
        }
        actSetPaletteColor(a, idx);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(a.palette.size() <= 1);
    if (ImGui::Button("删除选中色", ImVec2(bw, 0.0f))) {
        if (a.palette.removeColor(a.palette.currentIndex())) {
            a.lastAction = "已从色板删除";
            actSetPaletteColor(a, a.palette.currentIndex());
        }
    }
    ImGui::EndDisabled();

    // ------------------------------------------------------------------
    // 5. 最近使用的颜色（跨色板的编辑历史，所以存在 App 而不是 Palette 里）
    // ------------------------------------------------------------------
    if (a.recentCount > 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("最近使用");
        const float rsw = a.ui(18.0f);
        const float rgap = a.ui(3.0f);
        const int rPerRow = std::max(1, (int)((availW + rgap) / (rsw + rgap)));
        for (int i = 0; i < a.recentCount; i++) {
            const RGBA8 c = a.recent[i];
            char tip[80];
            std::snprintf(tip, sizeof(tip), "#%02X%02X%02X  alpha %d", c.r, c.g, c.b, c.a);
            ImGui::PushID(1000 + i);
            if (ui::colorSwatch("rc", IM_COL32(c.r, c.g, c.b, 255), ImVec2(rsw, rsw),
                                false, tip)) {
                a.opts.color = c;   // 完整恢复（含 alpha）
                pushRecentColor(a, c);
            }
            ImGui::PopID();
            if (((i + 1) % rPerRow) != 0 && i + 1 < a.recentCount)
                ImGui::SameLine(0.0f, rgap);
        }
    }
}

// 材质预设：一次性套用 (遮挡 alpha, 自发光, 折射率, 标志位) 的一组取值。
//
// 这些不是"随便起的名字"，每一个都对应光照模型里一种真实的像素行为；
// 特别是「霓虹管」与「彩色玻璃」正是把自发光和遮挡解耦之后才能区分的两类像素。
struct MaterialPreset {
    const char* name;
    const char* desc;
    uint8_t alpha;
    uint8_t emission;
    float ior;      // 1.0 = 不折射
    uint8_t flags;
};

const MaterialPreset kMaterialPresets[] = {
    {"空气", "完全通透，不挡光也不发光（等于擦除）", 0, 0, 1.0f, 0},
    {"半透明", "只挡一部分光，自己不发光", 128, 0, 1.0f, 0},
    {"霓虹管", "半透明 + 强自发光，会点亮周围", 110, 170, 1.0f, 0},
    {"彩色玻璃", "半透明 + 带色，透过的光被染色。建议用于细线/薄片："
                 "遮挡是逐格吸收的，大块填充会因厚度而变得不透明", 120, 0, 1.5f, 0},
    {"实心墙", "完全挡光，画面显示自身颜色", 255, 0, 1.0f, 0},
    {"自发光屏", "不透明但自发光 —— 旧模型表达不了的一类像素", 255, 230, 1.0f, 0},
};
const int kMaterialPresetCount = (int)(sizeof(kMaterialPresets) / sizeof(kMaterialPresets[0]));

void applyMaterialPreset(App& a, int i) {
    if (i < 0 || i >= kMaterialPresetCount) return;
    const MaterialPreset& p = kMaterialPresets[i];
    a.opts.color.a = p.alpha;
    // 预设给的是中性自发光（灰白），用户可以随后把它的颜色改成任意值 ——
    // 这正是"完全解耦"的意义：表面颜色与发光颜色互不影响。
    a.opts.material.setEmissionFrom(p.emission, p.emission, p.emission);
    a.opts.material.setIorValue(p.ior);
    a.opts.material.flags = p.flags;
    a.lastAction = std::string("材质 ") + p.name;
}

void drawMaterialSection(App& a) {
    Material& m = a.opts.material;
    RGBA8& col = a.opts.color;

    // ------------------------------------------------------------------
    // 1. 材质预设（注意：预设会同时改写颜色以外的全部轴）
    // ------------------------------------------------------------------
    ImGui::TextDisabled("预设（一键套用一组参数，会覆盖下面的各项）");
    const float pw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
                     0.5f;
    for (int i = 0; i < kMaterialPresetCount; i++) {
        const MaterialPreset& p = kMaterialPresets[i];
        if (ImGui::Button(p.name, ImVec2(pw, 0.0f))) applyMaterialPreset(a, i);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.desc);
        if ((i & 1) == 0 && i + 1 < kMaterialPresetCount) ImGui::SameLine();
    }

    // ==================================================================
    // 轴 1：颜色（albedo）
    // ==================================================================
    // 颜色决定两件事：像素显示成什么颜色、以及半透明时滤过什么颜色的光。
    // 它**不**决定亮度——亮度是下面独立的第二条轴。
    ImGui::Spacing();
    ImGui::SeparatorText("颜色（albedo）");
    float rgb[3] = {col.r / 255.0f, col.g / 255.0f, col.b / 255.0f};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::ColorEdit3("##albedo", rgb,
                          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB |
                              ImGuiColorEditFlags_PickerHueBar)) {
        col.r = (uint8_t)std::lround(rgb[0] * 255.0f);
        col.g = (uint8_t)std::lround(rgb[1] * 255.0f);
        col.b = (uint8_t)std::lround(rgb[2] * 255.0f);
        pushRecentColor(a, col);
    }
    ImGui::TextWrapped("决定像素显示成什么颜色，以及半透明时把透过的光染成什么颜色。");

    // ==================================================================
    // 轴 2：自发光（另一组 RGB，与 albedo 完全独立）
    // ==================================================================
    // 这是"完全解耦"的关键：自发光是**自己的一个 RGB 值**，不是从 albedo 派生的标量。
    // 所以暗色表面可以发绿光、红表面可以发蓝光，两者互不约束。
    ImGui::Spacing();
    ImGui::SeparatorText("自发光（独立 RGB）");
    float em[3] = {m.emR / 255.0f, m.emG / 255.0f, m.emB / 255.0f};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::ColorEdit3("##emcolor", em,
                          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB |
                              ImGuiColorEditFlags_PickerHueBar)) {
        m.emR = (uint8_t)std::lround(em[0] * 255.0f);
        m.emG = (uint8_t)std::lround(em[1] * 255.0f);
        m.emB = (uint8_t)std::lround(em[2] * 255.0f);
    }
    // 便捷：整条亮度缩放（仍然作用在那个 RGB 上，不引入新的耦合）
    int level = m.emissionLevel();
    ImGui::SetNextItemWidth(-1);
    if (ui::labeledSliderInt("##emlevel", &level, 0, 255, "亮度（等比缩放自发光 RGB）")) {
        if (m.emissionLevel() > 0) {
            const float k = (float)level / (float)m.emissionLevel();
            const int r = (int)std::lround(m.emR * k);
            const int g = (int)std::lround(m.emG * k);
            const int b = (int)std::lround(m.emB * k);
            m.emR = (uint8_t)std::clamp(r, 0, 255);
            m.emG = (uint8_t)std::clamp(g, 0, 255);
            m.emB = (uint8_t)std::clamp(b, 0, 255);
        } else if (level > 0) {
            m.setEmissionFrom((uint8_t)level, (uint8_t)level, (uint8_t)level);
        }
    }
    const float ew = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
                     0.5f;
    if (ImGui::Button("跟随表面颜色", ImVec2(ew, 0.0f)))
        m.setEmissionFrom(col.r, col.g, col.b);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("把自发光设成与 albedo 相同 —— 这不是必须的，只是常用的一键操作");
    ImGui::SameLine();
    if (ImGui::Button("无自发光", ImVec2(ew, 0.0f))) m.clearEmission();

    // 亮度渐变条：用与 blend 通道相同的公式画出"当前自发光在 6 档下的效果"
    {
        const int kSteps = 6;
        const float h = a.ui(18.0f);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float w = (avail - a.ui(2.0f) * (kSteps - 1)) / kSteps;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < kSteps; i++) {
            const float k = (float)i / (float)(kSteps - 1);
            // 与 blend 的实心分支同式：albedo + 自发光*k*1.7，峰值归一
            float r = col.r / 255.0f + m.emR / 255.0f * k * 1.7f;
            float g = col.g / 255.0f + m.emG / 255.0f * k * 1.7f;
            float b = col.b / 255.0f + m.emB / 255.0f * k * 1.7f;
            const float pk = std::max(std::max(r, g), b);
            if (pk > 1.0f) { r /= pk; g /= pk; b /= pk; }
            const ImVec2 q0(p0.x + (w + a.ui(2.0f)) * i, p0.y);
            const ImVec2 q1(q0.x + w, q0.y + h);
            dl->AddRectFilled(q0, q1,
                              IM_COL32((int)std::lround(r * 255), (int)std::lround(g * 255),
                                       (int)std::lround(b * 255), 255));
            dl->AddRect(q0, q1, IM_COL32(120, 124, 134, 255));
        }
        ImGui::Dummy(ImVec2(avail, h));
    }
    ImGui::TextWrapped("自发光是**自己的一个 RGB 值**，与上面的颜色互不影响："
                       "深色表面可以发绿光，红表面可以发蓝光。");

    // ==================================================================
    // 轴 3：遮挡（alpha）
    // ==================================================================
    ImGui::Spacing();
    ImGui::SeparatorText("遮挡（alpha）");
    int alpha = col.a;
    if (ui::labeledSliderInt("##matAlpha", &alpha, 0, 255,
                             "遮挡 alpha（0 = 空气，255 = 完全挡光）")) {
        col.a = (uint8_t)alpha;
    }
    ImGui::TextWrapped("0 = 空气（光直接穿过），255 = 完全挡光。"
                       "中间值是半透明介质：既挡一部分光，又把透过的光按上面的颜色染色。");

    // ==================================================================
    // 几何：反射 / 折射（只在路径追踪模式下生效）
    // ==================================================================
    ImGui::Spacing();
    ImGui::SeparatorText("几何");
    float ior = m.iorValue();
    ImGui::SetNextItemWidth(-1);
    if (ui::labeledSliderFloat("##matIor", &ior, 1.0f, 3.55f, "折射率（1.00 = 不折射）")) {
        m.setIorValue(ior);
    }
    bool solid = m.solid();
    if (ImGui::Checkbox("强制实心", &solid)) m.setSolid(solid);

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
    ImGui::TextWrapped("遮挡与自发光：两种模式都生效。"
                       "折射：只在「路径追踪」模式下生效"
                       "（扫描 GI 是直线扫描，物理上做不了弯曲光路）。");
    ImGui::PopStyleColor();

    // ==================================================================
    // 预览：左右并排显示"颜色"和"该颜色在当前位置的最终外观"
    // ==================================================================
    ImGui::Spacing();
    ImGui::SeparatorText("预览");
    {
        const float h = a.ui(30.0f);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float w = (avail - a.ui(4.0f)) * 0.5f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##matpreview", ImVec2(avail, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();

        auto checker = [&](ImVec2 q0, ImVec2 q1) {
            const float cell = a.ui(7.0f);
            for (float y = q0.y; y < q1.y; y += cell)
                for (float x = q0.x; x < q1.x; x += cell) {
                    const bool odd = ((int)((x - q0.x) / cell) + (int)((y - q0.y) / cell)) & 1;
                    dl->AddRectFilled(ImVec2(x, y),
                                      ImVec2(std::min(x + cell, q1.x), std::min(y + cell, q1.y)),
                                      odd ? IM_COL32(58, 58, 62, 255) : IM_COL32(86, 86, 92, 255));
                }
        };

        // 左：纯颜色（带棋盘，直观看出遮挡程度）
        const ImVec2 l0(p0.x, p0.y), l1(p0.x + w, p0.y + h);
        checker(l0, l1);
        const float cov = (m.solid() ? 255.0f : (float)col.a) / 255.0f;
        dl->AddRectFilled(l0, l1, IM_COL32(col.r, col.g, col.b, (int)std::lround(cov * 255.0f)));
        dl->AddRect(l0, l1, IM_COL32(120, 124, 134, 255));

        // 右：加上自发光之后的外观（与着色器同式：albedo + 自发光*1.7，峰值归一）
        float r = col.r / 255.0f + m.emR / 255.0f * 1.7f;
        float g = col.g / 255.0f + m.emG / 255.0f * 1.7f;
        float b = col.b / 255.0f + m.emB / 255.0f * 1.7f;
        const float pk = std::max(std::max(r, g), b);
        if (pk > 1.0f) { r /= pk; g /= pk; b /= pk; }
        const ImVec2 r0(p0.x + w + a.ui(4.0f), p0.y), r1(r0.x + w, p0.y + h);
        dl->AddRectFilled(r0, r1,
                          IM_COL32((int)std::lround(r * 255), (int)std::lround(g * 255),
                                   (int)std::lround(b * 255), 255));
        dl->AddRect(r0, r1, IM_COL32(120, 124, 134, 255));
    }
    ImGui::TextDisabled("左 = 颜色（棋盘透出多少 = 遮挡程度）　右 = 加上亮度后的外观");

    // ------------------------------------------------------------------
    // 从画布吸取
    // ------------------------------------------------------------------
    ImGui::Spacing();
    if (ImGui::Button("吸取光标下的颜色与材质", ImVec2(-1.0f, 0.0f))) {
        a.opts.color = a.cursorColor;
        a.opts.material = a.cursorMaterial;
        pushRecentColor(a, a.opts.color);
        a.lastAction = "已吸取材质";
    }
    ImGui::TextDisabled("也可以直接用取色器工具（K）在画布上点");
}

void drawLightingSection(App& a) {
    // ------------------------------------------------------------------
    // 1. 照明模式
    // ------------------------------------------------------------------
    ImGui::TextDisabled("照明模式");
    const float mw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
                     0.5f;
    const bool isSweep = (a.lightMode == LightMode::Sweep);
    const bool isPt = (a.lightMode == LightMode::PathTrace);
    const bool is25D = (a.lightMode == LightMode::Trace25D);
    if (ui::toolButton("modeSweep", "扫描 GI", isSweep, "每帧沿 N 个方向扫描，极快，无反射",
                       ImVec2(mw, a.ui(26.0f))))
        actSetLightMode(a, LightMode::Sweep);
    ImGui::SameLine();
    if (ui::toolButton("modePt", "路径追踪", isPt,
                       "每条射线可反射/漫反射/折射，逐步累积收敛", ImVec2(mw, a.ui(26.0f))))
        actSetLightMode(a, LightMode::PathTrace);
    if (ui::toolButton("mode25d", "2.5D 光追", is25D,
                       "把图层当体素空间：只算柱顶曲面的上半球辐照度。"
                       "上层的光会照亮下层，并投出光晕",
                       ImVec2(-1.0f, a.ui(26.0f))))
        actSetLightMode(a, LightMode::Trace25D);

    ImGui::Spacing();
    ImGui::Checkbox("开启光照", &a.lightOn);
    if (ImGui::IsItemEdited()) a.lightDirty = true;
    ImGui::SameLine();
    if (ImGui::Button("重置累积")) actLightSettingsChanged(a);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("清空时间累积，重新从第 1 个样本开始收敛");

    // ------------------------------------------------------------------
    // 2. 公共参数
    // ------------------------------------------------------------------
    ImGui::SeparatorText("公共");
    {
        float amb[3] = {0.0f, 0.0f, 0.0f};
        if (isSweep && a.sweep) {
            amb[0] = a.sweep->settings().ambient[0];
            amb[1] = a.sweep->settings().ambient[1];
            amb[2] = a.sweep->settings().ambient[2];
        } else if (isPt && a.pt) {
            amb[0] = a.pt->settings().ambient[0];
            amb[1] = a.pt->settings().ambient[1];
            amb[2] = a.pt->settings().ambient[2];
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::ColorEdit3("##ambient", amb, ImGuiColorEditFlags_InputRGB)) {
            if (isSweep && a.sweep) {
                for (int i = 0; i < 3; i++) a.sweep->settings().ambient[i] = amb[i];
            } else if (isPt && a.pt) {
                for (int i = 0; i < 3; i++) a.pt->settings().ambient[i] = amb[i];
            }
            actLightSettingsChanged(a);
        }
        ImGui::TextDisabled("环境光（逃出画布的射线得到的亮度）");

        float em = isSweep && a.sweep ? a.sweep->settings().emitScale
                                      : (a.pt ? a.pt->settings().emitScale : 1.0f);
        ImGui::SetNextItemWidth(-1);
        if (ui::labeledSliderFloat("##emit", &em, 0.0f, 4.0f, "自发光倍率")) {
            if (isSweep && a.sweep) a.sweep->settings().emitScale = em;
            else if (a.pt) a.pt->settings().emitScale = em;
            actLightSettingsChanged(a);
        }
    }

    // ------------------------------------------------------------------
    // 3. 模式专属参数
    // ------------------------------------------------------------------
    if (isSweep && a.sweep) {
        ImGui::SeparatorText("扫描 GI");
        gfx::LightSettings& s = a.sweep->settings();
        int r = s.rayRadius;
        if (ui::labeledSliderInt("##radius", &r, 4, 512, "方向表半径")) {
            s.rayRadius = r;
            actLightSettingsChanged(a);
        }
        ImGui::TextDisabled("当前 %d 个方向（半径越大越平滑、越慢）",
                            a.sweep->directionCount());
        ImGui::SetNextItemWidth(-1);
        if (ui::labeledSliderFloat("##cp", &s.propagate, 0.5f, 2.0f, "沿程增益")) {
            actLightSettingsChanged(a);
        }
    } else if (is25D) {
        // ---- 2.5D 光追的专属参数 ----
        ImGui::SeparatorText("2.5D 光追");
        float r = a.dz25Radius;
        if (ui::labeledSliderFloat("dz25r", &r, 8.0f, 320.0f, "作用半径", "%.0f")) {
            a.dz25Radius = r;
            a.dz25Dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("只收这个半径内的发光体素（1/r³ 让远处贡献迅速变得无关紧要）");
        float g2 = a.dz25Gain;
        if (ui::labeledSliderFloat("dz25gain", &g2, 0.5f, 40.0f, "辐照度增益", "%.1f")) {
            a.dz25Gain = g2;
            a.dz25Dirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("光晕整体亮度。太大时高光会被归一化压成纯白");
        float sun = a.dz25Sun;
        if (ui::labeledSliderFloat("dz25sun", &sun, 0.0f, 1.0f, "天光强度")) {
            a.dz25Sun = sun;
            a.dz25Dirty = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("上半球里非太阳方向的背景光");
        float es = a.dz25EmitScale;
        if (ui::labeledSliderFloat("dz25em", &es, 0.0f, 4.0f, "自发光倍率")) {
            a.dz25EmitScale = es;
            a.dz25Dirty = true;
        }
        ImGui::TextDisabled("图层 = z 层；解析 gather，无方向采样 -> 无条带");
    } else if (a.pt) {
        ImGui::SeparatorText("路径追踪");
        gfx::PathTraceSettings& s = a.pt->settings();

        int steps = s.maxSteps;
        if (ui::labeledSliderInt("##steps", &steps, 16, 512, "最大步长")) {
            s.maxSteps = steps;
            actLightSettingsChanged(a);
        }
        int bounces = s.maxBounces;
        if (ui::labeledSliderInt("##bounces", &bounces, 1, 8,
                                 "最大弹射（反射/折射/漫反射的总次数）")) {
            s.maxBounces = bounces;
            actLightSettingsChanged(a);
        }

        int spp = s.samplesPerFrame;
        if (ui::labeledSliderInt("##spp", &spp, 1, 16, "每帧采样（条/像素）")) {
            s.samplesPerFrame = spp;
            actLightSettingsChanged(a);
        }
        ImGui::TextDisabled("噪声按 1/sqrt(N) 下降；单帧成本有大量余量可用");

        ImGui::SetNextItemWidth(-1);
        if (ui::labeledSliderFloat("##exposure", &s.exposure, 0.2f, 5.0f, "曝光")) {
            actLightSettingsChanged(a);
        }
        if (ImGui::Checkbox("色调映射（Reinhard）", &s.tonemap)) actLightSettingsChanged(a);
        ImGui::TextDisabled("累积值可能远超 1，关掉会丢掉高光");

        // 收敛进度
        ImGui::Spacing();
        const int n = a.pt->sampleCount();
        const float frac = (float)n / (float)gfx::PathTrace::kConvergedSamples;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d / %d 样本", n,
                      gfx::PathTrace::kConvergedSamples);
        ImGui::ProgressBar(frac, ImVec2(-1.0f, a.ui(14.0f)), buf);
        if (a.pt->converged())
            ImGui::TextDisabled("已收敛，停止继续采样（省 GPU）");
        else
            ImGui::TextDisabled("每帧 1 条射线，靠时间累积收敛");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("光照 %.1f ms | 光照图层 %s", a.lightMs, a.lightOn ? "开" : "关");
}

// ---------------------------------------------------------------------------
// 状态栏
// ---------------------------------------------------------------------------

void drawStatusBar(App& a) {
    ImGui::AlignTextToFramePadding();

    ImGui::Text("(%d, %d)", a.cursorX, a.cursorY);

    ImGui::SameLine(0, 12);
    {
        const RGBA8 c = a.cursorColor;
        ui::colorSwatch("statuscol", toU32(c), ImVec2(16, 16), false,
                        "光标下的像素颜色");
        ImGui::SameLine(0, 4);
        if (c.a == 0)
            ImGui::TextDisabled("空");
        else
            ImGui::Text("#%02X%02X%02X a=%d e=%d", c.r, c.g, c.b, c.a,
                        a.cursorMaterial.emissionLevel());
    }

    ImGui::SameLine(0, 12);
    ImGui::Text("| %s | %s | %dx%d", toolName(a.opts.tool), a.palette.presetName(),
                a.doc.width(), a.doc.height());

    ImGui::SameLine(0, 12);
    ImGui::Text("| %s", a.doc.active().name.c_str());

    ImGui::SameLine(0, 12);
    ImGui::Text("| 缩放 %.0f%%", a.vp ? 100.0 / std::max(0.01, a.vp->camera().zoom) : 100.0);

    ImGui::SameLine(0, 12);
    ui::statusDot(a.lightOn ? IM_COL32(120, 220, 120, 255) : IM_COL32(120, 120, 130, 255),
                  a.lightOn ? "光照开" : "光照关");

    ImGui::SameLine(0, 12);
    ImGui::Text("| 撤销 %zu / 重做 %zu", a.history.undoSteps(), a.history.redoSteps());

    // 右对齐：fps 与光照耗时
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "光照 %.1f ms | %.0f fps", a.lightMs, a.fps);
        const float w = ImGui::CalcTextSize(buf).x;
        const float right = ImGui::GetWindowWidth() - w - 14.0f;
        if (right > ImGui::GetCursorPosX()) {
            ImGui::SameLine(right);
            ImGui::TextUnformatted(buf);
        }
    }

    // 最近一次动作提示
    if (!a.lastAction.empty()) {
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("· %s", a.lastAction.c_str());
    }
}

// ---------------------------------------------------------------------------
// 整帧 UI
// ---------------------------------------------------------------------------

void drawUi(App& a) {
    drawMainMenuBar(a);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // 工作区窗口本身必须**透明**：画布是用裸 GL 画的，而 ImGui 在 GL 之后渲染。
    // 如果这里保留不透明背景，它会把画布整块盖住（已实测踩过，表现为画布全黑）。
    // 各面板用自己 child 的背景上色，画布 child 保持透明，于是 GL 画布能透出来。
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::Begin("##workspace", nullptr, kWorkspaceFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterW = 6.0f;
    const float bodyH = std::max(40.0f, avail.y - a.statusHeight);

    float leftW = a.leftOpen ? a.leftWidth : 0.0f;
    float rightW = a.rightOpen ? a.rightWidth : 0.0f;
    const float minCanvas = 96.0f;
    const float splitters = (a.leftOpen ? splitterW : 0.0f) + (a.rightOpen ? splitterW : 0.0f);

    // 宽度收敛：窗口很窄时优先压缩右栏，再压左栏，保证画布还剩得下
    if (leftW + rightW + splitters + minCanvas > avail.x) {
        float over = leftW + rightW + splitters + minCanvas - avail.x;
        const float takeR = std::min(over, std::max(0.0f, rightW - 180.0f));
        rightW -= takeR;
        over -= takeR;
        if (over > 0.0f) leftW = std::max(0.0f, leftW - over);
    }

    const float canvasW = std::max(minCanvas, avail.x - leftW - rightW - splitters);

    // --- 左栏 ---
    if (a.leftOpen) {
        ImGui::BeginChild("##left", ImVec2(leftW, bodyH), ImGuiChildFlags_Borders);
        drawToolbarPanel(a);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
        ui::verticalSplitter("##splitL", &a.leftWidth, 170.0f, 460.0f, bodyH, +1.0f);
        ImGui::SameLine(0.0f, 0.0f);
    }

    // --- 中栏：画布 ---
    // 背景透明 + 只留边框：这样裸 GL 画出来的画布能从中间透出来，
    // 而 ImGui 的边框与叠加层仍然画在它上面。
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##canvas", ImVec2(canvasW, bodyH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoMove);
    drawCanvasPanel(a);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // --- 右栏 ---
    if (a.rightOpen) {
        ImGui::SameLine(0.0f, 0.0f);
        ui::verticalSplitter("##splitR", &a.rightWidth, 220.0f, 560.0f, bodyH, -1.0f);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::BeginChild("##right", ImVec2(rightW, bodyH), ImGuiChildFlags_Borders);
        drawPropertiesPanel(a);
        ImGui::EndChild();
    }

    // --- 底部状态栏（显式定位，避免受 ItemSpacing 影响）---
    ImGui::SetCursorPos(ImVec2(0.0f, bodyH));
    ImGui::BeginChild("##status", ImVec2(avail.x, std::max(20.0f, avail.y - bodyH)),
                      ImGuiChildFlags_Borders);
    drawStatusBar(a);
    ImGui::EndChild();

    ImGui::End();

    // --- 字体加载失败：必须显式告诉用户 ---
    //
    // 双击 exe 启动时没有控制台，如果只在 stderr 里报一句，用户只会看到满屏 '?'
    // 而完全不知道发生了什么。
    //
    // 注意这里的文案**全部是 ASCII**：这个弹窗要报告的问题就是"中文字形不可用"，
    // 文案要是用中文写，弹窗本身就会变成一片 '?'，等于没报。
    if (!a.fontWarning.empty()) {
        if (!a.fontWarningShown) {
            ImGui::OpenPopup("UI font not found");
            a.fontWarningShown = true;
        }
        if (ImGui::BeginPopupModal("UI font not found", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.40f, 1.0f));
            ImGui::TextUnformatted("Chinese UI text cannot be rendered.");
            ImGui::PopStyleColor();
            ImGui::Separator();
            ImGui::TextUnformatted(a.fontWarning.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("The app looks for assets in: exe dir -> its parent -> cwd,");
            ImGui::TextDisabled("so keeping the font in the project root also works.");
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(a.ui(120.0f), 0.0f)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // --- 对话框 ---
    if (g_openShortcuts) ImGui::OpenPopup("快捷键一览");
    if (g_openShortcuts && ImGui::BeginPopupModal("快捷键一览", &g_openShortcuts,
                                                  ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "工具   P 铅笔   L 直线   R 矩形   E 椭圆   F 油漆桶\n"
            "       D 抖动   K 取色   X 橡皮   I 加光   O 减光\n"
            "笔刷   [ ] 改大小      B 方圆切换     G 像素网格\n"
            "色板   1-9,0 选色      , . 切换色板\n"
            "视图   滚轮缩放        中键/空格+左键 拖拽平移\n"
            "       Ctrl+0 适应窗口\n"
            "编辑   Ctrl+Z 撤销     Ctrl+Y 重做    Ctrl+N 新建\n"
            "导出   Ctrl+S 原始图   Ctrl+Shift+S 受光图\n"
            "光照   Tab 开关光照");
        ImGui::Spacing();
        if (ImGui::Button("关闭", ImVec2(120, 0))) g_openShortcuts = false;
        ImGui::EndPopup();
    }

    if (g_openAbout) ImGui::OpenPopup("关于");
    if (g_openAbout && ImGui::BeginPopupModal("关于", &g_openAbout,
                                              ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("paintboard — 2D 像素风格画板");
        ImGui::Separator();
        ImGui::Text("由 magic-simulator 模块改造而来");
        ImGui::Text("渲染：方向扫描式全局光照（路径追踪见 P2）");
        ImGui::Text("界面：Dear ImGui %s", IMGUI_VERSION);
        ImGui::Text("字体：Unifont（含全量中文）");
        ImGui::Spacing();
        if (ImGui::Button("关闭", ImVec2(120, 0))) g_openAbout = false;
        ImGui::EndPopup();
    }

    if (g_openNewCanvas) ImGui::OpenPopup("新建画布");
    if (g_openNewCanvas && ImGui::BeginPopupModal("新建画布", &g_openNewCanvas,
                                                  ImGuiWindowFlags_AlwaysAutoResize)) {
        static const int kPresets[][2] = {
            {64, 64}, {128, 128}, {256, 256}, {320, 180}, {512, 288},
        };
        for (int i = 0; i < 5; i++) {
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%dx%d", kPresets[i][0], kPresets[i][1]);
            if (ImGui::Button(lbl, ImVec2(84, 0))) {
                actNewCanvas(a, kPresets[i][0], kPresets[i][1]);
                g_openNewCanvas = false;
            }
            if (i < 4) ImGui::SameLine();
        }
        ImGui::Spacing();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("宽", &g_newW, 0, 0);
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("高", &g_newH, 0, 0);
        g_newW = std::clamp(g_newW, 1, 2048);
        g_newH = std::clamp(g_newH, 1, 2048);
        ImGui::Spacing();
        if (ImGui::Button("创建", ImVec2(120, 0))) {
            actNewCanvas(a, g_newW, g_newH);
            g_openNewCanvas = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) g_openNewCanvas = false;
        ImGui::EndPopup();
    }
}

}  // namespace pb::app
