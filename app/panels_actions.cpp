// app/panels_actions.cpp — 面板 / 菜单 / 快捷键共用的动作
//
// 这些函数是"被界面调用的命令"，与"怎么画界面"无关，所以从 panels.cpp 里分出来。
// 每个动作有多个入口（菜单栏、快捷键、面板按钮），实现只有这一份 —— 这是刻意
// 的：同一个功能有两份实现，早晚会出现"改了这边忘了那边"。
//
// 动作本身只改 App 状态 / 文档 / 撤销栈，不碰 ImGui；界面部分见
// panels_canvas.cpp 与 panels_menu.cpp。
#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

#include "app/app.h"
#include "app/panels.h"
#include "core/composite.h"
#include "core/paths.h"
#include "core/image.h"
#include "core/project.h"
#include "gfx/viewport.h"
#include "gfx/gl_texture.h"

namespace pb::app {

void actSetTool(App& a, ToolId t) { a.opts.tool = t; }

void actSetPaletteColor(App& a, int index) {
    a.palette.setCurrent(index);
    const RGBA8 c = a.palette.current();
    // 色板项 = **7 维材质**（颜色 RGB + 自发光 RGB + 透明度）。
    // 选色 = 同时选定这一笔要用的颜色与材质，不再有"色板和材质两套东西"的困惑。
    const Palette::Mat7 m = a.palette.mat(index);
    a.opts.color = RGBA8{c.r, c.g, c.b, m.a};
    a.opts.material.setEmissionFrom(m.eR, m.eG, m.eB);
    pushRecentColor(a, a.opts.color);
}

void pushRecentColor(App& a, RGBA8 c) {
    int found = -1;
    for (int i = 0; i < a.recentCount; i++) {
        if (a.recent[i] == c) {
            found = i;
            break;
        }
    }
    if (found == 0) return;   // 已经是最新，不用动

    int last = 0;
    if (found > 0) {
        last = found;   // 提到最前：把 [0, found-1] 整体后移
    } else {
        if (a.recentCount < App::kRecentMax) a.recentCount++;
        last = a.recentCount - 1;
    }
    for (int i = last; i > 0; i--) a.recent[i] = a.recent[i - 1];
    a.recent[0] = c;
}

void actUndo(App& a) {
    if (a.history.undo(a.doc)) {
        a.lightDirty = true;
        a.layerPanelDirty = true;
        a.lastAction = "撤销";
    }
}

void actRedo(App& a) {
    if (a.history.redo(a.doc)) {
        a.lightDirty = true;
        a.layerPanelDirty = true;
        a.lastAction = "重做";
    }
}

void actClearCanvas(App& a) {
    // 清的是**当前图层**，不是整份文档 —— 与"工具只作用于当前图层"保持一致。
    // 但它是一次整层改动，所以走 beginAll：逐层取差在这里没有意义。
    a.history.beginAll(a.doc);
    a.doc.active().clear(RGBA8{0, 0, 0, 0}, Material{});
    a.history.commit(a.doc, "清空图层");
    a.lightDirty = true;
    a.layerPanelDirty = true;
    a.lastAction = "清空当前图层";
}

void actResetCamera(App& a) {
    if (a.vp) a.vp->fitIntegerZoom();
}

void actNewCanvas(App& a, int w, int h) {
    if (w < 1 || h < 1) return;
    // Document::resize 会丢弃所有图层并重置为单个空层
    a.doc.resize(w, h);
    a.history.reset();
    // LightSweep 的纹理和 SSBO 都是按初始尺寸一次性分配的，
    // 改尺寸必须重新 init（init 内部会 shutdown 再重建）
    if (a.sweep) a.sweep->init(w, h);
    if (a.vp) a.vp->setCanvasSize(w, h);
    a.lightDirty = true;
    a.layerPanelDirty = true;
    resetLightingCache();
    markAllLayersForLight(a);
    a.lastAction = "新建画布";
    a.cameraInitialized = false;   // 新画布重新做一次整数倍适配
    printf("[画布] 新建 %dx%d\n", w, h);
}

void actSavePng(App& a, bool lit) {
    // 导出到 **exe 同目录的 out\**，不是"当前工作目录的 out\"。
    //
    // 为什么：双击 exe 时当前工作目录是 exe 所在目录，看起来一样；但把
    // build\ 拷到别处（便携版）后，若用快捷方式/脚本从别处启动，当前目录
    // 就变了，`out\` 建在那边、或者干脆建不出来（没有写权限）——
    // **表现是"按了导出但什么都没发生"**，而且没有任何报错。
    // 和字体、光照配置、工程文件保持同一套"资源都跟着 exe 走"的约定。
    const std::string dir = executableDir() + "out";
    char path[512];
    std::snprintf(path, sizeof(path), "%s%cpaintboard_%03d%s.png", dir.c_str(),
                  '/', a.saveCounter, lit ? "_lit" : "");
    a.saveCounter++;

#ifdef _WIN32
    _mkdir(dir.c_str());   // 已存在会失败，无所谓（返回 -1）
#endif

    const int W = a.doc.width(), H = a.doc.height();
    if (W <= 0 || H <= 0) return;

    if (!lit) {
        // 导出的是**压平后的结果**：图层是编辑期的概念，导出的应该就是用户看到的那张图。
        // 这里的 alpha 也是覆盖率 —— 直接写出 albedo 会让半透明像素看起来"没画"，
        // 所以用 core 的压平（它做的是标准 source-over）。
        Image flat(W, H);
        std::vector<Material> mats((size_t)W * (size_t)H);
        flattenDocument(a.doc, flat, mats, Rect{0, 0, W - 1, H - 1});
        if (writePNG(path, flat)) {
            a.lastAction = std::string("已保存 ") + path;
            printf("[保存] %s（压平后的画布）\n", path);
        }
        return;
    }

    if (!a.sweep) return;
    Image litImg(W, H);
    if (!gfx::readbackTexture(a.sweep->resultTexture(), W, H, litImg.data())) {
        a.lastAction = "受光图回读失败";
        return;
    }
    for (size_t i = 0; i < litImg.pixelCount(); i++) litImg.data()[i].a = 255;
    // 写 PNG 之前翻一次（用户实测导出上下颠倒）
    litImg.flipVertical();

    if (writePNG(path, litImg)) {
        a.lastAction = std::string("已保存 ") + path;
        printf("[保存] %s（逐层光照 + 合成后的结果）\n", path);
    }
}

void actZoomAtCursor(App& a, double factor) {
    if (!a.ctx || !a.vp) return;
    double cx = 0.0, cy = 0.0;
    a.vp->screenToCanvasF(a.mx, a.my, cx, cy);
    a.vp->zoomBy(factor);
    float du = 1.0f, dv = 1.0f;
    a.vp->viewUV(du, dv);
    // 反解相机，使光标下的画布点在缩放前后保持不动
    const double u = (a.mx - a.vp->viewX()) / a.vp->viewW();
    const double v = 1.0 - (a.my - a.vp->viewY()) / a.vp->viewH();
    a.vp->camera().x = cx / (double)a.vp->canvasWidth() - (double)du * (u - 0.5);
    a.vp->camera().y = cy / (double)a.vp->canvasHeight() - (double)dv * (v - 0.5);
}

void actLightSettingsChanged(App& a) {
    a.lightDirty = true;
}


// ---------------------------------------------------------------------------
// 工程文件（.pbproj）
// ---------------------------------------------------------------------------
//
// 存/读的范围见 core/project.h —— 一个文件装下作品、调色板、工具、界面布局
// 与光照参数，目标是"下次打开跟关掉时一模一样"。

// 把 App 里散落的状态打包成工程文件需要的视图。
// 集中在一处的原因：save 与 load 必须**用同一份**映射，否则很容易只加了一半
// （存了却没读，或读了却没存）—— 这类 bug 表现为"某个设置重启后丢了"，
// 而且很难发现是哪一个。放一起就一眼能对照。
static ProjectState projectStateOf(App& a, gfx::LightSettings& light) {
    ProjectState st;
    st.doc = &a.doc;
    st.palette = &a.palette;
    st.tool = &a.opts;
    st.light = &light;
    st.lightMode = 0;   // 只剩扫描 GI；字段保留只为文件格式兼容
    st.fieldStrength = a.fieldStrength;
    return st;
}

bool actSaveProject(App& a, const std::string& path) {
    if (!a.sweep) return false;
    gfx::LightSettings light = a.sweep->settings();
    // 界面与视图状态是本地的，临时填进去（ProjectState 只借用指针）
    static ViewState vs;
    static UiState us;
    // Viewport 没有 zoom()/offsetX() 这类取值接口，但 camera() 返回可变引用，
    // 相机就是那一个结构（x/y = 视野中心的画布 UV，zoom = 可见比例）。
    vs.zoom = a.vp ? (float)a.vp->camera().zoom : 1.0f;
    vs.camX = a.vp ? (float)a.vp->camera().x : 0.5f;
    vs.camY = a.vp ? (float)a.vp->camera().y : 0.5f;
    us.leftWidth = a.leftWidth;
    us.rightWidth = a.rightWidth;
    us.leftOpen = a.leftOpen;
    us.rightOpen = a.rightOpen;
    us.showPixelGrid = a.showPixelGrid;

    ProjectState st = projectStateOf(a, light);
    st.view = &vs;
    st.ui = &us;

    if (!saveProject(path, st)) {
        a.lastAction = "保存工程失败（详见控制台）";
        return false;
    }
    a.projectPath = path;
    a.lastAction = "已保存工程 " + path;
    return true;
}

bool actLoadProject(App& a, const std::string& path) {
    if (!a.sweep) return false;
    gfx::LightSettings light = a.sweep->settings();
    static ViewState vs;
    static UiState us;
    ProjectState st = projectStateOf(a, light);
    st.view = &vs;
    st.ui = &us;

    if (!loadProject(path, st)) {
        a.lastAction = "读取工程失败（详见控制台）";
        return false;
    }

    // 加载成功后把各处的状态写回
    a.sweep->settings() = light;
    // st.lightMode 故意忽略：旧工程里可能是 1（路径追踪），但那条路已于
    // 2026-10-19 移除，现在一律按扫描 GI 打开。字段本身保留只为格式兼容。
    a.fieldStrength = st.fieldStrength;
    a.leftWidth = us.leftWidth;
    a.rightWidth = us.rightWidth;
    a.leftOpen = us.leftOpen;
    a.rightOpen = us.rightOpen;
    a.showPixelGrid = us.showPixelGrid;
    if (a.vp) {
        a.vp->camera().zoom = std::clamp((double)vs.zoom, 0.02, 64.0);
        a.vp->camera().x = vs.camX;
        a.vp->camera().y = vs.camY;
    }

    // 全部图层都要重算光照，并重建缩略图
    markAllLayersForLight(a);
    a.layerPanelDirty = true;
    a.lightDirty = true;
    a.projectPath = path;
    a.lastAction = "已打开工程 " + path;
    printf("[proj] 层数 %d，活动层 %d，色板 %d 色\n", a.doc.layerCount(),
           a.doc.activeIndex(), a.palette.size());
    return true;
}
}  // namespace pb::app
