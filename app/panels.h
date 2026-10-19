// app/panels.h — 画板的各个 UI 面板（对外接口）
//
// ## 文件划分
//
// 原先这些都挤在 panels.cpp 里（1434 行，占 app/ 的一半），按"一个分区一个文件"
// 拆开之后，改某一块不必再通读其它五块：
//
//   panels.h                     本文件：对外接口
//   panels_internal.h            仅面板实现之间共享的状态（分区折叠 / 标题栏捕获）
//   panels_state.cpp             上述状态的实现 + setUiSections / 自动点击钩子
//   panels_actions.cpp           被菜单 / 快捷键 / 面板按钮共用的**动作**
//   panels_canvas.cpp            工作区三栏 + 左栏工具栏 + 中栏画布与叠加层
//   panels_menu.cpp              顶部主菜单栏 + 底部状态栏
//   panels_dialogs.cpp           三个模态对话框 + 字体缺失警告
//   panels_properties.cpp        右栏属性面板（提交四个可折叠分区）
//   panels_sections_*.cpp        调色板 / 材质 / 光照三个分区的内容
//   layer_panel.cpp              图层面板（自带缩略图缓存）
#pragma once

#include <string>

#include "core/tool.h"   // ToolId（在 pb 里，不在 pb::app 里）
#include "core/types.h"

namespace pb::app {

struct App;

// 构建整帧 UI（菜单栏 + 工作区三栏 + 状态栏 + 对话框）。
// 必须在 ImGui::NewFrame() 之后、ImGui::Render() 之前调用，
// 且画布的裸 GL 绘制要排在这中间。
void drawUi(App& a);

// --- 面板 ---
void drawMainMenuBar(App& a);       // 顶部菜单栏
void drawToolbarPanel(App& a);      // 左栏：工具 + 工具选项 + 前景/背景色
void drawCanvasPanel(App& a);       // 中栏：画布（内容由 GL 直接绘制），这里只给矩形 + 叠加层
void drawPropertiesPanel(App& a);   // 右栏：调色板 / 材质 / 光照 / 图层
void drawStatusBar(App& a);         // 底部状态栏
void drawDialogs(App& a);           // 模态对话框与警告（由 drawUi 末尾调用）

// 右栏的四个可折叠分区
void drawPaletteSection(App& a);
void drawMaterialSection(App& a);
void drawLightingSection(App& a);
// 图层面板实现在 app/layer_panel.cpp：它自带缩略图缓存，独立成一文件
// 免得 panels 系列继续膨胀
void drawLayerSection(App& a);

// --- 图层动作（实现在 app/layer_actions.cpp）---
//
// 菜单栏、快捷键、图层面板三个入口共用同一套实现，避免"同一个动作三份代码"。
// 这些是**结构性操作**：都会改变"哪些层参与合成"，所以统一走 History::beginAll/commit
// （整份文档快照），而不是像一笔画那样只看当前图层。
void actAddLayer(App& a);
void actDuplicateLayer(App& a);
void actDeleteLayer(App& a);
void actMergeDown(App& a);
void actMoveLayer(App& a, int delta);          // delta = +1 上移 / -1 下移
void actToggleLayerVisible(App& a, int index);

// 图层的非像素属性被改动（不透明度 / 锁定 / 改名）后调用。
// 不记入撤销（一眼就能看出效果的即时开关，撤销一个眼睛图标没有意义），
// 但必须让合成与光照重算。
void notifyLayerPropertiesChanged(App& a);

// 缩略图缓存：让 a.thumbs 与图层数一致，并按 Layer::revision 重建内容。
// 每帧在 drawUi 之后调用一次（它要访问 GL，所以不在面板函数里）。
void uploadLayerThumbnails(App& a);

// --- 逐层光照（app/lighting.cpp）---
//
// 逐层光场模型在应用侧的落地：每层各自上传 albedo / 材质，跑一遍平面内光照，
// 最后由 GPU 的 LayerComposite 按"光场照亮下层"合成。设计见 README 的「光照模型」。
//
// 每帧的调用顺序：
//   syncLightDirty(a)  -> 算出"哪几层变了"（只重算变了的层，这是实时的关键）
//   runLighting(a)     -> 上传 + 逐层照亮 + 合成
void syncLightDirty(App& a);
// 逐层照亮 + 合成。返回"这一帧真的有光照要算"（供 GPU 计时判断，
// 没有脏层时它就直接返回，计时器不该被触发）
bool runLighting(App& a);

// 文档结构 / 全局光照参数变了：所有层都要重算
void markAllLayersForLight(App& a);
// 文档被整个换掉（新建画布）时清掉缓存
void resetLightingCache();
bool anyLayerDirty(const App& a);
float opacityOf(const App& a, int index);
int lightingLayerCap();

// --- 动作（菜单 / 快捷键 / 面板共用，实现在 app/panels_actions.cpp）---
void actUndo(App& a);
void actRedo(App& a);
void actClearCanvas(App& a);
void actSavePng(App& a, bool lit);
void actNewCanvas(App& a, int w, int h);
// 工程文件（.pbproj）：一次存下作品 + 界面 + 工具设置 + 光照参数。
// 路径为空时会拉起路径对话框；actLoadProject 直接读指定路径。
bool actSaveProject(App& a, const std::string& path);
bool actLoadProject(App& a, const std::string& path);
void actZoomAtCursor(App& a, double factor);
void actSetTool(App& a, ToolId t);
void actSetPaletteColor(App& a, int index);
void actResetCamera(App& a);

// 把颜色记入"最近使用"（去重，最新在前）。所有改色的入口都应调用它。
void pushRecentColor(App& a, RGBA8 c);

// 光照参数变了：标记需要重算（渐进累积也会因此重开，否则新旧参数会混叠）
void actLightSettingsChanged(App& a);

// --- 对话框（实现在 app/panels_dialogs.cpp）---
//
// 对话框的展开状态属于 UI 自己的生命周期，所以调用方只表达"请求打开"，
// 不直接写状态；菜单栏与快捷键都经由这三个入口。
void requestNewCanvasDialog();
void requestShortcutsDialog();
void requestAboutDialog();

// 评测 / 自动化截图用：强制展开或折叠右侧栏的分区（按名字子串匹配，空串 = 不管）。
// ImGui 的折叠状态存在 .ini 里，功能一多右侧栏就要滚动，于是"改了某一节"在
// 截图里可能根本看不到。有了它就能稳定地把要验证的那一块放到画面里。
void setUiSections(const std::string& forceOpen, const std::string& forceClosed);

// --- 分区折叠的自动验证钩子 ---
//
// 折叠交互是"看不出来对不对"的典型：只能靠截图，而截图里点过和没点长得一样。
// 所以把分区标题栏的矩形暴露出来，让 main 用**真实鼠标事件**点它、再回读展开状态。
// 这样"点材质无法展开"这类缺陷就能被自动证明／证伪，而不是靠人肉点。
void beginPanelHeaderCapture();
// 取某个分区标题栏的中心点（屏幕坐标）。返回是否找到了它。
// 拿到点之后由调用方在 NewFrame 之后 io.AddMousePosEvent + 按下/抬起，
// 顺序不能反（见 app/panels_state.cpp 里的说明）。
bool panelSectionPoint(const char* name, float& cx, float& cy);
bool panelSectionOpen(const char* name);

}  // namespace pb::app
