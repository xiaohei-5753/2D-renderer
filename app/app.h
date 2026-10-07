// app/app.h — 画板应用状态
//
// 把所有可变状态集中在一个结构里，面板（app/panels.cpp）直接读写它。
// 这样"一一实现各面板"时不需要在各处传参，也不会出现状态散落。
#pragma once

#include <string>
#include <vector>

#include "core/history.h"
#include "core/layer.h"
#include "core/palette.h"
#include "core/tool.h"
#include "core/types.h"
#include "gfx/gl_common.h"
#include "gfx/gl_context.h"
#include "gfx/light_sweep.h"
#include "gfx/path_trace.h"
#include "gfx/viewport.h"

namespace pb::app {

// 照明模式：实时扫描 GI / 渐进式逐像素光追（后者在 P2 实现）
enum class LightMode { Sweep, PathTrace, Trace25D };

const char* lightModeName(LightMode m);

// 图层面板用的缩略图资源。
//
// 缩略图必须缓存：每帧把一整层重新转成 RGBA32 再传 GPU，在 512×512 × 十几层下
// 是纯粹的浪费。靠 Layer::revision 判断"内容变没变"，变了才重建。
struct LayerThumb {
    GLuint tex = 0;
    int w = 0, h = 0;
    uint32_t revision = 0;
};

struct App {
    // --- 文档与编辑 ---
    //
    // Document 是真正的文档（图层栈），tools / History 都作用在**当前图层**上。
    //
    // ⚠️ 逐层光照之后**不再需要一份"压平后的合成缓冲"**：光照是逐层做的，
    // 最后由 GPU 的 LayerComposite 合成；CPU 侧的压平只在"关灯看原图"时用得上，
    // 那条路现在也走 GPU（把各层 albedo 数组合成）。所以 App 里没有 composite 成员。
    Document doc;
    History history;
    Palette palette;
    ToolOptions opts;
    ToolSession session;

    // 需要重新照亮哪些层（渲染层用）。空 = 本帧没有光照要做。
    // 由 syncLightDirty() 每帧填好；只重算真正变了的那几层，这是"实时"的关键：
    // 画一笔只会重算一笔所在的那一层，而不是整个图层栈。
    std::vector<uint8_t> lightDirtyLayers;
    // 文档结构/全局光照参数变了：所有层都要重算（层数变化、切模式、调参数）
    bool lightAllLayers = true;

    // --- 渲染对象（由 main 填充，不持有所有权）---
    gfx::GLContext* ctx = nullptr;
    gfx::LightSweep* sweep = nullptr;
    gfx::PathTrace* pt = nullptr;

    gfx::Viewport* vp = nullptr;

    // --- 图层面板 ---
    std::vector<LayerThumb> thumbs;
    bool layerPanelDirty = true;   // 请求重建缩略图（图层数变了 / 图层被改过）

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
    // --- 体素光追参数（渲染端设法的镜像，GUI 只负责摆控件）---
    float vzSunAz = 135.0f;        // 太阳方位角（度）
    float vzSunEl = 45.0f;         // 太阳仰角（度）
    float vzSunIntensity = 1.0f;
    float vzSky = 0.20f;           // 天空亮度
    float vzEmitScale = 1.0f;
    bool vzShadows = true;
    bool vzDirty = true;

    // --- 视图选项 ---
    bool showPixelGrid = false;
    // 首次拿到画布矩形后自动做一次整数倍缩放（像素画必须），之后不再干预用户
    bool cameraInitialized = false;

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
