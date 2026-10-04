// ui/ui_context.cpp
#include "ui_context.h"

#include <cmath>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

namespace pb::ui {

UiContext::~UiContext() { shutdown(); }

bool UiContext::init(GLFWwindow* window, const UiConfig& cfg) {
    if (inited_) return true;
    if (!window) {
        fprintf(stderr, "[ui] 需要一个 GLFWwindow\n");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    // 刻意**不**开启 ImGuiConfigFlags_NavEnableKeyboard：
    // 键盘导航会占用 Tab（切换焦点）与方向键（在控件间移动），而画板要用
    // Tab 开关光照、用方向键平移视图，两者会直接打架。
    io.IniFilename = cfg.iniPath.empty() ? nullptr : cfg.iniPath.c_str();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        fprintf(stderr, "[ui] ImGui_ImplGlfw_InitForOpenGL 失败\n");
        ImGui::DestroyContext();
        return false;
    }
    // GLSL 版本要和 GLContext 申请的 4.3 core 兼容；330 是 OpenGL3 后端支持的版本串
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        fprintf(stderr, "[ui] ImGui_ImplOpenGL3_Init 失败\n");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    backendsInited_ = true;

    // DPI 缩放：本程序是 DPI-aware 的，GLFW 的内容缩放反映了显示器 DPI 比例
    dpiScale_ = ImGui_ImplGlfw_GetContentScaleForWindow(window);
    if (!(dpiScale_ > 0.0f)) dpiScale_ = 1.0f;

    applyTheme(dpiScale_);
    loadFont(cfg, dpiScale_);

    inited_ = true;
    printf("[ui] ImGui %s 就绪，DPI 缩放 %.2f，字体 %s\n", IMGUI_VERSION, dpiScale_,
           fontLoaded_ ? fontPathUsed_.c_str() : "内置回退字体");
    return true;
}

void UiContext::shutdown() {
    if (!inited_ && !backendsInited_) return;
    if (backendsInited_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        backendsInited_ = false;
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    inited_ = false;
}

// ---------------------------------------------------------------------------
// 像素风主题
// ---------------------------------------------------------------------------
//
// 要点：**全部圆角归零**、边框 1px、间距紧凑、对比度高。
// 圆角是"现代扁平 UI"的特征，和像素画工具的气质冲突。

void UiContext::applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();

    // 先把尺寸按 DPI 放大（ScaleAllSizes 不缩放字体）
    s = ImGuiStyle();
    if (scale > 1.001f) s.ScaleAllSizes(scale);

    // 直角、细边、紧凑
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.PopupRounding = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.GrabRounding = 0.0f;
    s.TabRounding = 0.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;

    s.WindowPadding = ImVec2(6, 6);
    s.FramePadding = ImVec2(6, 3);
    s.CellPadding = ImVec2(4, 2);
    s.ItemSpacing = ImVec2(6, 4);
    s.ItemInnerSpacing = ImVec2(4, 4);
    s.IndentSpacing = 14.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 8.0f;

    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    // 暗色冷调 + 高对比：像素画的颜色容易被界面干扰，所以界面本身要够沉
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.47f, 0.52f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.090f, 0.106f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.106f, 0.110f, 0.129f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.106f, 0.110f, 0.129f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.29f, 0.35f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.39f, 0.45f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.44f, 0.48f, 0.55f, 1.00f);
    c[ImGuiCol_CheckMark] = ImVec4(0.36f, 0.72f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.36f, 0.62f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.46f, 0.74f, 1.00f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.31f, 0.37f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.50f, 0.78f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.24f, 0.30f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.32f, 0.40f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.44f, 0.66f, 1.00f);
    c[ImGuiCol_Separator] = ImVec4(0.24f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.36f, 0.52f, 0.70f, 1.00f);
    c[ImGuiCol_SeparatorActive] = ImVec4(0.44f, 0.66f, 0.90f, 1.00f);
    c[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.29f, 0.35f, 0.60f);
    c[ImGuiCol_Tab] = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.32f, 0.40f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.30f, 0.42f, 1.00f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.24f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.18f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.24f, 0.48f, 0.74f, 0.55f);
    c[ImGuiCol_NavCursor] = ImVec4(0.44f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.03f, 0.03f, 0.04f, 0.65f);
}

// ---------------------------------------------------------------------------
// 字体
// ---------------------------------------------------------------------------

bool UiContext::loadFont(const UiConfig& cfg, float scale) {
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig fc;
    // unifont 本身是点阵风字形，关掉过采样、开启水平像素对齐才能保持锐利；
    // 开着过采样反而会把它的点阵边缘糊掉。
    fc.OversampleH = 1;
    fc.OversampleV = 1;
    fc.PixelSnapH = true;

    // 尺寸按 DPI 缩放。
    //
    // 这里刻意**不**把尺寸对齐到 16 的整数倍来"追求锐利"：unifont 的设计尺寸是 16px，
    // 在 150% 缩放下 16*1.5 = 24px，如果向上取整到 32px，字号会比预期大 33%，
    // 直接导致状态栏和面板文字溢出（已实测踩过）。1.92 的动态图集本来就会按实际
    // 有效尺寸重新光栅化，24px 的质量是可接受的。
    const float size = std::max(9.0f, cfg.fontPixelSize * scale);

    // 1.92 起字形范围是 AddFontFromFileTTF 的参数（ImFontConfig::GlyphRanges 已标记 LEGACY）
    const ImWchar* ranges = nullptr;
    if (cfg.fullCjk) {
        ranges = io.Fonts->GetGlyphRangesChineseFull();
    } else {
        ranges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    }

    ImFont* f = io.Fonts->AddFontFromFileTTF(cfg.fontPath.c_str(), size, &fc, ranges);
    if (f) {
        fontLoaded_ = true;
        fontPathUsed_ = cfg.fontPath;
        return true;
    }

    // 回退：内置字体（不含中文），至少让程序能跑起来并报出原因
    fprintf(stderr, "[ui] 无法加载字体 '%s'（%s），回退到内置字体，中文将显示为方块\n",
            cfg.fontPath.c_str(), "确认工作目录下有该文件");
    io.Fonts->AddFontDefault();
    fontLoaded_ = false;
    fontPathUsed_.clear();
    return false;
}

// ---------------------------------------------------------------------------
// 帧循环
// ---------------------------------------------------------------------------

void UiContext::beginFrame() {
    if (!inited_) return;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void UiContext::render() {
    if (!inited_) return;
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool UiContext::wantsMouse() const {
    if (!inited_) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

bool UiContext::wantsKeyboard() const {
    if (!inited_) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
}

}  // namespace pb::ui
