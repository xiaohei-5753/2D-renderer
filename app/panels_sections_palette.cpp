// app/panels_sections_palette.cpp — 右栏「调色板」分区
//
// 色板项 = **7 维材质**（RGB + 自发光 RGB + alpha）。核心约定：
// **选一个色板项 = 同时选定这一笔的颜色、自发光、透明度。**
// 没有第二套并行的笔刷属性；反向也成立（改「材质」面板、用取色器 K 都会写回
// 当前色板项）。改这块时别破坏这个约定，详见 HANDOVER.md §3。
#include <algorithm>
#include <cstdio>
#include <string>

#include "app/panels_internal.h"
#include "core/palette.h"
#include "ui/widgets.h"

namespace pb::app {

void drawPaletteSection(App& a) {
    // ------------------------------------------------------------------
    // 1. 预设下拉
    // ------------------------------------------------------------------
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##preset", a.palette.presetName())) {
        for (int i = 0; i < builtinPaletteCount(); i++) {
            const PalettePreset& p = builtinPalette(i);
            const bool selected = (i == a.palette.presetIndex());
            char label[96];
            std::snprintf(label, sizeof(label), "%s  (%d 色)", p.name, p.count);
            if (ImGui::Selectable(label, selected)) {
                a.palette.loadPreset(i);
                a.palette.resetMats();
                actSetPaletteColor(a, 0);
                a.lastAction = std::string("色板 ") + a.palette.presetName();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    const int n = a.palette.size();

    // ------------------------------------------------------------------
    // 2. 色块网格（按面板宽度自动换行，NES 的 55 色也排得下）
    // ------------------------------------------------------------------
    const float sw = a.ui(22.0f);
    const float gap = a.ui(3.0f);
    const float availW = ImGui::GetContentRegionAvail().x;
    const int perRow = std::max(1, (int)((availW + gap) / (sw + gap)));
    const int rows = (n + perRow - 1) / perRow;
    const float gridH = (float)rows * (sw + gap);
    const float maxGridH = a.ui(210.0f);

    // 留 2px 内边距：选中环是画在色块外面的，不留边会被 child 的裁剪区切掉
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(a.ui(3.0f), a.ui(3.0f)));
    ImGui::BeginChild("##swatches", ImVec2(0.0f, std::min(gridH + a.ui(6.0f), maxGridH)),
                      ImGuiChildFlags_None);
    for (int i = 0; i < n; i++) {
        const RGBA8 c = a.palette.color(i);
        const Palette::Mat7 m = a.palette.mat(i);
        const int em = std::max(m.eR, std::max(m.eG, m.eB));
        // tooltip 直接把 7 个分量都列出来，用户不用猜这一项带什么材质
        char tip[128];
        std::snprintf(tip, sizeof(tip), "#%02X%02X%02X   a=%d   发光=(%d,%d,%d)", c.r, c.g,
                      c.b, m.a, m.eR, m.eG, m.eB);
        ImGui::PushID(i);
        // 色块按**真实 alpha** 画：半透明的项一眼看出来
        if (ui::colorSwatch("sw", IM_COL32(c.r, c.g, c.b, m.a), ImVec2(sw, sw),
                            i == a.palette.currentIndex(), tip)) {
            actSetPaletteColor(a, i);
        }
        // 发光标记：右上角一个小圆点，亮度越高越亮
        if (em > 0) {
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            const float rr = (mx.x - mn.x) * 0.17f;
            const ImVec2 ctr(mx.x - rr * 1.5f, mn.y + rr * 1.5f);
            const int gv = std::min(255, 110 + em / 2);
            ImGui::GetWindowDrawList()->AddCircleFilled(ctr, rr, IM_COL32(gv, gv, 255, 255));
            ImGui::GetWindowDrawList()->AddCircle(ctr, rr, IM_COL32(20, 20, 24, 220), 0,
                                                  a.ui(1.0f));
        }
        ImGui::PopID();
        if (((i + 1) % perRow) != 0 && i + 1 < n) ImGui::SameLine(0.0f, gap);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // ------------------------------------------------------------------
    // 3. 自由拾色 + 十六进制输入
    // ------------------------------------------------------------------
    // ColorEdit3 一次覆盖两个需求：它的颜色按钮弹出拾色器，而 DisplayHex 让它
    // 直接显示一个可编辑的 #RRGGBB 输入框。比另外手写一个十六进制输入框更不容易出错。
    ImGui::Spacing();
    float col[3] = {a.opts.color.r / 255.0f, a.opts.color.g / 255.0f,
                    a.opts.color.b / 255.0f};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::ColorEdit3("##freecolor", col,
                          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB |
                              ImGuiColorEditFlags_PickerHueBar)) {
        a.opts.color.r = (uint8_t)std::lround(col[0] * 255.0f);
        a.opts.color.g = (uint8_t)std::lround(col[1] * 255.0f);
        a.opts.color.b = (uint8_t)std::lround(col[2] * 255.0f);
        pushRecentColor(a, a.opts.color);
    }

    // ------------------------------------------------------------------
    // 4. 加入色板 / 删除选中色
    // ------------------------------------------------------------------
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
                     0.5f;
    if (ImGui::Button("加入色板", ImVec2(bw, 0.0f))) {
        // 色板条目约定为不透明（a=255）：a 是笔刷的遮挡属性，不该混进色板，
        // 否则同一个 RGB 会因为 alpha 不同被当成两种颜色。
        RGBA8 entry = a.opts.color;
        entry.a = 255;
        int idx = -1;
        for (int i = 0; i < n; i++) {
            if (a.palette.color(i) == entry) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            idx = a.palette.addColor(entry);
            a.lastAction = "已加入色板";
        } else {
            a.palette.setCurrent(idx);
            a.lastAction = "该颜色已在色板中";
        }
        actSetPaletteColor(a, idx);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(a.palette.size() <= 1);
    if (ImGui::Button("删除选中色", ImVec2(bw, 0.0f))) {
        if (a.palette.removeColor(a.palette.currentIndex())) {
            a.lastAction = "已从色板删除";
            actSetPaletteColor(a, a.palette.currentIndex());
        }
    }
    ImGui::EndDisabled();

    // ------------------------------------------------------------------
    // 5. 最近使用的颜色（跨色板的编辑历史，所以存在 App 而不是 Palette 里）
    // ------------------------------------------------------------------
    if (a.recentCount > 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("最近使用");
        const float rsw = a.ui(18.0f);
        const float rgap = a.ui(3.0f);
        const int rPerRow = std::max(1, (int)((availW + rgap) / (rsw + rgap)));
        for (int i = 0; i < a.recentCount; i++) {
            const RGBA8 c = a.recent[i];
            char tip[80];
            std::snprintf(tip, sizeof(tip), "#%02X%02X%02X  alpha %d", c.r, c.g, c.b, c.a);
            ImGui::PushID(1000 + i);
            if (ui::colorSwatch("rc", IM_COL32(c.r, c.g, c.b, 255), ImVec2(rsw, rsw),
                                false, tip)) {
                a.opts.color = c;   // 完整恢复（含 alpha）
                pushRecentColor(a, c);
            }
            ImGui::PopID();
            if (((i + 1) % rPerRow) != 0 && i + 1 < a.recentCount)
                ImGui::SameLine(0.0f, rgap);
        }
    }
}

}  // namespace pb::app