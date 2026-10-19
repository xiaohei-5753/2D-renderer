// app/panels_canvas.cpp — 工作区骨架 + 左栏工具栏 + 中栏画布
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
//    键盘快捷键则用 ui.wantsKeyboard() 来闸（见 app/input.cpp）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app/panels_internal.h"
#include "core/history.h"
#include "core/layer.h"
#include "core/palette.h"
#include "ui/widgets.h"

namespace pb::app {

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
// 整帧 UI：工作区三栏 + 对话框
// ---------------------------------------------------------------------------

void drawUi(App& a) {
    // --bench-scene 的读数叠加层：必须**在 hidePanels 之前**画，
    // 否则一开 --hide-ui 就什么都看不见了 —— 而实机测试时恰恰需要
    // 一边看画面一边读帧率。
    if (a.benchFrame > 0) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + a.ui(12.0f), vp->WorkPos.y + a.ui(12.0f)));
        ImGui::SetNextWindowBgAlpha(0.72f);
        ImGui::Begin("##benchhud", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 1.0f, 0.6f, 1.0f));
        ImGui::Text("基准场景  %d 层  %dx%d", a.doc.layerCount(), a.doc.width(),
                    a.doc.height());
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Text("帧号     %d", a.benchFrame);
        ImGui::Text("光照     %.2f ms", (double)a.lightMs);
        ImGui::Text("模式     %s", lightModeName(a.lightMode));
        if (a.sweep)
            ImGui::Text("方向     %d 个（半径 %d）", a.sweep->directionCount(),
                        a.sweep->settings().rayRadius);
        // 大号帧率：实机测试时一眼可读
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
        ImGui::Text("%.0f fps", a.fps);
        ImGui::PopStyleColor();
        ImGui::End();
    }

    // --hide-ui：跳过菜单与所有面板，但**必须**把画布矩形自己算出来。
    //
    // 这里踩过一个坑：`a.canvasVisible` 只在 drawCanvasPanel 里被赋值，
    // 所以最初"hidePanels 就直接 return"的写法会让 canvasVisible 永远为 false
    // —— 画面完全不重绘，截出来的两帧几乎逐像素相同（只有 410 个像素差），
    // 看起来像"动画卡住了"。实际是画布压根没画。
    //
    // 所以这里改成：不画面板，但把整块工作区当作画布，并照常设置视口矩形。
    if (a.hidePanels) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        a.canvasX = vp->WorkPos.x;
        a.canvasY = vp->WorkPos.y;
        a.canvasW = std::max(1.0f, vp->WorkSize.x);
        a.canvasH = std::max(1.0f, vp->WorkSize.y);
        a.canvasVisible = a.canvasW > 4.0f && a.canvasH > 4.0f;
        a.canvasHovered = false;
        if (a.vp) {
            a.vp->setViewRect((double)a.canvasX, (double)a.canvasY, (double)a.canvasW,
                              (double)a.canvasH);
        }
        return;
    }

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

    drawDialogs(a);
}

}  // namespace pb::app
