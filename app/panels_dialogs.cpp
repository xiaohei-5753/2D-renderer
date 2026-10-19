// app/panels_dialogs.cpp — 模态对话框（快捷键 / 关于 / 新建画布）+ 字体缺失警告
//
// 三个对话框的"打开"请求都只是这里的文件内状态，通过 requestXxx() 设置。
// 之所以不让调用方直接写状态：快捷键（app/input.cpp）与菜单栏都要能拉起同一个
// 对话框，而对话框的展开状态本来就属于 UI 自己的生命周期。
#include <algorithm>
#include <cstdio>

#include <string>

#include "app/panels_internal.h"
#include "core/paths.h"

namespace pb::app {

namespace {

bool g_openNewCanvas = false;
bool g_openAbout = false;
bool g_openShortcuts = false;
int g_newW = 320, g_newH = 180;

// 字体加载失败必须显式告诉用户：
// 双击 exe 启动时没有控制台，如果只在 stderr 里报一句，用户只会看到满屏 '?'
// 而完全不知道发生了什么。
//
// 注意这里的文案**全部是 ASCII**：这个弹窗要报告的问题就是"中文字形不可用"，
// 文案要是用中文写，弹窗本身就会变成一片 '?'，等于没报。
void drawFontWarning(App& a) {
    if (a.fontWarning.empty()) return;
    if (!a.fontWarningShown) {
        ImGui::OpenPopup("UI font not found");
        a.fontWarningShown = true;
    }
    if (ImGui::BeginPopupModal("UI font not found", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.40f, 1.0f));
        ImGui::TextUnformatted("Chinese UI text cannot be rendered.");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::TextUnformatted(a.fontWarning.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("The app looks for assets in: exe dir -> its parent -> cwd,");
        ImGui::TextDisabled("so keeping the font in the project root also works.");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(a.ui(120.0f), 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void drawShortcutsDialog() {
    if (g_openShortcuts) ImGui::OpenPopup("快捷键一览");
    if (g_openShortcuts && ImGui::BeginPopupModal("快捷键一览", &g_openShortcuts,
                                                  ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "工具   P 铅笔   L 直线   R 矩形   E 椭圆   F 油漆桶\n"
            "       D 抖动   K 取色   X 橡皮   I 加光   O 减光\n"
            "笔刷   [ ] 改大小      B 方圆切换     G 像素网格\n"
            "色板   1-9,0 选色      , . 切换色板\n"
            "视图   滚轮缩放        中键/空格+左键 拖拽平移\n"
            "       Ctrl+0 适应窗口\n"
            "编辑   Ctrl+Z 撤销     Ctrl+Y 重做    Ctrl+N 新建\n"
            "工程   Ctrl+S 保存       Ctrl+Shift+S 另存为   Ctrl+O 打开\n"
            "导出   Ctrl+E 原始图     Ctrl+Shift+E 受光图\n"
            "光照   Tab 开关光照");
        ImGui::Spacing();
        if (ImGui::Button("关闭", ImVec2(120, 0))) g_openShortcuts = false;
        ImGui::EndPopup();
    }
}

void drawAboutDialog() {
    if (g_openAbout) ImGui::OpenPopup("关于");
    if (g_openAbout && ImGui::BeginPopupModal("关于", &g_openAbout,
                                              ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("paintboard — 2D 像素风格画板");
        ImGui::Separator();
        ImGui::Text("由 magic-simulator 模块改造而来");
        ImGui::Text("渲染：方向扫描式全局光照（扫描 GI）");
        ImGui::Text("界面：Dear ImGui %s", IMGUI_VERSION);
        ImGui::Text("字体：Unifont（含全量中文）");
        ImGui::Spacing();
        if (ImGui::Button("关闭", ImVec2(120, 0))) g_openAbout = false;
        ImGui::EndPopup();
    }
}

void drawNewCanvasDialog(App& a) {
    if (g_openNewCanvas) ImGui::OpenPopup("新建画布");
    if (g_openNewCanvas && ImGui::BeginPopupModal("新建画布", &g_openNewCanvas,
                                                  ImGuiWindowFlags_AlwaysAutoResize)) {
        static const int kPresets[][2] = {
            {64, 64}, {128, 128}, {256, 256}, {320, 180}, {512, 288},
        };
        for (int i = 0; i < 5; i++) {
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "%dx%d", kPresets[i][0], kPresets[i][1]);
            if (ImGui::Button(lbl, ImVec2(84, 0))) {
                actNewCanvas(a, kPresets[i][0], kPresets[i][1]);
                g_openNewCanvas = false;
            }
            if (i < 4) ImGui::SameLine();
        }
        ImGui::Spacing();
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("宽", &g_newW, 0, 0);
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt("高", &g_newH, 0, 0);
        g_newW = std::clamp(g_newW, 1, 2048);
        g_newH = std::clamp(g_newH, 1, 2048);
        ImGui::Spacing();
        if (ImGui::Button("创建", ImVec2(120, 0))) {
            actNewCanvas(a, g_newW, g_newH);
            g_openNewCanvas = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(120, 0))) g_openNewCanvas = false;
        ImGui::EndPopup();
    }
}

}  // namespace

// 前置声明：定义在文件后面（drawDialogs 要用）
void drawProjectDialogs(App& a);

void requestNewCanvasDialog() { g_openNewCanvas = true; }
void requestShortcutsDialog() { g_openShortcuts = true; }
void requestAboutDialog() { g_openAbout = true; }

// 每帧在 drawUi 的最后调用
void drawDialogs(App& a) {
    drawFontWarning(a);
    drawShortcutsDialog();
    drawAboutDialog();
    drawNewCanvasDialog(a);
    drawProjectDialogs(a);
}

// ---------------------------------------------------------------------------
// 工程文件的"打开 / 另存为"路径对话框
// ---------------------------------------------------------------------------
//
// 为什么自己做输入框而不是调 Win32 的 GetOpenFileName：
//   * 那需要 comdlg32 + 一大堆 Windows 结构体，而本项目的 gfx/core 层刻意
//     保持平台无关（这一层只有 app/ 允许碰 win32）
//   * 一个输入框已经够用：路径可以直接粘贴，而且启动时用 `--project` 传路径
//     是更常见的用法（脚本 / 双击快捷方式）
//
// 默认路径给 exe 同目录下的 project.pbproj —— 和字体、光照配置一个道理，
// 整个 build\ 拷走就带着作品走。
void drawProjectDialogs(App& a) {
    const char* kOpenTitle = "打开工程";
    const char* kSaveTitle = "保存工程";

    // ⚠️ 这里有个**很容易写错的地方**，我第一版就错了：
    //
    // ImGui 的 BeginPopupModal 在 "OpenPopup 的那一帧" 返回 **false** ——
    // 弹窗要到下一帧才真正出现。所以绝对不能在本函数末尾无条件清掉请求标志：
    // 清了以后，下一帧 `openReq` 已经是 false，于是既不 OpenPopup 也不
    // BeginPopupModal，弹窗内容永远不绘制、EndPopup 也不会被调用 ——
    // 表现就是**点菜单完全没反应**。
    //
    // 正确做法：请求标志保持到弹窗真正关闭为止；模式（打开/保存）另存在
    // a.projectDialogIsOpen 里，跨帧有效。
    const bool reqOpen = a.requestProjectOpenDialog;
    const bool reqSave = a.requestProjectSaveDialog;
    // 请求只在**刚发出**的那一帧锁存模式；之后靠 projectDialogModeOpen 自己活着。
    //
    // ⚠️ 这里是我踩的坑：第一版写成
    //        a.projectDialogIsOpen = a.requestProjectOpenDialog;
    //    看着"把模式存下来"，实际读的是**请求标志本身**。点"保存工程"时
    //    requestProjectOpenDialog 是 false（发的是 SAVE 请求），于是这一句把
    //    存活标志设成了 **false** → 立刻 return，弹窗永远打不开。
    //    症状是"点菜单完全没反应"，而代码看起来完全合理。
    if (reqOpen || reqSave) {
        a.projectDialogIsOpen = true;
        a.projectDialogModeOpen = reqOpen;   // 锁存：这一帧之后不再受请求影响
        a.projectPathBuf[0] = '\0';          // 重新填默认路径
    }
    if (!a.projectDialogIsOpen) return;

    const bool isOpen = a.projectDialogModeOpen;
    const char* title = isOpen ? kOpenTitle : kSaveTitle;

    if (a.projectPathBuf[0] == '\0') {
        std::string def;
        if (!a.projectPath.empty()) {
            def = a.projectPath;              // 已有工程：默认覆盖/打开同一个
        } else {
            def = executableDir();            // 否则给 exe 同目录的默认名
            def += "project.pbproj";
        }
        std::snprintf(a.projectPathBuf, sizeof(a.projectPathBuf), "%s", def.c_str());
    }

    if (reqOpen || reqSave) ImGui::OpenPopup(title);
    bool stillOpen = false;
    const bool began = ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (began) {
        stillOpen = true;
        ImGui::TextUnformatted(isOpen ? "输入要打开的 .pbproj 路径："
                                      : "输入要保存到的 .pbproj 路径：");
        ImGui::SetNextItemWidth(a.ui(560.0f));
        const bool enter = ImGui::InputText("##projpath", a.projectPathBuf,
                                            sizeof(a.projectPathBuf),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("可以粘贴完整路径；相对路径按\"当前工作目录\"解析");
        ImGui::Spacing();
        const bool ok = ImGui::Button("确定", ImVec2(a.ui(120.0f), 0.0f)) || enter;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("取消", ImVec2(a.ui(120.0f), 0.0f));

        if (ok && a.projectPathBuf[0] != '\0') {
            const std::string path = a.projectPathBuf;
            if (isOpen) actLoadProject(a, path);
            else actSaveProject(a, path);
            ImGui::CloseCurrentPopup();
            stillOpen = false;
        }
        if (cancel) {
            ImGui::CloseCurrentPopup();
            stillOpen = false;
        }
        ImGui::EndPopup();
    }

    // 弹窗真的关掉了才收尾。stillOpen 在本帧为 true 说明它还在显示中。
    if (!stillOpen) {
        a.projectDialogIsOpen = false;
        a.requestProjectOpenDialog = false;
        a.requestProjectSaveDialog = false;
        a.projectPathBuf[0] = '\0';
    }
}
}  // namespace pb::app
