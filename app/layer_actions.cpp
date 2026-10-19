// app/layer_actions.cpp — 图层动作
//
// 单独一个文件的原因：这些动作有三个入口（菜单栏、工具栏快捷键、图层面板的按钮），
// 把实现放在 layer_panel.cpp 里会让"面板"和"动作"互相依赖，
// 也使 menu / input 为了调一个动作而必须把整个面板文件拉进来。
//
// ## 为什么全部走 beginAll/commit
//
// 这些动作都会改变"哪些图层参与合成"（增删改序），逐层取差既麻烦又容易漏；
// 而整份文档本来就不大，直接整体快照最稳妥。相比之下，一笔画只需要看当前图层，
// 那条路径用 History::beginStroke。
//
// ## 为什么可见性 / 不透明度 / 改名**不**记撤销
//
// 它们是一眼就能看出效果的即时开关，撤销一个眼睛图标没有意义，反而会把
// 真正的笔画步骤挤出撤销栈。但它们同样改变合成结果，所以必须让光照重算。
#include <string>

#include "app/app.h"
#include "app/panels.h"
#include "core/composite.h"

namespace pb::app {

namespace {

// 结构性动作的收尾：提交撤销、标记需要重算
void commitStructural(App& a, const std::string& label) {
    if (a.history.commit(a.doc, label)) a.lastAction = label;
    a.lightDirty = true;
    a.layerPanelDirty = true;
}

}  // namespace

void notifyLayerPropertiesChanged(App& a) {
    // 非像素属性不会弄脏任何图层，所以要显式让合成重算。
    //
    // 这里偷懒地把所有图层标脏：图层数不多时这点代价可以忽略，而且比"精确算出哪些
    // 像素受影响"可靠得多 —— 后者一旦算漏，用户会看到画面和图层状态对不上。
    for (int i = 0; i < a.doc.layerCount(); i++) a.doc.layer(i).markAllDirty();
    a.lightDirty = true;
    a.layerPanelDirty = true;
}

void actAddLayer(App& a) {
    a.history.beginAll(a.doc);
    a.doc.addLayer();
    commitStructural(a, "新建图层");
}

void actDuplicateLayer(App& a) {
    a.history.beginAll(a.doc);
    a.doc.duplicateLayer(a.doc.activeIndex());
    commitStructural(a, "复制图层");
}

void actDeleteLayer(App& a) {
    if (a.doc.layerCount() <= 1) {
        a.lastAction = "至少要保留一个图层";
        return;
    }
    a.history.beginAll(a.doc);
    a.doc.removeLayer(a.doc.activeIndex());
    commitStructural(a, "删除图层");
}

void actMergeDown(App& a) {
    const int i = a.doc.activeIndex();
    if (i <= 0) {
        a.lastAction = "最底层没有可合并的图层";
        return;
    }
    a.history.beginAll(a.doc);
    a.doc.mergeDown(i);
    commitStructural(a, "向下合并");
}

void actMoveLayer(App& a, int delta) {
    const int from = a.doc.activeIndex();
    const int to = from + delta;
    if (to < 0 || to >= a.doc.layerCount()) return;
    a.history.beginAll(a.doc);
    a.doc.moveLayer(from, to);
    commitStructural(a, "移动图层");
}

void actToggleLayerVisible(App& a, int index) {
    if (index < 0 || index >= a.doc.layerCount()) return;
    Layer& L = a.doc.layer(index);
    L.visible = !L.visible;
    a.lastAction = L.visible ? "显示图层" : "隐藏图层";
    notifyLayerPropertiesChanged(a);
}

}  // namespace pb::app
