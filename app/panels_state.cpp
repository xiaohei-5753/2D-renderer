// app/panels_state.cpp — 分区折叠状态与标题栏矩形捕获
//
// 这些状态被 panels_properties.cpp（提交分区）、panels_dialogs.cpp 与 app/main.cpp
// （自动点击验证）共同使用，所以单独出来一份。设计理由见 panels_internal.h。
#include <algorithm>

#include "app/panels_internal.h"

namespace pb::app {

namespace {

// 这一轮是否已经强制过 name（强制只能施加一次，见 panels_internal.h 的说明）
bool alreadyForced(const std::string& n) {
    std::vector<std::string>& v = panelUi().forcedOnce;
    return std::find(v.begin(), v.end(), n) != v.end();
}

bool wantedOpen(const std::string& n, bool defaultOpen) {
    const PanelUiState& s = panelUi();
    if (!s.forceOpen.empty() && s.forceOpen.find(n) != std::string::npos) return true;
    if (!s.forceClosed.empty() && s.forceClosed.find(n) != std::string::npos) return false;
    return defaultOpen;
}

}  // namespace

bool sectionHeader(const char* name, bool defaultOpen) {
    PanelUiState& s = panelUi();
    const std::string n = name;
    if (!alreadyForced(n)) {
        s.forcedOnce.push_back(n);
        const bool forced = wantedOpen(n, defaultOpen);
        // 只有用户显式传了 --open / --close（forced 与默认值不一致）时才需要
        // 主动设一次初始状态，而且只设一次。
        if (forced != defaultOpen) ImGui::SetNextItemOpen(forced, ImGuiCond_Once);
    }
    // 默认展开交给 ImGuiTreeNodeFlags_DefaultOpen：它不会覆盖用户的选择
    return defaultOpen;
}

void recordHeader(const char* name, bool open) {
    PanelHeaderRect h;
    h.name = name;
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    h.x0 = mn.x;
    h.y0 = mn.y;
    h.x1 = mx.x;
    h.y1 = mx.y;
    h.open = open;
    panelUi().headers.push_back(h);
}

// ---------------------------------------------------------------------------
// 对外入口（见 panels.h 的说明）
// ---------------------------------------------------------------------------

void beginPanelHeaderCapture() { panelUi().headers.clear(); }

// ⚠️ 关键：鼠标位置必须在 **ImGui::NewFrame() 之后**注入。
// GLFW 后端在 NewFrame 里会发现鼠标不在窗口上（自动化跑的是隐藏窗口），
// 于是拿真实光标位置去 AddMousePosEvent —— 把我们在 NewFrame 之前设的位置**冲掉**。
// 这也正是第一版脚本点击"派发了但 hovered=0"的原因。
bool panelSectionPoint(const char* name, float& cx, float& cy) {
    for (const PanelHeaderRect& h : panelUi().headers) {
        if (h.name != name) continue;
        cx = (h.x0 + h.x1) * 0.5f;
        cy = (h.y0 + h.y1) * 0.5f;
        return true;
    }
    return false;
}

bool panelSectionOpen(const char* name) {
    for (const PanelHeaderRect& h : panelUi().headers)
        if (h.name == name) return h.open;
    return false;
}

void setUiSections(const std::string& forceOpen, const std::string& forceClosed) {
    PanelUiState& s = panelUi();
    s.forceOpen = forceOpen;
    s.forceClosed = forceClosed;
    s.forcedOnce.clear();   // 新的一轮强制
}

}  // namespace pb::app
