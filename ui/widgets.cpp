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
        ImU32 border = hovered ? IM_COL32(200, 210, 230, 255) : IM_COL32(0, 0, 0, 170);
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

}  // namespace pb::ui
