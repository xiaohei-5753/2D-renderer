// ui/widgets.h — 通用 ImGui 小部件
//
// 只放 ImGui 本身没有、而画板界面反复要用的东西。这里同样不认识画板。
#pragma once

#include "imgui.h"

namespace pb::ui {

// 竖直分隔条：拖拽调整 *width（像素）。
//
// `dir` 决定拖拽方向与宽度变化的关系：分隔条在面板**右边**时传 +1（右移变宽），
// 在面板**左边**时传 -1（右移变窄）。返回是否正在被拖拽。
bool verticalSplitter(const char* id, float* width, float minW, float maxW, float height,
                      float dir = 1.0f);

// 纯色色块按钮（比 ImGui 自带的 ColorButton 更好控制尺寸与选中框）
bool colorSwatch(const char* id, ImU32 col, ImVec2 size, bool selected,
                 const char* tooltip = nullptr);

// 带选中态的工具按钮
bool toolButton(const char* id, const char* label, bool selected, const char* tooltip,
                ImVec2 size);

// 小圆点指示灯（用于状态栏：光照模式、累积状态等）
void statusDot(ImU32 col, const char* label);

}  // namespace pb::ui
