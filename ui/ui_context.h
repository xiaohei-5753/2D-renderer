// ui/ui_context.h — Dear ImGui 的生命周期、字体与主题
//
// 这一层是通用 ImGui 管线，**不认识画板**（不知道 Canvas / 工具 / 材质）。
// 画板自己的面板放在 app/panels.cpp，通过这里的 API 使用 ImGui。
//
// 为什么不用 Docking：本项目内置的 ImGui 是 v1.92.9b 官方发布版，
// 而 docking / multi-viewport 只存在于单独的 docking 分支，发布版里连
// ImGuiConfigFlags_DockingEnable 都不存在。所以布局由 app 侧手工划分。
#pragma once

#include <string>

struct GLFWwindow;

namespace pb::ui {

struct UiConfig {
    // unifont 是 16×16 点阵风字体，用它的整数倍大小能得到锐利的像素字。
    // 实际加载尺寸 = fontPixelSize * DPI 缩放。
    float fontPixelSize = 16.0f;
    std::string fontPath = "unifont-17.0.04.otf";
    // 全量 CJK（约 21000 字）。1.92 起字形是按需光栅化 + 增量上传的，
    // 所以不必预烘一个巨大的图集，全量中文是可行的。
    bool fullCjk = true;
    std::string iniPath = "paintboard_layout.ini";
};

class UiContext {
public:
    UiContext() = default;
    ~UiContext();

    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    // 必须在宿主自己的 GLFW 回调**之后**调用。
    //
    // ImGui 的 GLFW 后端会保存并链式调用先前安装的回调（源码里的 PrevUserCallback*
    // 机制），所以顺序是：先装宿主的回调，再让 ImGui 接管。宿主的回调里再用
    // wantsMouse()/wantsKeyboard() 决定要不要响应画布输入。
    bool init(GLFWwindow* window, const UiConfig& cfg = UiConfig{});
    void shutdown();
    bool valid() const { return inited_; }

    // 一帧：NewFrame（平台 + 渲染后端 + ImGui）
    void beginFrame();
    // 一帧结束：ImGui::Render + 后端绘制。会保存/恢复 GL 状态，
    // 所以要放在"用裸 GL 画的画布"之后。
    void render();

    bool wantsMouse() const;
    bool wantsKeyboard() const;

    float dpiScale() const { return dpiScale_; }
    bool fontLoaded() const { return fontLoaded_; }
    const std::string& fontPathUsed() const { return fontPathUsed_; }

    // 字体加载失败时的说明（含已尝试过的路径）。非空表示界面中文会显示成 '?'，
    // 界面必须把这件事**显示出来**——双击启动时没有控制台，用户只会看到满屏问号。
    const std::string& fontWarning() const { return fontWarning_; }

private:
    void applyTheme(float scale);
    bool loadFont(const UiConfig& cfg, float scale);

    bool inited_ = false;
    bool backendsInited_ = false;
    float dpiScale_ = 1.0f;
    bool fontLoaded_ = false;
    std::string fontPathUsed_;
    std::string fontWarning_;
};

}  // namespace pb::ui
