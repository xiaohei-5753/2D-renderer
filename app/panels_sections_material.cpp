// app/panels_sections_material.cpp — 右栏「材质」分区
//
// 这里编辑的是**当前色板项**的 7 维材质：函数末尾会把结果写回色板，
// 于是每个色板项记住自己的颜色/自发光/透明度，点回来还是这套（见 HANDOVER.md §3）。
//
// 材质的三条轴是**互相独立**的，别把它们绑死：
//   颜色(albedo) —— 决定像素显示成什么颜色，以及半透明时把透过的光染成什么颜色
//   自发光        —— **自己的一个 RGB 值**：可以等于表面色、也可以完全无关
//                    （深色表面发绿光、红表面发蓝光）
//   遮挡(alpha)   —— 0 = 空气，255 = 完全挡光；两者独立才能表达"不透明但发光"的屏幕
//
// ⚠️ 注意"独立"指的是**三轴之间没有固定换算关系**，不是"预设不该给自发光定初值"。
// 材质预设给的初值是「表面颜色 × 该预设的强度」（见 applyMaterialPreset）——
// 这是"霓虹管是蓝的、发的光就是蓝的"这个直觉，但用户随时可以用取色器改掉。
// 曾经预设给的是中性灰白，结果所有发光体都褪成同一种白色，彩色霓虹全没了。
#include <algorithm>
#include <cmath>

#include "app/panels_internal.h"
#include "core/palette.h"
#include "ui/widgets.h"

namespace pb::app {

namespace {

// 材质预设：一次性套用 (遮挡 alpha, 自发光, 折射率, 标志位) 的一组取值。
//
// 这些不是"随便起的名字"，每一个都对应光照模型里一种真实的像素行为；
// 特别是「霓虹管」与「彩色玻璃」正是把自发光和遮挡解耦之后才能区分的两类像素。
struct MaterialPreset {
    const char* name;
    const char* desc;
    uint8_t alpha;
    uint8_t emission;
    float ior;      // 1.0 = 不折射
    // 原来这里还有个 flags（kSolid）。已删除：它等价于"把 alpha 拉到 255"，
    // 是冗余功能，而且着色器从来没读过它。
};

const MaterialPreset kMaterialPresets[] = {
    {"空气", "完全通透，不挡光也不发光（等于擦除）", 0, 0, 1.0f},
    {"半透明", "只挡一部分光，自己不发光", 128, 0, 1.0f},
    {"霓虹管", "半透明 + 强自发光，会点亮周围", 110, 170, 1.0f},
    {"彩色玻璃", "半透明 + 带色，透过的光被染色。建议用于细线/薄片："
                 "遮挡是逐格吸收的，大块填充会因厚度而变得不透明", 120, 0, 1.5f},
    {"实心墙", "完全挡光，画面显示自身颜色", 255, 0, 1.0f},
    {"自发光屏", "不透明但自发光 —— 旧模型表达不了的一类像素", 255, 230, 1.0f},
};
const int kMaterialPresetCount = (int)(sizeof(kMaterialPresets) / sizeof(kMaterialPresets[0]));

void applyMaterialPreset(App& a, int i) {
    if (i < 0 || i >= kMaterialPresetCount) return;
    const MaterialPreset& p = kMaterialPresets[i];
    a.opts.color.a = p.alpha;

    // 自发光颜色 = **表面颜色 × 这个预设的发光强度**。
    //
    // 预设代表的是"这类像素怎么发光"，而发光的颜色理应就是它表面的颜色 ——
    // 一个霓虹管是蓝的，它发的光就该是蓝的。所以这里不再给中性灰白：
    // 灰白会让所有发光体褪成同一种白色，彩色霓虹就没了。
    //
    // 「颜色」与「亮度」仍然是两条独立的轴，只是预设把它们的**默认关系**
    // 定成了"发光跟着表面色走"：
    //   * 想换发光颜色：直接改下面的自发光取色器
    //   * 想只调亮度：用「亮度」滑块（等比缩放，不动色相）
    //   * 想让发光完全等于表面色：点「跟随表面颜色」
    // 用户改过之后并不会被这条公式绑死 —— 它只在按预设的那一刻算一次。
    //
    // 强度为 0 的预设（空气 / 半透明 / 彩色玻璃 / 实心墙）由这条公式自然
    // 得出不自发光，不需要额外分支。
    a.opts.material.setEmissionScaled(a.opts.color.r, a.opts.color.g, a.opts.color.b,
                                      p.emission);

    a.opts.material.setIorValue(p.ior);
    a.lastAction = std::string("材质 ") + p.name;
}

}  // namespace

void drawMaterialSection(App& a) {
    Material& m = a.opts.material;
    // 交给下面那些控件改；本函数末尾会把结果写回**当前色板项**，
    // 这样每个色板项记住自己的材质（改完再点回来还是这套）。
    const int palIdx = a.palette.currentIndex();
    RGBA8& col = a.opts.color;

    // ------------------------------------------------------------------
    // 1. 材质预设（注意：预设会同时改写颜色以外的全部轴）
    // ------------------------------------------------------------------
    ImGui::TextDisabled("预设（一键套用一组参数，会覆盖下面的各项）");
    const float pw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
                     0.5f;
    for (int i = 0; i < kMaterialPresetCount; i++) {
        const MaterialPreset& p = kMaterialPresets[i];
        if (ImGui::Button(p.name, ImVec2(pw, 0.0f))) applyMaterialPreset(a, i);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.desc);
        if ((i & 1) == 0 && i + 1 < kMaterialPresetCount) ImGui::SameLine();
    }

    // ==================================================================
    // 轴 1：颜色（albedo）
    // ==================================================================
    // 颜色决定两件事：像素显示成什么颜色、以及半透明时滤过什么颜色的光。
    // 它**不**决定亮度——亮度是下面独立的第二条轴。
    ImGui::Spacing();
    ImGui::SeparatorText("颜色（albedo）");
    float rgb[3] = {col.r / 255.0f, col.g / 255.0f, col.b / 255.0f};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::ColorEdit3("##albedo", rgb,
                          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB |
                              ImGuiColorEditFlags_PickerHueBar)) {
        col.r = (uint8_t)std::lround(rgb[0] * 255.0f);
        col.g = (uint8_t)std::lround(rgb[1] * 255.0f);
        col.b = (uint8_t)std::lround(rgb[2] * 255.0f);
        pushRecentColor(a, col);
    }
    ImGui::TextWrapped("决定像素显示成什么颜色，以及半透明时把透过的光染成什么颜色。");

    // ==================================================================
    // 轴 2：自发光（另一组 RGB，与 albedo 是两条独立的轴）
    // ==================================================================
    // 关键：自发光是**自己的一个 RGB 值**，不是从 albedo 派生的标量 ——
    // 所以暗色表面可以发绿光、红表面可以发蓝光，两者互不约束。
    // 预设会把它的**初值**设成"表面颜色 × 强度"，但这里可以改成任意颜色。
    ImGui::Spacing();
    ImGui::SeparatorText("自发光（独立 RGB）");
    float em[3] = {m.emR / 255.0f, m.emG / 255.0f, m.emB / 255.0f};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::ColorEdit3("##emcolor", em,
                          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB |
                              ImGuiColorEditFlags_PickerHueBar)) {
        m.emR = (uint8_t)std::lround(em[0] * 255.0f);
        m.emG = (uint8_t)std::lround(em[1] * 255.0f);
        m.emB = (uint8_t)std::lround(em[2] * 255.0f);
    }
    // 便捷：整条亮度缩放（仍然作用在那个 RGB 上，不引入新的耦合）
    int level = m.emissionLevel();
    ImGui::SetNextItemWidth(-1);
    if (ui::labeledSliderInt("##emlevel", &level, 0, 255, "亮度（等比缩放自发光 RGB）")) {
        if (m.emissionLevel() > 0) {
            const float k = (float)level / (float)m.emissionLevel();
            const int r = (int)std::lround(m.emR * k);
            const int g = (int)std::lround(m.emG * k);
            const int b = (int)std::lround(m.emB * k);
            m.emR = (uint8_t)std::clamp(r, 0, 255);
            m.emG = (uint8_t)std::clamp(g, 0, 255);
            m.emB = (uint8_t)std::clamp(b, 0, 255);
        } else if (level > 0) {
            m.setEmissionFrom((uint8_t)level, (uint8_t)level, (uint8_t)level);
        }
    }
    const float ew = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
                     0.5f;
    if (ImGui::Button("跟随表面颜色", ImVec2(ew, 0.0f)))
        m.setEmissionFrom(col.r, col.g, col.b);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("把自发光设成与 albedo 相同 —— 这不是必须的，只是常用的一键操作");
    ImGui::SameLine();
    if (ImGui::Button("无自发光", ImVec2(ew, 0.0f))) m.clearEmission();

    // 亮度渐变条：用与 blend 通道相同的公式画出"当前自发光在 6 档下的效果"
    {
        const int kSteps = 6;
        const float h = a.ui(18.0f);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float w = (avail - a.ui(2.0f) * (kSteps - 1)) / kSteps;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < kSteps; i++) {
            const float k = (float)i / (float)(kSteps - 1);
            // 与 blend 的实心分支同式：albedo + 自发光*k*1.7，峰值归一
            float r = col.r / 255.0f + m.emR / 255.0f * k * 1.7f;
            float g = col.g / 255.0f + m.emG / 255.0f * k * 1.7f;
            float b = col.b / 255.0f + m.emB / 255.0f * k * 1.7f;
            const float pk = std::max(std::max(r, g), b);
            if (pk > 1.0f) { r /= pk; g /= pk; b /= pk; }
            const ImVec2 q0(p0.x + (w + a.ui(2.0f)) * i, p0.y);
            const ImVec2 q1(q0.x + w, q0.y + h);
            dl->AddRectFilled(q0, q1,
                              IM_COL32((int)std::lround(r * 255), (int)std::lround(g * 255),
                                       (int)std::lround(b * 255), 255));
            dl->AddRect(q0, q1, IM_COL32(120, 124, 134, 255));
        }
        ImGui::Dummy(ImVec2(avail, h));
    }
    ImGui::TextWrapped("自发光是**自己的一个 RGB 值**："
                       "预设会把它设成「表面颜色 × 该预设的强度」（霓虹管是蓝的，发的光就是蓝的），"
                       "但这里的取色器可以把它改成与表面完全无关的颜色 —— "
                       "深色表面发绿光、红表面发蓝光都行。");

    // ==================================================================
    // 轴 3：遮挡（alpha）
    // ==================================================================
    ImGui::Spacing();
    ImGui::SeparatorText("遮挡（alpha）");
    int alpha = col.a;
    if (ui::labeledSliderInt("##matAlpha", &alpha, 0, 255,
                             "遮挡 alpha（0 = 空气，255 = 完全挡光）")) {
        col.a = (uint8_t)alpha;
    }
    ImGui::TextWrapped("0 = 空气（光直接穿过），255 = 完全挡光。"
                       "中间值是半透明介质：既挡一部分光，又把透过的光按上面的颜色染色。");

    // ==================================================================
    // 几何：折射（路径追踪已于 2026-10-19 移除，字段保留但不再参与光照）
    // ==================================================================
    ImGui::Spacing();
    ImGui::SeparatorText("几何");
    float ior = m.iorValue();
    ImGui::SetNextItemWidth(-1);
    if (ui::labeledSliderFloat("##matIor", &ior, 1.0f, 3.55f, "折射率（1.00 = 不折射）")) {
        m.setIorValue(ior);
    }
    // 这里原来有个「强制实心」复选框（Material::kSolid）。已删除：
    // 它的效果等价于"把上面的遮挡 alpha 拉到 255"，是冗余功能，
    // 删掉之后每像素材质从 6 字节降到 4 字节、扫描的材质纹理从 4 字节/像素
    // 降到 3 字节/像素。想要实心就把「遮挡（alpha）」拉到最大。

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
    ImGui::TextWrapped("遮挡与自发光：生效。"
                       "折射：字段保留，但路径追踪移除后已不参与光照"
                       "（扫描 GI 是直线扫描，物理上做不了弯曲光路）。");
    ImGui::PopStyleColor();

    // ==================================================================
    // 预览：左右并排显示"颜色"和"该颜色在当前位置的最终外观"
    // ==================================================================
    ImGui::Spacing();
    ImGui::SeparatorText("预览");
    {
        const float h = a.ui(30.0f);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float w = (avail - a.ui(4.0f)) * 0.5f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##matpreview", ImVec2(avail, h));
        ImDrawList* dl = ImGui::GetWindowDrawList();

        auto checker = [&](ImVec2 q0, ImVec2 q1) {
            const float cell = a.ui(7.0f);
            for (float y = q0.y; y < q1.y; y += cell)
                for (float x = q0.x; x < q1.x; x += cell) {
                    const bool odd = ((int)((x - q0.x) / cell) + (int)((y - q0.y) / cell)) & 1;
                    dl->AddRectFilled(ImVec2(x, y),
                                      ImVec2(std::min(x + cell, q1.x), std::min(y + cell, q1.y)),
                                      odd ? IM_COL32(58, 58, 62, 255) : IM_COL32(86, 86, 92, 255));
                }
        };

        // 左：纯颜色（带棋盘，直观看出遮挡程度）
        const ImVec2 l0(p0.x, p0.y), l1(p0.x + w, p0.y + h);
        checker(l0, l1);
        const float cov = (float)col.a / 255.0f;   // 遮挡只看 alpha（kSolid 已删）
        dl->AddRectFilled(l0, l1, IM_COL32(col.r, col.g, col.b, (int)std::lround(cov * 255.0f)));
        dl->AddRect(l0, l1, IM_COL32(120, 124, 134, 255));

        // 右：加上自发光之后的外观（与着色器同式：albedo + 自发光*1.7，峰值归一）
        float r = col.r / 255.0f + m.emR / 255.0f * 1.7f;
        float g = col.g / 255.0f + m.emG / 255.0f * 1.7f;
        float b = col.b / 255.0f + m.emB / 255.0f * 1.7f;
        const float pk = std::max(std::max(r, g), b);
        if (pk > 1.0f) { r /= pk; g /= pk; b /= pk; }
        const ImVec2 r0(p0.x + w + a.ui(4.0f), p0.y), r1(r0.x + w, p0.y + h);
        dl->AddRectFilled(r0, r1,
                          IM_COL32((int)std::lround(r * 255), (int)std::lround(g * 255),
                                   (int)std::lround(b * 255), 255));
        dl->AddRect(r0, r1, IM_COL32(120, 124, 134, 255));
    }
    ImGui::TextDisabled("左 = 颜色（棋盘透出多少 = 遮挡程度）　右 = 加上亮度后的外观");

    // ------------------------------------------------------------------
    // 从画布吸取
    // ------------------------------------------------------------------
    ImGui::Spacing();
    if (ImGui::Button("吸取光标下的颜色与材质", ImVec2(-1.0f, 0.0f))) {
        a.opts.color = a.cursorColor;
        a.opts.material = a.cursorMaterial;
        pushRecentColor(a, a.opts.color);
        // 吸到的颜色+材质写回当前色板项：色板是唯一真相
        const int pi = a.palette.currentIndex();
        Palette::Mat7 mm = a.palette.mat(pi);
        mm.r = a.opts.color.r; mm.g = a.opts.color.g; mm.b = a.opts.color.b;
        mm.a = a.opts.color.a;
        mm.eR = a.opts.material.emR; mm.eG = a.opts.material.emG;
        mm.eB = a.opts.material.emB;
        a.palette.setMat(pi, mm);
        a.lastAction = "已吸取材质";
    }
    ImGui::TextDisabled("也可以直接用取色器工具（K）在画布上点");

    // 把这一轮编辑的结果写回**当前色板项**：色板项 = 7 维材质，
    // 所以每个色板项记住自己的颜色/自发光/透明度，点回来还是这套。
    {
        Palette::Mat7 mm = a.palette.mat(palIdx);
        mm.r = a.opts.color.r;
        mm.g = a.opts.color.g;
        mm.b = a.opts.color.b;
        mm.a = a.opts.color.a;
        mm.eR = a.opts.material.emR;
        mm.eG = a.opts.material.emG;
        mm.eB = a.opts.material.emB;
        a.palette.setMat(palIdx, mm);
    }
}

}  // namespace pb::app