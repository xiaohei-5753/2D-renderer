// app/layer_panel.cpp — 图层面板（缩略图 / 眼睛 / 不透明度 / 增删移并）
//
// ## 为什么单独一个文件
//
// 这个面板比其它分区重得多：它要维护一份 GPU 缩略图缓存、要处理拖拽排序、
// 还要把结构性操作正确地接到 History 上。塞进 panels.cpp 只会让它更难读。
//
// ## 两个关键的设计点
//
// 1. **缩略图必须缓存**。每帧把整层转成 RGBA32 再传 GPU，在 512×512 × 十几层下
//    是纯浪费。靠 Layer::revision 判断内容变没变，只有变了才重建那一张。
//    但要注意 revision 只管"内容"，**图层顺序 / 增删**不会改任何 revision，
//    所以还要额外记一份"结构指纹"来触发重建。
//
// 2. **结构性操作走 history.beginAll/commit**。新建、删除、合并、重排都会改变
//    "哪些图层参与合成"，逐层取差既麻烦又容易漏；整份文档快照最稳妥。
//    而可见性 / 不透明度 / 改名是即时开关，不记入撤销（撤销一个眼睛图标没有意义），
//    但它们同样会改变合成结果，所以必须让光照重算。
//
// ## 列表是**倒序**显示的
//
// Document 的索引 0 是最底层，而图层面板的惯例是"最上面的图层显示在列表最上面"。
// 所以渲染循环从 layerCount()-1 往下走。这个反序只出现在这里，别的地方一律用
// Document 的原始索引 —— 混用是这类代码最容易出的错。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "app/panels.h"
#include "imgui.h"
#include "ui/widgets.h"

namespace pb::app {

namespace {

constexpr float kRowHeight = 44.0f;   // 逻辑像素，实际用 a.ui() 缩放

// ---------------------------------------------------------------------------
// 缩略图缓存
// ---------------------------------------------------------------------------

// 缩略图的降采样：把整层按整数块平均成一个更小的 RGBA 图。
//
// 用**块平均**而不是最近邻采样：像素画的细节（霓虹细线之类）在缩略图里
// 用最近邻会整条消失，平均之后至少还能看出"这里有一条亮的"。
void downsampleLayer(const Layer& L, int maxDim, std::vector<uint8_t>& out, int& ow,
                     int& oh) {
    ow = oh = 0;
    if (L.empty() || maxDim <= 0) return;
    const int W = L.width(), H = L.height();
    const int f = std::max(1, (std::max(W, H) + maxDim - 1) / maxDim);
    ow = std::max(1, W / f);
    oh = std::max(1, H / f);
    out.assign((size_t)ow * (size_t)oh * 4, 0);

    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            int sr = 0, sg = 0, sb = 0, sa = 0, n = 0;
            for (int dy = 0; dy < f; dy++) {
                const int sy = y * f + dy;
                if (sy >= H) break;
                for (int dx = 0; dx < f; dx++) {
                    const int sx = x * f + dx;
                    if (sx >= W) break;
                    const RGBA8 c = L.albedo.at(sx, sy);
                    // 预乘后平均，避免透明像素的黑色把颜色拉暗
                    const float al = c.a / 255.0f;
                    sr += (int)std::lround(c.r * al);
                    sg += (int)std::lround(c.g * al);
                    sb += (int)std::lround(c.b * al);
                    sa += c.a;
                    n++;
                }
            }
            if (n == 0) continue;
            const float aAvg = (float)sa / (float)n;
            const float inv = aAvg > 0.5f ? 255.0f / aAvg : 0.0f;
            const size_t o = ((size_t)y * (size_t)ow + (size_t)x) * 4;
            out[o + 0] = (uint8_t)std::clamp((int)std::lround((float)sr / n * inv), 0, 255);
            out[o + 1] = (uint8_t)std::clamp((int)std::lround((float)sg / n * inv), 0, 255);
            out[o + 2] = (uint8_t)std::clamp((int)std::lround((float)sb / n * inv), 0, 255);
            out[o + 3] = (uint8_t)std::clamp((int)std::lround(aAvg), 0, 255);
        }
    }
}

// 缩略图用的纹理：与画布纹理不同，这里要 **LINEAR** 采样。
//
// 画布必须 NEAREST（放大后要保持方块像素边缘），但缩略图是"缩小显示"，
// 最近邻会把降采样后还留下的细节整片丢掉，看起来像坏了；线性过滤反而是对的。
GLuint makeThumbTexture(int w, int h, const uint8_t* rgba) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void freeThumb(LayerThumb& t) {
    if (t.tex) glDeleteTextures(1, &t.tex);
    t = LayerThumb{};
}

// 结构指纹：图层身份 + 顺序 + 元数据。任何一项变了就得重建全部缩略图。
uint64_t structureKey(const Document& d) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    mix((uint64_t)d.layerCount());
    for (int i = 0; i < d.layerCount(); i++) {
        const Layer& L = d.layer(i);
        mix((uint64_t)L.width() * 1315423911ull + (uint64_t)L.height());
        for (char ch : L.name) mix((uint64_t)(unsigned char)ch);
        mix(L.visible ? 3u : 5u);
        mix((uint64_t)L.opacity);
        mix(L.locked ? 7u : 11u);
    }
    return h;
}

uint64_t g_structureKey = 0;
bool g_structureKeyValid = false;

}  // namespace

void uploadLayerThumbnails(App& a) {
    const int n = a.doc.layerCount();
    const uint64_t key = structureKey(a.doc);
    const bool structureChanged = !g_structureKeyValid || key != g_structureKey;
    if (structureChanged) {
        g_structureKey = key;
        g_structureKeyValid = true;
        a.layerPanelDirty = true;
    }

    // 先释放多余的（图层被删掉了）
    for (size_t i = (size_t)n; i < a.thumbs.size(); i++) freeThumb(a.thumbs[i]);
    a.thumbs.resize((size_t)n);

    std::vector<uint8_t> px;
    for (int i = 0; i < n; i++) {
        Layer& L = a.doc.layer(i);
        LayerThumb& t = a.thumbs[(size_t)i];
        if (t.tex != 0 && !structureChanged && t.revision == L.revision) continue;

        int tw = 0, th = 0;
        downsampleLayer(L, 64, px, tw, th);
        if (tw <= 0 || th <= 0) {
            freeThumb(t);
            continue;
        }
        if (t.tex != 0 && (t.w != tw || t.h != th)) freeThumb(t);
        if (t.tex == 0) {
            t.tex = makeThumbTexture(tw, th, px.data());
            t.w = tw;
            t.h = th;
        } else {
            glBindTexture(GL_TEXTURE_2D, t.tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE,
                            px.data());
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        t.revision = L.revision;
    }
    a.layerPanelDirty = false;
}

// ---------------------------------------------------------------------------
// 面板
// ---------------------------------------------------------------------------

void drawLayerSection(App& a) {
    const int n = a.doc.layerCount();
    const int active = a.doc.activeIndex();

    // --- 顶部：当前图层摘要 ---
    ImGui::TextDisabled("当前：%s", a.doc.active().name.c_str());
    if (!a.doc.active().visible)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "该图层已隐藏，无法绘制");
    else if (a.doc.active().locked)
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "该图层已锁定，无法绘制");

    // --- 操作按钮（动作实现在 app/layer_actions.cpp，菜单与快捷键共用同一套）---
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float bw2 = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
    const ImVec2 half(bw2, 0.0f);

    if (ImGui::Button("新建", half)) actAddLayer(a);
    ImGui::SameLine();
    if (ImGui::Button("复制", half)) actDuplicateLayer(a);

    ImGui::BeginDisabled(n <= 1);
    if (ImGui::Button("删除", half)) actDeleteLayer(a);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(active <= 0);
    if (ImGui::Button("向下合并", half)) actMergeDown(a);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(active >= n - 1);
    if (ImGui::Button("上移一层", half)) actMoveLayer(a, +1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(active <= 0);
    if (ImGui::Button("下移一层", half)) actMoveLayer(a, -1);
    ImGui::EndDisabled();

    // --- 当前图层的不透明度 ---
    {
        Layer& L = a.doc.active();
        int op = L.opacity;
        ImGui::SetNextItemWidth(-1);
        if (ui::labeledSliderInt("##layerOpacity", &op, 0, 255, "图层不透明度")) {
            L.opacity = (uint8_t)op;
            notifyLayerPropertiesChanged(a);
        }
        ImGui::TextDisabled("与笔刷的遮挡 alpha 相乘：两者一起决定这个像素有多遮光");
    }

    // --- 图层列表（倒序：最上层在图层面板的最上面）---
    const float rowH = a.ui(kRowHeight);
    const int rows = std::max(2, std::min(n, 10));
    const float listH = rowH * (float)rows + a.ui(8.0f);
    ImGui::BeginChild("##layerlist", ImVec2(0.0f, listH), ImGuiChildFlags_Borders);

    int moveFrom = -1, moveTo = -1;

    for (int i = n - 1; i >= 0; i--) {
        Layer& L = a.doc.layer(i);
        ImGui::PushID(i);
        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));
        const bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        // 点击选中（拖拽排序在下面统一处理）
        if (clicked && i != active) a.doc.setActiveIndex(i);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 r0 = rowPos;
        const ImVec2 r1 = ImVec2(rowPos.x + rowW, rowPos.y + rowH);
        const bool isActive = (i == active);
        if (isActive)
            dl->AddRectFilled(r0, r1, IM_COL32(90, 130, 200, 90));
        else if (hovered)
            dl->AddRectFilled(r0, r1, IM_COL32(255, 255, 255, 18));

        // 眼睛（用单个字母：内置字体里没有图标，也刻意不引入图标字体）
        const float eyeW = a.ui(22.0f);
        ImGui::SetCursorScreenPos(ImVec2(rowPos.x + a.ui(3.0f), rowPos.y + a.ui(3.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              L.visible ? ImVec4(0.85f, 0.92f, 1.0f, 1.0f)
                                        : ImVec4(0.45f, 0.46f, 0.5f, 1.0f));
        if (ImGui::Button(L.visible ? "O" : "-", ImVec2(eyeW, a.ui(18.0f))))
            actToggleLayerVisible(a, i);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(L.visible ? "点击隐藏（隐藏后不参与合成与光照）" : "点击显示");

        // 锁定
        ImGui::SetCursorScreenPos(ImVec2(rowPos.x + a.ui(3.0f), rowPos.y + a.ui(23.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              L.locked ? ImVec4(1.0f, 0.78f, 0.4f, 1.0f)
                                       : ImVec4(0.45f, 0.46f, 0.5f, 1.0f));
        if (ImGui::Button(L.locked ? "L" : ".", ImVec2(eyeW, a.ui(18.0f)))) {
            L.locked = !L.locked;
            L.revision++;
            a.lastAction = L.locked ? "锁定图层" : "解锁图层";
            a.layerPanelDirty = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("锁定后该层不接受笔触");

        // 缩略图：棋盘底 + 图层内容（GL 的 V 轴向上，所以 uv0.y/uv1.y 要对调）
        const float th = a.ui(36.0f);
        const float tw = (L.height() > 0) ? th * (float)L.width() / (float)L.height() : th;
        const ImVec2 t0(rowPos.x + eyeW + a.ui(8.0f), rowPos.y + a.ui(4.0f));
        const ImVec2 t1(t0.x + std::min(tw, rowW - eyeW - a.ui(70.0f)), t0.y + th);
        dl->AddRectFilled(t0, t1, IM_COL32(40, 42, 48, 255));
        if (i < (int)a.thumbs.size() && a.thumbs[(size_t)i].tex != 0) {
            dl->AddImage((ImTextureID)(intptr_t)a.thumbs[(size_t)i].tex, t0, t1,
                         ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        }
        dl->AddRect(t0, t1, IM_COL32(120, 124, 134, 255));

        // 名字与信息
        const ImVec2 tx(t1.x + a.ui(6.0f), rowPos.y + a.ui(4.0f));
        dl->AddText(tx, isActive ? IM_COL32(255, 255, 255, 255)
                                 : IM_COL32(210, 212, 218, 255),
                    L.name.c_str());
        char info[64];
        std::snprintf(info, sizeof(info), "%dx%d  %d%%%s", L.width(), L.height(),
                      (int)std::lround(L.opacity / 255.0f * 100.0f),
                      L.locked ? "  锁" : "");
        dl->AddText(ImVec2(tx.x, tx.y + ImGui::GetTextLineHeight() + a.ui(2.0f)),
                    IM_COL32(150, 152, 160, 255), info);

        // 拖拽排序：在行上按住拖动，松手时把图层移到目标行的位置。
        // 用 ImGui 的 drag&drop（而不是自己算鼠标位移）：它的 payload 机制天然处理了
        // "拖出了列表""中途取消"这些边界情况。
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            ImGui::SetDragDropPayload("PB_LAYER", &i, sizeof(int));
            ImGui::Text("移动 %s", L.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PB_LAYER")) {
                moveFrom = *(const int*)p->Data;
                moveTo = i;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SetCursorScreenPos(ImVec2(rowPos.x, rowPos.y + rowH));
        ImGui::PopID();
    }

    // 拖拽排序的结果要等**退出循环**再执行。
    //
    // 原因是个很容易踩的坑：拖动过程中 ImGui 仍然持有 source 那个 item 的指针，
    // 如果在循环里就地 moveLayer（会搬动整个 vector、让 Layer& 悬空），
    // ImGui 后续访问就会读到已经失效的内存。
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo) {
        a.history.beginAll(a.doc);
        a.doc.moveLayer(moveFrom, moveTo);
        a.history.commit(a.doc, "重排图层");
        a.lastAction = "重排图层";
        a.lightDirty = true;
        a.layerPanelDirty = true;
    }

    ImGui::EndChild();
    ImGui::TextDisabled("拖拽行可排序；O 眼睛 / L 锁定。列表最上面 = 叠在最上层");

    // 提示本轮"未实现"的部分，避免用户以为合并/混合模式丢了
    ImGui::TextDisabled("混合模式目前只有「正常」——层间影响由光照承担，见下方光照分区");
}

}  // namespace pb::app
