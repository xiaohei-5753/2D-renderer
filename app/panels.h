// app/panels.h — 画板的各个 UI 面板
//
// 每个面板一个函数，按"一一实现"的节奏逐个填内容。
#pragma once

namespace pb::app {

struct App;

// 构建整帧 UI（菜单栏 + 工作区三栏 + 状态栏）。
// 必须在 ImGui::NewFrame() 之后、ImGui::Render() 之前调用，
// 且画布的裸 GL 绘制要排在这中间。
void drawUi(App& a);

// --- 面板 ---
void drawMainMenuBar(App& a);       // 顶部菜单栏
void drawToolbarPanel(App& a);      // 左栏：工具 + 工具选项 + 前景/背景色
void drawCanvasPanel(App& a);       // 中栏：画布（内容由 GL 直接绘制），这里只给矩形 + 叠加层
void drawPropertiesPanel(App& a);   // 右栏：调色板 / 材质 / 光照
void drawStatusBar(App& a);         // 底部状态栏

// 右栏的三个可折叠分区（逐步实现）
void drawPaletteSection(App& a);
void drawMaterialSection(App& a);
void drawLightingSection(App& a);

// --- 动作（菜单与工具栏共用）---
void actUndo(App& a);
void actRedo(App& a);
void actClearCanvas(App& a);
void actSavePng(App& a, bool lit);
void actNewCanvas(App& a, int w, int h);
void actZoomAtCursor(App& a, double factor);
void actSetTool(App& a, ToolId t);
void actSetPaletteColor(App& a, int index);
void actResetCamera(App& a);

// 请求打开"新建画布"对话框（快捷键与菜单共用同一个入口）
void requestNewCanvasDialog();

}  // namespace pb::app
