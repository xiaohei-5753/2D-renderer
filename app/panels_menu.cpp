// app/panels_menu.cpp — 顶部主菜单栏 + 底部状态栏
//
// 菜单项只是动作的入口：真正的实现在 panels_actions.cpp / layer_actions.cpp，
// 与快捷键（app/input.cpp）共用同一份。这样"这个功能怎么触发"和"它做什么"
// 不会各自演化出两个版本。
#include <algorithm>
#include <cstdio>
#include <string>

#include "app/panels_internal.h"
#include "ui/widgets.h"

namespace pb::app {

// ---------------------------------------------------------------------------
// 主菜单栏
// ---------------------------------------------------------------------------

void drawMainMenuBar(App& a) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("新建...", "Ctrl+N")) requestNewCanvasDialog();
        ImGui::Separator();
        // 工程文件：一次存下作品 + 界面 + 工具 + 光照参数
        //
        // 快捷键约定：**Ctrl+S 是"保存工程"**（业界惯例，用户第一反应就是这个），
        // 导出 PNG 挪到 Ctrl+E。原来 Ctrl+S 是导出 PNG，但工程文件才是"保存作品"
        // 的正解，PNG 导出属于"输出成品"，用 E(xport) 更贴切也更不容易误按。
        if (ImGui::MenuItem("打开工程...", "Ctrl+O")) a.requestProjectOpenDialog = true;
        if (ImGui::MenuItem("保存工程", "Ctrl+S")) {
            if (a.projectPath.empty()) a.requestProjectSaveDialog = true;
            else actSaveProject(a, a.projectPath);
        }
        if (ImGui::MenuItem("另存工程为...", "Ctrl+Shift+S")) a.requestProjectSaveDialog = true;
        if (!a.projectPath.empty()) {
            ImGui::TextDisabled("  当前：%s", a.projectPath.c_str());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("导出 PNG（原始）", "Ctrl+E")) actSavePng(a, false);
        if (ImGui::MenuItem("导出 PNG（受光）", "Ctrl+Shift+E")) actSavePng(a, true);
        ImGui::Separator();
        if (ImGui::MenuItem("退出", "Esc")) a.requestQuit = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("编辑")) {
        if (ImGui::MenuItem("撤销", "Ctrl+Z", false, a.history.canUndo())) actUndo(a);
        if (ImGui::MenuItem("重做", "Ctrl+Y", false, a.history.canRedo())) actRedo(a);
        ImGui::Separator();
        if (ImGui::MenuItem("清空画布", nullptr, false, true)) actClearCanvas(a);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("视图")) {
        ImGui::MenuItem("显示像素网格", "G", &a.showPixelGrid);
        ImGui::Separator();
        if (ImGui::MenuItem("完整显示画布（留边）", nullptr,
                            a.vp && a.vp->fitMode() == gfx::FitMode::Contain)) {
            if (a.vp) a.vp->setFitMode(gfx::FitMode::Contain);
        }
        if (ImGui::MenuItem("铺满面板（裁切）", nullptr,
                            a.vp && a.vp->fitMode() == gfx::FitMode::Cover)) {
            if (a.vp) a.vp->setFitMode(gfx::FitMode::Cover);
        }
        ImGui::Separator();
        ImGui::MenuItem("左侧工具栏", nullptr, &a.leftOpen);
        ImGui::MenuItem("右侧属性面板", nullptr, &a.rightOpen);
        ImGui::Separator();
        if (ImGui::MenuItem("放大", nullptr)) actZoomAtCursor(a, 1.0 / 1.25);
        if (ImGui::MenuItem("缩小", nullptr)) actZoomAtCursor(a, 1.25);
        if (ImGui::MenuItem("适应窗口", "Ctrl+0")) actResetCamera(a);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("光照")) {
        if (ImGui::MenuItem("开启光照", "Tab", &a.lightOn)) a.lightDirty = true;
        ImGui::Separator();
        // 路径追踪已于 2026-10-19 移除，只剩扫描 GI（不再有模式切换项）
        ImGui::TextDisabled("扫描 GI（实时）");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("快捷键一览")) requestShortcutsDialog();
        if (ImGui::MenuItem("关于")) requestAboutDialog();
        ImGui::EndMenu();
    }

    // 右侧显示当前状态摘要
    {
        const float right = ImGui::GetWindowWidth() - 320.0f;
        if (right > 200.0f) {
            ImGui::SameLine(right);
            ImGui::TextDisabled("%s | %s | %.0f fps", lightModeName(a.lightMode),
                                a.lightOn ? "光照开" : "光照关", a.fps);
        }
    }

    ImGui::EndMainMenuBar();
}

// ---------------------------------------------------------------------------
// 状态栏
// ---------------------------------------------------------------------------

void drawStatusBar(App& a) {
    ImGui::AlignTextToFramePadding();

    ImGui::Text("(%d, %d)", a.cursorX, a.cursorY);

    ImGui::SameLine(0, 12);
    {
        const RGBA8 c = a.cursorColor;
        ui::colorSwatch("statuscol", toU32(c), ImVec2(16, 16), false,
                        "光标下的像素颜色");
        ImGui::SameLine(0, 4);
        if (c.a == 0)
            ImGui::TextDisabled("空");
        else
            ImGui::Text("#%02X%02X%02X a=%d e=%d", c.r, c.g, c.b, c.a,
                        a.cursorMaterial.emissionLevel());
    }

    ImGui::SameLine(0, 12);
    ImGui::Text("| %s | %s | %dx%d", toolName(a.opts.tool), a.palette.presetName(),
                a.doc.width(), a.doc.height());

    ImGui::SameLine(0, 12);
    ImGui::Text("| %s", a.doc.active().name.c_str());

    ImGui::SameLine(0, 12);
    ImGui::Text("| 缩放 %.0f%%", a.vp ? 100.0 / std::max(0.01, a.vp->camera().zoom) : 100.0);

    ImGui::SameLine(0, 12);
    ui::statusDot(a.lightOn ? IM_COL32(120, 220, 120, 255) : IM_COL32(120, 120, 130, 255),
                  a.lightOn ? "光照开" : "光照关");

    ImGui::SameLine(0, 12);
    ImGui::Text("| 撤销 %zu / 重做 %zu", a.history.undoSteps(), a.history.redoSteps());

    // 右对齐：fps 与光照耗时
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "光照 %.1f ms | %.0f fps", a.lightMs, a.fps);
        const float w = ImGui::CalcTextSize(buf).x;
        const float right = ImGui::GetWindowWidth() - w - 14.0f;
        if (right > ImGui::GetCursorPosX()) {
            ImGui::SameLine(right);
            ImGui::TextUnformatted(buf);
        }
    }

    // 最近一次动作提示
    if (!a.lastAction.empty()) {
        ImGui::SameLine(0, 16);
        ImGui::TextDisabled("· %s", a.lastAction.c_str());
    }
}

}  // namespace pb::app