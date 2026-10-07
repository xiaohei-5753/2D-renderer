// ui/widgets.cpp
#include "widgets.h"

#include <algorithm>

namespace pb::ui {

bool verticalSplitter(const char* id, float* width, float minW, float maxW, float height,
                      float dir) {
    const float thickness = 6.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton(id, ImVec2(thickness, height));
    ImGui::PopStyleVar();

    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    // 悬停时把光标换成左右箭头，否则用户不知道这里能拖
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if (active && width) {
        *width += dir * ImGui::GetIO().MouseDelta.x;
        *width = std::clamp(*width, minW, maxW);
    }

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImU32 col = IM_COL32(38, 41, 48, 255);
    if (active) col = IM_COL32(96, 168, 240, 255);
    else if (hovered) col = IM_COL32(70, 100, 140, 255);
    ImGui::GetWindowDrawList()->AddRectFilled(mn, mx, col);

    return active;
}

bool colorSwatch(const char* id, ImU32 col, ImVec2 size, bool selected,
                 const char* tooltip) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##sw", size);
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);

    // 棋盘底：让半透明颜色也能被看清（画板里 alpha 是有意义的）
    const float cell = 4.0f;
    for (float y = 0; y < size.y; y += cell) {
        for (float x = 0; x < size.x; x += cell) {
            const bool odd = ((int)(x / cell) + (int)(y / cell)) & 1;
            dl->AddRectFilled(ImVec2(p0.x + x, p0.y + y),
                              ImVec2(std::min(p0.x + x + cell, p1.x),
                                     std::min(p0.y + y + cell, p1.y)),
                              odd ? IM_COL32(60, 60, 64, 255) : IM_COL32(90, 90, 96, 255));
        }
    }
    dl->AddRectFilled(p0, p1, col);

    if (selected) {
        dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2),
                    IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
    } else {
        // 未选中用**中灰**描边（不是近黑）：很多调色板（NES、Game Boy）里有纯黑与深色，
        // 深色描边会跟深色块本身糊在一起，看不出色块的边界在哪。
        const ImU32 border = hovered ? IM_COL32(235, 240, 250, 255) : IM_COL32(120, 124, 134, 255);
        dl->AddRect(p0, p1, border);
    }

    if (hovered && tooltip && tooltip[0]) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool toolButton(const char* id, const char* label, bool selected, const char* tooltip,
                ImVec2 size) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.78f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.58f, 0.86f, 1.00f));
    }
    ImGui::PushID(id);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopID();
    if (selected) ImGui::PopStyleColor(2);

    if (ImGui::IsItemHovered() && tooltip && tooltip[0]) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

void statusDot(ImU32 col, const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = ImGui::GetTextLineHeight() * 0.28f;
    const float cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
    dl->AddCircleFilled(ImVec2(p.x + r, cy), r, col);
    ImGui::Dummy(ImVec2(r * 2.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextUnformatted(label);
    (void)dl;
}

// ---------------------------------------------------------------------------
// 带独立标签的滑块
// ---------------------------------------------------------------------------
//
// ⚠️ 这类滑块有一个反复踩到的坑，值得单独写成一个部件：
//
// ImGui 的 SliderInt/SliderFloat 把 format 里的标签画在**滑块正中间**。它不会
// 换行、也不会省略，宽度不够时是**直接裁掉**的。右侧面板在 DPI 缩放之后本来就不宽
// （默认 322 逻辑像素），于是"图层不透明度 255"这种中文标签会被切掉半截 ——
// 看起来像渲染坏了，其实是布局问题。
//
// 解决办法很简单：标签自己占一行，滑块不显示标签。代价是多占一行高度，
// 但在这种参数面板里可读性远比紧凑重要。

bool labeledSliderInt(const char* id, int* v, int lo, int hi, const char* label,
                      const char* fmt, float fullWidth) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(fullWidth);
    return ImGui::SliderInt(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}

bool labeledSliderFloat(const char* id, float* v, float lo, float hi, const char* label,
                        const char* fmt, float fullWidth) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(fullWidth);
    return ImGui::SliderFloat(id, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}

}  // namespace pb::ui
