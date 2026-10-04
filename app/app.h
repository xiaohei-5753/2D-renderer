// app/app.h — 画板应用状态
//
// 把所有可变状态集中在一个结构里，面板（app/panels.cpp）直接读写它。
// 这样"一一实现各面板"时不需要在各处传参，也不会出现状态散落。
#pragma once

#include <string>

#include "core/canvas.h"
#include "core/history.h"
#include "core/palette.h"
#include "core/tool.h"
#include "core/types.h"
#include "gfx/gl_context.h"
#include "gfx/light_sweep.h"
#include "gfx/viewport.h"

namespace pb::app {

// 照明模式：实时扫描 GI / 渐进式逐像素光追（后者在 P2 实现）
enum class LightMode { Sweep, PathTrace };

const char* lightModeName(LightMode m);

struct App {
    // --- 文档与编辑 ---
    Canvas canvas;
    History history;
    Palette palette;
    ToolOptions opts;
    ToolSession session;

    // --- 渲染对象（由 main 填充，不持有所有权）---
    gfx::GLContext* ctx = nullptr;
    gfx::LightSweep* sweep = nullptr;
    gfx::Viewport* vp = nullptr;

    // --- 面板布局（可拖拽调宽 / 折叠）---
    //
    // 注意：ImGui 的坐标是"物理像素"（本程序 DPI-aware，窗口尺寸与帧缓冲一致），
    // 而字体与 style 已经按 uiScale 放大过。所以布局里所有**固定像素**的尺寸
    // 都必须乘 uiScale，否则会出现"字比面板宽"的溢出（已实测踩过）。
    float uiScale = 1.0f;
    float ui(float v) const { return v * uiScale; }

    float leftWidth = 236.0f;
    float rightWidth = 322.0f;
    float statusHeight = 26.0f;
    bool leftOpen = true;
    bool rightOpen = true;

    // --- 光照 ---
    bool lightOn = true;
    LightMode lightMode = LightMode::Sweep;
    bool lightDirty = true;
    float lightMs = 0.0f;

    // --- 视图选项 ---
    bool showPixelGrid = false;

    // --- 画布视口矩形（逻辑坐标，左上原点），由画布面板每帧写入 ---
    float canvasX = 0.0f, canvasY = 0.0f, canvasW = 0.0f, canvasH = 0.0f;
    bool canvasHovered = false;
    bool canvasVisible = false;

    // --- 鼠标状态（逻辑坐标）---
    double mx = 0.0, my = 0.0;
    bool lmbDown = false, rmbDown = false;
    bool stroking = false, panning = false, spaceDown = false;
    ToolId savedTool = ToolId::Pencil;

    // --- 状态栏用的光标信息 ---
    int cursorX = 0, cursorY = 0;
    RGBA8 cursorColor{0, 0, 0, 0};
    Material cursorMaterial{};

    // --- 统计 ---
    int saveCounter = 0;
    double fps = 0.0;

    // --- 最近使用的颜色 ---
    // 调色板本身只存"某一套色板"，最近使用是跨色板的编辑历史，所以放在 App 里。
    static constexpr int kRecentMax = 18;
    RGBA8 recent[kRecentMax]{};
    int recentCount = 0;

    // --- 自动化 / 演示模式 ---
    bool demo = false;
    bool requestQuit = false;
    std::string lastAction;   // 状态栏短暂显示的提示

    // 字体加载失败时的说明（由 main 从 UiContext 填入）。
    // 界面必须把它显示出来：双击启动没有控制台，否则用户只看到满屏 '?'。
    std::string fontWarning;
    bool fontWarningShown = false;
};

}  // namespace pb::app
