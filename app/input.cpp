// app/input.cpp
#include "app/input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "app/app.h"
#include "app/panels.h"
#include "imgui.h"

#include <GLFW/glfw3.h>

namespace pb::app {

namespace {

App* g_app = nullptr;

// 在当前视口下把鼠标位置换算成画布坐标
void cursorToCanvas(App& a, double sx, double sy, int& cx, int& cy) {
    cx = cy = 0;
    if (!a.vp) return;
    a.vp->screenToCanvas(sx, sy, cx, cy);
}

void beginStrokeAt(App& a, int cx, int cy) {
    if (a.opts.tool == ToolId::Picker) {
        RGBA8 c;
        Material m;
        // 取色读的是**当前图层**：取色器给的是"我要继续画的那个值"，
        // 而压平结果里混着下面几层，取到的颜色画出来会不一样。
        if (pickPixel(a.doc.active(), cx, cy, c, m)) {
            a.opts.color = c;
            a.opts.material = m;
            pushRecentColor(a, c);
            a.lastAction = "取色";
        }
        return;
    }
    // 隐藏或锁定的图层不接受笔触：往看不见的层上画是纯粹的挫败来源
    if (!a.doc.activeWritable()) {
        a.lastAction = a.doc.active().locked ? "图层已锁定" : "图层已隐藏";
        return;
    }
    a.history.beginStroke(a.doc);
    a.session.begin(a.doc.active(), a.opts, cx, cy);
    a.stroking = true;
}

void endStroke(App& a) {
    if (!a.stroking) return;
    a.session.end();
    if (a.history.commit(a.doc, toolName(a.opts.tool))) {
        a.lightDirty = true;
        a.layerPanelDirty = true;
    }
    a.stroking = false;
}

void onMouseButton(GLFWwindow* w, int button, int action, int mods) {
    (void)mods;
    App& a = *g_app;
    const bool press = (action == GLFW_PRESS);
    glfwGetCursorPos(w, &a.mx, &a.my);

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        a.lmbDown = press;
        if (press) {
            // 鼠标是否归画布，用"画布是不是被悬停的窗口"来判断（见 panels.cpp 顶部说明）
            if (!a.canvasHovered) return;
            if (a.spaceDown) {
                a.panning = true;
            } else {
                int cx = 0, cy = 0;
                cursorToCanvas(a, a.mx, a.my, cx, cy);
                beginStrokeAt(a, cx, cy);
            }
        } else {
            if (a.panning) a.panning = false;
            else endStroke(a);
        }
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        a.rmbDown = press;
        if (press) {
            if (!a.canvasHovered) return;
            a.savedTool = a.opts.tool;
            a.opts.tool = ToolId::Eraser;
            int cx = 0, cy = 0;
            cursorToCanvas(a, a.mx, a.my, cx, cy);
            beginStrokeAt(a, cx, cy);
        } else {
            endStroke(a);
            a.opts.tool = a.savedTool;
        }
        return;
    }

    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        a.panning = press;
    }
}

void onCursorPos(GLFWwindow* w, double x, double y) {
    (void)w;
    App& a = *g_app;

    if (a.panning && a.vp) {
        float du = 1.0f, dv = 1.0f;
        a.vp->viewUV(du, dv);
        // 屏幕位移 -> 画布 UV 位移（除以绘制矩形尺寸，因为 uv 是相对矩形归一化的）
        a.vp->panByUV(-(x - a.mx) / a.vp->viewW() * du, (y - a.my) / a.vp->viewH() * dv);
    }

    a.mx = x;
    a.my = y;

    if (a.stroking) {
        int cx = 0, cy = 0;
        cursorToCanvas(a, x, y, cx, cy);
        a.session.update(a.doc.active(), a.opts, cx, cy);
    }
}

void onScroll(GLFWwindow* w, double dx, double dy) {
    (void)w;
    (void)dx;
    App& a = *g_app;
    if (!a.canvasHovered) return;
    if (dy > 0.0) actZoomAtCursor(a, 0.88);
    else if (dy < 0.0) actZoomAtCursor(a, 1.0 / 0.88);
}

}  // namespace

void installCallbacks(GLFWwindow* window, App& a) {
    g_app = &a;
    glfwSetMouseButtonCallback(window, onMouseButton);
    glfwSetCursorPosCallback(window, onCursorPos);
    glfwSetScrollCallback(window, onScroll);
    // 键盘快捷键走 ImGui 的按键 API（handleShortcuts），这里不装 key 回调，
    // 免得两套键盘状态互相打架。
}

// ---------------------------------------------------------------------------
// 键盘
// ---------------------------------------------------------------------------

namespace {

bool ctrlDown() {
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyCtrl;
}
bool shiftDown() {
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyShift;
}

void cyclePalette(App& a, int delta) {
    const int n = builtinPaletteCount();
    if (n <= 0) return;
    a.palette.loadPreset(((a.palette.presetIndex() + delta) % n + n) % n);
    actSetPaletteColor(a, 0);
    a.lastAction = std::string("色板 ") + a.palette.presetName();
}

}  // namespace

void handleShortcuts(App& a) {
    ImGuiIO& io = ImGui::GetIO();

    // 有文本输入焦点时一律让路（否则在输入框里打 "P" 会切到铅笔）
    if (io.WantTextInput) return;

    const bool ctrl = ctrlDown();
    const bool shift = shiftDown();

    if (ctrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) { shift ? actRedo(a) : actUndo(a); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) { actRedo(a); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_S)) { actSavePng(a, shift); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_N)) { requestNewCanvasDialog(); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_0)) { actResetCamera(a); return; }
        // Ctrl+J = 新建图层（Photoshop 的惯例），Ctrl+Shift+E = 向下合并
        if (ImGui::IsKeyPressed(ImGuiKey_J)) { actAddLayer(a); return; }
        if (shift && ImGui::IsKeyPressed(ImGuiKey_E)) { actMergeDown(a); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_Equal)) { actZoomAtCursor(a, 1.0 / 1.25); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_Minus)) { actZoomAtCursor(a, 1.25); return; }
        return;
    }

    // 工具
    struct KeyTool { ImGuiKey key; ToolId tool; };
    static const KeyTool kToolKeys[] = {
        {ImGuiKey_P, ToolId::Pencil},  {ImGuiKey_L, ToolId::Line},
        {ImGuiKey_R, ToolId::Rect},    {ImGuiKey_E, ToolId::Ellipse},
        {ImGuiKey_F, ToolId::Fill},    {ImGuiKey_D, ToolId::Dither},
        {ImGuiKey_K, ToolId::Picker},  {ImGuiKey_X, ToolId::Eraser},
        {ImGuiKey_I, ToolId::Lighten}, {ImGuiKey_O, ToolId::Darken},
    };
    for (const auto& kt : kToolKeys) {
        if (ImGui::IsKeyPressed(kt.key)) {
            actSetTool(a, kt.tool);
            a.lastAction = std::string("工具 ") + toolName(kt.tool);
            return;
        }
    }

    // 笔刷
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) {
        a.opts.brushSize = std::max(1, a.opts.brushSize - 1);
        a.lastAction = "笔刷 " + std::to_string(a.opts.brushSize);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) {
        a.opts.brushSize = std::min(64, a.opts.brushSize + 1);
        a.lastAction = "笔刷 " + std::to_string(a.opts.brushSize);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_B)) {
        a.opts.brushRound = !a.opts.brushRound;
        a.lastAction = a.opts.brushRound ? "圆形笔刷" : "方形笔刷";
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_G)) {
        a.showPixelGrid = !a.showPixelGrid;
        a.lastAction = a.showPixelGrid ? "像素网格 开" : "像素网格 关";
        return;
    }

    // 色板
    if (ImGui::IsKeyPressed(ImGuiKey_Comma)) { cyclePalette(a, -1); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Period)) { cyclePalette(a, +1); return; }
    for (int i = 0; i < 9; i++) {
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i))) {
            actSetPaletteColor(a, i);
            return;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_0)) { actSetPaletteColor(a, 9); return; }

    // 光照
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        a.lightOn = !a.lightOn;
        if (a.lightOn) a.lightDirty = true;
        a.lastAction = a.lightOn ? "光照 开" : "光照 关";
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) a.requestQuit = true;
}

void simulateStroke(App& a, int x0, int y0, int x1, int y1) {
    beginStrokeAt(a, x0, y0);
    if (!a.stroking) {
        printf("[sim] 没进入描边状态（图层被锁定/隐藏？）\n");
        return;
    }
    // 分几步拖过去，模拟真实拖动（形状工具靠这一步预览）
    const int steps = 8;
    for (int i = 1; i <= steps; i++) {
        const int x = x0 + (x1 - x0) * i / steps;
        const int y = y0 + (y1 - y0) * i / steps;
        a.session.update(a.doc.active(), a.opts, x, y);
    }
    endStroke(a);
}

void handleNavigation(App& a) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        a.spaceDown = false;
        return;
    }

    a.spaceDown = ImGui::IsKeyDown(ImGuiKey_Space);

    if (!a.vp) return;
    const double step = 12.0 / std::max(1.0, a.vp->pixelsPerCell());
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) a.vp->panByUV(-step, 0.0);
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) a.vp->panByUV(step, 0.0);
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) a.vp->panByUV(0.0, step);
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) a.vp->panByUV(0.0, -step);
}

}  // namespace pb::app
