// app/input.h — 输入处理
#pragma once

struct GLFWwindow;

namespace pb::app {

struct App;

// 安装 GLFW 回调。
//
// ⚠️ 必须在 ui::UiContext::init() **之前**调用：ImGui 的 GLFW 后端会保存并
// 链式调用先前安装的回调（源码里的 PrevUserCallback* 机制），所以顺序是
// 「先装宿主的，再让 ImGui 接管」，之后 ImGui 处理完自己的状态会回调我们。
void installCallbacks(GLFWwindow* window, App& a);

// 每帧处理键盘快捷键。必须在 ImGui::NewFrame() 之后调用。
// 内部会在有文本输入焦点时自动让路（用 io.WantTextInput 判断）。
void handleShortcuts(App& a);

// 每帧处理需要持续按住的键（方向键平移、空格作平移修饰）
void handleNavigation(App& a);

// 自动化验证用：走**和鼠标完全一样**的那条路画一笔
// （beginStrokeAt -> session.update -> endStroke），从 (x0,y0) 拖到 (x1,y1)。
//
// 为什么需要它：`--demo` 是在主循环**之前**直接调工具画的，所以它验证不了
// "在主循环里画一笔之后，这一笔会不会被光照看到" —— 而"画完什么都看不见"
// 恰恰就是这类问题。有了这个入口，这件事才能被自动复现。
void simulateStroke(App& a, int x0, int y0, int x1, int y1);

}  // namespace pb::app
