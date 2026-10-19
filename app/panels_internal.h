// app/panels_internal.h — 面板实现之间共享的状态与工具
//
// 这一层**只在 app/panels*.cpp 之间使用**，不是对外接口；对外接口仍然是
// app/panels.h。把 panels.cpp 按"一个分区一个文件"拆开之后，仍然有几样东西是
// 跨文件共享的，全部集中在这里：
//
//   * 分区折叠状态（--open / --close 的强制展开、标题栏矩形捕获）
//   * 几个通用小工具（toU32 / pushRecentColor 已经在 panels.h 里）
//
// 为什么要拆：原 panels.cpp 是 1434 行，占了 app/ 的一半，且里面是 6 个互不
// 相干的 section。按函数边界拆开之后每个文件 200~400 行，"改调色板"不再需要
// 在读"光照面板"的代码时保持上下文。
#pragma once

#include <string>
#include <vector>

#include "app/app.h"
#include "app/panels.h"
#include "imgui.h"

namespace pb::app {

// RGBA8 -> ImGui 的 32 位色。forceA 用来覆盖 alpha（画 UI 用的色块通常按
// 真实 alpha 画，但状态栏的小色点希望永远不透明）。
inline ImU32 toU32(RGBA8 c, uint8_t forceA = 255) {
    return IM_COL32(c.r, c.g, c.b, forceA);
}

// ---------------------------------------------------------------------------
// 分区折叠状态
// ---------------------------------------------------------------------------
//
// ⚠️ 这里踩过一个很难看的坑：第一版为了让「材质」默认折叠，每帧都调
//     ImGui::SetNextItemOpen(open, ImGuiCond_Always);
// 结果**所有分区都点不动了** —— Always 的含义是"每一帧都把状态设成这个值"，
// 用户点一下展开，下一帧立刻被按回去，表现就是点击完全没反应。
// 正确做法是**不主动设置状态**，改用 ImGui 自己的 ImGuiTreeNodeFlags_DefaultOpen
// （它只在"用户还没做过选择"时生效）；`--open` / `--close` 也只在第一次提交那个
// 分区时施加一次。
//
// 顺带说清另一个写法上的坑：`if (ImGui::CollapsingHeader(...))` 里
// CollapsingHeader **每一帧都必须调用**（它的返回值才是展开状态）。写成
// `if (open) CollapsingHeader(...)` 会让折叠后的控件再也不被提交，状态永远翻不回来。
struct PanelHeaderRect {
    std::string name;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool open = false;
};

struct PanelUiState {
    // 自动化截图用：强制展开 / 折叠右侧栏的分区（按名字子串匹配）。
    // ImGui 的折叠状态存在 .ini 里，功能一多右侧栏就长到要滚动，于是"我改了图层
    // 面板"这件事在截图里可能根本看不到（被挤到屏幕外了）。有了这个开关，
    // `--frames N --out x.png --open 图层` 就能把要验证的那一块稳定地放进画面。
    // 留空 = 用 .ini 里的用户状态（正常交互时的行为）。
    std::string forceOpen;
    std::string forceClosed;
    // 强制状态每轮只施加一次（见上面的坑）
    std::vector<std::string> forcedOnce;

    // 每个分区标题栏的屏幕矩形（配合自动点击验证）。
    // 记录它几乎不花代价，却换来"折叠交互可以被自动化证明"。
    std::vector<PanelHeaderRect> headers;
};

inline PanelUiState& panelUi() {
    static PanelUiState s;
    return s;
}

// 分区标题栏的提交。返回"默认是否展开"，调用方拿去当 CollapsingHeader 的 flags：
//
//     const bool open = ImGui::CollapsingHeader(
//         "调色板", sectionHeader("调色板", true) ? ImGuiTreeNodeFlags_DefaultOpen : 0);
//     if (open) drawPaletteSection(a);
//     recordHeader("调色板", open);
bool sectionHeader(const char* name, bool defaultOpen);

// 记录上一次 CollapsingHeader 的矩形（必须在它之后立刻调用）。
// 展开状态由调用方传入 —— ImGui 没有"查询上一个 Tree 节点是否展开"的公开 API，
// 但 CollapsingHeader 的返回值本身就是它。
void recordHeader(const char* name, bool open);

}  // namespace pb::app
