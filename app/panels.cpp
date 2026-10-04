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
        const int cw = a.canvas.width(), chh = a.canvas.height();

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
        default: return "?";
    }
}

void requestNewCanvasDialog() { g_openNewCanvas = true; }

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------

void actSetTool(App& a, ToolId t) { a.opts.tool = t; }

void actSetPaletteColor(App& a, int index) {
    a.palette.setCurrent(index);
    a.opts.color = a.palette.current();
}

void actUndo(App& a) {
    if (a.history.undo(a.canvas)) {
        a.lightDirty = true;
        a.lastAction = "撤销";
    }
}

void actRedo(App& a) {
    if (a.history.redo(a.canvas)) {
        a.lightDirty = true;
        a.lastAction = "重做";
    }
}

void actClearCanvas(App& a) {
    a.history.beginStroke(a.canvas);
    a.canvas.clear(RGBA8{0, 0, 0, 0}, Material{});
    a.history.endStroke(a.canvas, "清空");
    a.lightDirty = true;
    a.lastAction = "清空画布";
}

void actResetCamera(App& a) {
    if (a.vp) a.vp->resetCamera();
}

void actNewCanvas(App& a, int w, int h) {
    if (w < 1 || h < 1) return;
    a.canvas.resize(w, h);
    a.canvas.clear(RGBA8{0, 0, 0, 0}, Material{});
    a.history.reset();
    // LightSweep 的纹理和 SSBO 是按初始尺寸一次性分配的，改尺寸必须重新 init
    if (a.sweep) a.sweep->init(w, h);
    if (a.vp) a.vp->setCanvasSize(w, h);
    a.lightDirty = true;
    a.lastAction = "新建画布";
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

    if (!lit) {
        if (writePNG(path, a.canvas.image())) {
            a.lastAction = std::string("已保存 ") + path;
            printf("[保存] %s（原始画布）\n", path);
        }
        return;
    }

    if (!a.sweep) return;
    Image litImg(a.canvas.width(), a.canvas.height());
    if (!gfx::readbackTexture(a.sweep->resultTexture(), a.canvas.width(),
                              a.canvas.height(), litImg.data())) {
        a.lastAction = "受光图回读失败";
        return;
    }
    for (size_t i = 0; i < litImg.pixelCount(); i++) litImg.data()[i].a = 255;
    if (writePNG(path, litImg)) {
        a.lastAction = std::string("已保存 ") + path;
        printf("[保存] %s（受光结果）\n", path);
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
                            a.lightMode == LightMode::PathTrace, false)) {
            // P2 实现；这里先禁用，避免选了却没有效果
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

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##brush", &a.opts.brushSize, 1, 64, "笔刷 %d px",
                     ImGuiSliderFlags_AlwaysClamp);

    ImGui::Checkbox("圆形笔刷", &a.opts.brushRound);
    ImGui::SameLine();
    ImGui::Checkbox("填充图形", &a.opts.shapeFilled);

    if (a.opts.tool == ToolId::Dither) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##dither", &a.opts.ditherLevel, 0, 100, "抖动密度 %d%%",
                         ImGuiSliderFlags_AlwaysClamp);
    }
    if (a.opts.tool == ToolId::Lighten || a.opts.tool == ToolId::Darken) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##shade", &a.opts.shadeAmount, 1, 255, "强度 %d",
                         ImGuiSliderFlags_AlwaysClamp);
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
        ImGui::TextDisabled("a=%d  e=%d", a.opts.color.a, a.opts.material.emission);
        ImGui::EndGroup();
    }

    ImGui::SetNextItemWidth(-1);
    int alpha = a.opts.color.a;
    if (ImGui::SliderInt("##alpha", &alpha, 0, 255, "遮挡 alpha %d",
                         ImGuiSliderFlags_AlwaysClamp)) {
        a.opts.color.a = (uint8_t)alpha;
    }
    // TextWrapped 按可用宽度折行，避免超出面板被裁掉
    ImGui::TextWrapped("alpha 同时是光照模型里的\"通透程度\"：0=空气，255=实心挡光。");

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
        // 鼠标位置换算成画布坐标，供状态栏与叠加层使用
        a.vp->screenToCanvas(a.mx, a.my, a.cursorX, a.cursorY);
        if (!a.canvas.empty()) {
            a.cursorColor = a.canvas.image().at(a.cursorX, a.cursorY);
            a.cursorMaterial = a.canvas.materialAt(a.cursorX, a.cursorY);
        }
    }

    drawCanvasOverlay(a);
}

// ---------------------------------------------------------------------------
// 右栏：属性面板
// ---------------------------------------------------------------------------

void drawPropertiesPanel(App& a) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));

    if (ImGui::CollapsingHeader("调色板", ImGuiTreeNodeFlags_DefaultOpen))
        drawPaletteSection(a);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("材质", ImGuiTreeNodeFlags_DefaultOpen))
        drawMaterialSection(a);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("光照", ImGuiTreeNodeFlags_DefaultOpen))
        drawLightingSection(a);
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("图层"))
        ImGui::TextDisabled("图层面板将在 P4 实现");

    ImGui::PopStyleVar();
}

void drawPaletteSection(App& a) {
    ImGui::TextDisabled("调色板面板待实现（UI-1）");
    ImGui::TextDisabled("当前：%s  [%d/%d]", a.palette.presetName(),
                        a.palette.currentIndex() + 1, a.palette.size());
}

void drawMaterialSection(App& a) {
    ImGui::TextDisabled("材质面板待实现（UI-2）");
    ImGui::TextDisabled("自发光 %d / 折射率 %.2f / 反射 %s", a.opts.material.emission,
                        a.opts.material.iorValue(), a.opts.material.reflective() ? "是" : "否");
}

void drawLightingSection(App& a) {
    ImGui::TextDisabled("光照面板待实现（UI-3）");
    ImGui::TextDisabled("模式：%s，扫描 GI 共 %d 个方向", lightModeName(a.lightMode),
                        a.sweep ? a.sweep->directionCount() : 0);
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
                        a.cursorMaterial.emission);
    }

    ImGui::SameLine(0, 12);
    ImGui::Text("| %s | %s | %dx%d", toolName(a.opts.tool), a.palette.presetName(),
                a.canvas.width(), a.canvas.height());

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
