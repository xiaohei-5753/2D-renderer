// app/panels_properties.cpp — 右栏：属性面板（四个可折叠分区的提交）
//
// 每个分区自己的内容在对应的文件里：
//   panels_sections_palette.cpp    调色板
//   panels_sections_material.cpp   材质
//   panels_sections_lighting.cpp   光照
//   layer_panel.cpp                图层
#include "app/app.h"
#include "app/panels.h"
#include "app/panels_internal.h"

namespace pb::app {

const char* lightModeName(LightMode m) {
    switch (m) {
        case LightMode::Sweep: return "扫描 GI";
        default: return "?";
    }
}

void drawPropertiesPanel(App& a) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));

    // ⚠️ `if (ImGui::CollapsingHeader(...))` 这个写法里 **每一帧都要调用**
    // CollapsingHeader，它的返回值才是"是否展开"。不能写成
    // `if (open) CollapsingHeader(...)` —— 那样一旦折叠就再也不提交这个控件，
    // 状态永远无法翻转（同类的坑在 ImGui 里很常见）。
    //
    // 默认展开状态：材质分区参数最多、最长，默认折起来 —— 否则右侧栏一开就要滚动
    // 才能看到「图层」，而图层是这一版的主角。
    //
    // 用一张表代替原来四段重复的代码：四个分区除了名字、默认值和内容函数之外
    // 完全一样，写四遍只会让"漏掉一次 recordHeader"这类错误有机会发生。
    struct Section {
        const char* name;
        bool defaultOpen;
        void (*draw)(App&);
    };
    static const Section kSections[] = {
        {"调色板", true, &drawPaletteSection},
        {"材质", false, &drawMaterialSection},
        {"光照", true, &drawLightingSection},
        {"图层", true, &drawLayerSection},
    };

    for (const Section& s : kSections) {
        const bool def = sectionHeader(s.name, s.defaultOpen);
        const bool open =
            ImGui::CollapsingHeader(s.name, def ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        if (open) s.draw(a);
        recordHeader(s.name, open);
        ImGui::Spacing();
    }

    ImGui::PopStyleVar();
}

}  // namespace pb::app