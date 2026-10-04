// gfx/gl_context.h — OpenGL 上下文
//
// 这一层是唯一直接接触 GLFW 的地方。gfx 的渲染模块（light_sweep 等）只接收
// 已经创建好的上下文，不自己建窗口，因此游戏侧可以把自己的窗口/上下文传进来复用。
//
// 离线渲染走的是 GLFW_VISIBLE=false 的隐藏窗口，同一套代码路径无需任何改动。
#pragma once

#include <string>

struct GLFWwindow;

namespace pb::gfx {

struct ContextDesc {
    int width = 1280;
    int height = 720;
    bool visible = false;          // 离线渲染置 false
    bool resizable = true;
    const char* title = "paintboard";
    int glMajor = 4;
    int glMinor = 3;
};

class GLContext {
public:
    GLContext() = default;
    ~GLContext();

    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;

    // 创建窗口与 GL 上下文并使其成为当前上下文。失败时返回 false 并打印诊断。
    bool create(const ContextDesc& desc);
    void destroy();

    bool valid() const { return window_ != nullptr; }
    GLFWwindow* window() const { return window_; }

    void makeCurrent();
    void swapBuffers();
    void pollEvents();
    bool shouldClose() const;
    void requestClose();

    // 帧缓冲尺寸（像素，已含 DPI 缩放），窗口最小化时可能为 0
    void framebufferSize(int& w, int& h) const;
    // 窗口尺寸（屏幕坐标）
    void windowSize(int& w, int& h) const;

    void setTitle(const std::string& t);

    const std::string& glVersion() const { return glVersion_; }
    const std::string& glRenderer() const { return glRenderer_; }

private:
    GLFWwindow* window_ = nullptr;
    bool ownsGlfw_ = false;
    std::string glVersion_, glRenderer_;
};

}  // namespace pb::gfx
