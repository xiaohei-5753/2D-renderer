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
#include "gfx/viewport.h"

namespace pb::app {

// 照明模式：只剩实时扫描 GI。
// 渐进式逐像素路径追踪已于 2026-10-19 整体移除（专注扫描 GI）。
enum class LightMode { Sweep };

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
    // 上层光场照亮下层的强度。**默认开**：
    // 每层的颜色 = 原始颜色 × (本层+上方各层的光场 + 环境光)，
    // 所以只有环境光时白纸是暗的（这正是期望行为）；0 = 退回旧的 mix 路径。
    float fieldStrength = 1.0f;
    bool lightDirty = true;
    float lightMs = 0.0f;
    // --- 渐进哈希采样（扫描 GI 的可选路径）---
    //
    // 开：每帧每像素只采一个方向、跨帧累积（零原子、单帧便宜、第 D 帧精确）。
    // 关：确定性扫描（每帧把所有方向扫一遍，单帧贵但无噪声）。
    bool progressive = false;
    // 分块 + shared 合并的扫描（x 主方向走列波前；y 主方向仍走主内核）
    bool tiledScan = false;
    // 光场降采样倍率（1=关）。光照是低频信息，只降光场、不降 albedo/自发光。
    int fieldScale = 1;
    // 渐进模式的去噪（à-trous 联合双边）。
    bool denoise = false;
    float denoiseStrength = 3.5f;
    int denoiseIters = 3;
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

    // --- 工程文件（.pbproj）---
    //
    // 一个工程文件装下"作品 + 界面 + 工具设置 + 光照参数"，见 core/project.h。
    // projectPath 为空表示"还没存过"，此时保存会先弹出路径对话框。
    std::string projectPath;
    // 工程打开/保存对话框请求（由菜单与快捷键设置，对话框自己消费）
    bool requestProjectSaveDialog = false;
    bool requestProjectOpenDialog = false;
    // 对话框当前是"打开"还是"保存"。必须在**弹窗存活期间**记住它：
    // ImGui 的 BeginPopupModal 在 OpenPopup 那一帧返回 false（下一帧才真出现），
    // 所以不能靠"请求标志还在不在"来判断模式 —— 请求标志会在第一次渲染时清掉。
    bool projectDialogIsOpen = false;
    // 弹窗当前是"打开"(true) 还是"保存"(false)。**锁存**下来的，
    // 不是每帧从请求标志重算 —— 见 drawProjectDialogs 里的注释（踩过的坑）。
    bool projectDialogModeOpen = false;
    // 对话框里的路径输入缓冲
    char projectPathBuf[512] = {0};

    // --- 基准场景（--bench-scene）---
    // benchFrame > 0 时每帧调用 app::bench::step() 驱动动画。
    // 这些图层**每帧全都变**，所以光照的脏层优化不会生效 —— 量到的是
    // "每帧把 N 层全部重算"的真实上限成本，这正是基准想要的最坏情况。
    int benchFrame = 0;
    bool hidePanels = false;   // --hide-ui：只画画布，把 UI 开销排除掉

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
