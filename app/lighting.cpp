// app/lighting.cpp — 把"逐层照亮 + 合成"接到应用上
//
// 这是模型 B 在应用侧的落地，也是"伪 3D"实际发生的地方：
//
//   对每个图层 i：
//     1. 用 core/composite.h 在 CPU 侧算好两张通道（纯函数、可 headless 测试）：
//          occlAbove_i —— 第 i 层上方各层的累计遮盖（→ 上层给下层投影）
//          emitFor_i   —— 其他层透过中间层照到第 i 层的光（→ 层间互相照亮）
//     2. 把这一层的 albedo / 材质 / 两张通道传到 GPU
//     3. 跑一遍平面内光照（sweep 或 path trace），结果写进结果数组纹理的第 i 层
//   最后 LayerComposite 把各层按图层不透明度叠成最终画面。
//
// ## 为什么只重算"脏层"
//
// 每多一层就多一整个光照 pass。如果每次改动都重算全部图层，5 个图层就是 5 倍开销。
// 所以这里记录每一层的内容版本（Layer::revision）以及可见性/不透明度，
// 只有真正变了的层才重算 —— 画一笔通常只影响当前图层，代价与单层时一样。
#include <cstdio>
#include <vector>

#include "app/app.h"
#include "core/composite.h"

namespace pb::app {

namespace {

// 每层的"光照输入指纹"：内容版本 + 参与合成的元数据。
// 只要它没变，这一层的受光结果就还能用，不必重算。
struct LayerSignature {
    uint32_t revision = 0;
    bool visible = true;
    uint8_t opacity = 255;
};

std::vector<LayerSignature> g_last;
bool g_hasLast = false;

LayerSignature sigOf(const Layer& L) {
    LayerSignature s;
    s.revision = L.revision;
    s.visible = L.visible;
    s.opacity = L.opacity;
    return s;
}

bool sameSig(const LayerSignature& a, const LayerSignature& b) {
    return a.revision == b.revision && a.visible == b.visible &&
           a.opacity == b.opacity;
}

// 每层最多参与光照（GPU 数组纹理的层数上限）。超出部分不参与光照，
// 但仍然会出现在图层列表里（由 UI 给出提示）。
constexpr int kMaxLayers = gfx::LightSweep::kMaxLayers;

}  // namespace

int lightingLayerCap() { return kMaxLayers; }

float opacityOf(const App& a, int index) {
    if (index < 0 || index >= a.doc.layerCount()) return 1.0f;
    const Layer& L = a.doc.layer(index);
    if (!L.visible) return 0.0f;   // 隐藏层不参与合成（= 不透明度 0）
    return L.opacity / 255.0f;
}

void resetLightingCache() {
    g_last.clear();
    g_hasLast = false;
}

void syncLightDirty(App& a) {
    const int n = a.doc.layerCount();
    const int nGpu = n < kMaxLayers ? n : kMaxLayers;
    a.lightDirtyLayers.assign((size_t)nGpu, 0);

    const bool structureChanged = !g_hasLast || (int)g_last.size() != nGpu;
    if (structureChanged) {
        a.lightAllLayers = true;
        g_last.assign((size_t)nGpu, LayerSignature{});
        for (int i = 0; i < nGpu; i++) g_last[(size_t)i] = sigOf(a.doc.layer(i));
        g_hasLast = true;
        for (int i = 0; i < nGpu; i++) a.lightDirtyLayers[(size_t)i] = 1;
        return;
    }

    if (a.lightAllLayers) {
        for (int i = 0; i < nGpu; i++) {
            a.lightDirtyLayers[(size_t)i] = 1;
            g_last[(size_t)i] = sigOf(a.doc.layer(i));
        }
        return;
    }

    for (int i = 0; i < nGpu; i++) {
        const LayerSignature now = sigOf(a.doc.layer(i));
        if (!sameSig(now, g_last[(size_t)i])) {
            a.lightDirtyLayers[(size_t)i] = 1;
            g_last[(size_t)i] = now;
        }
    }
}

// 把"哪几层要重算"变成"是不是要全部重算"。
// 由几个会改动整份文档的地方调用（新建画布 / 撤销 / 调光照参数 …）。
void markAllLayersForLight(App& a) {
    a.lightAllLayers = true;
    g_hasLast = false;
}

bool anyLayerDirty(const App& a) {
    if (a.lightAllLayers) return true;
    for (uint8_t v : a.lightDirtyLayers)
        if (v) return true;
    return false;
}

bool runLighting(App& a) {
    const int n = std::min(a.doc.layerCount(), kMaxLayers);
    if (n <= 0) return false;
    const int W = a.doc.width();
    const int H = a.doc.height();
    const Rect all{0, 0, W - 1, H - 1};

    // 至少要有东西要算。path trace 每帧都要累积，所以它另说（见下）。
    const bool anyDirty = anyLayerDirty(a);

    if (a.lightMode == LightMode::Sweep) {
        if (!a.sweep) return false;
        if (!anyDirty) return false;   // 没有变化就不重算，省 GPU

        a.sweep->setLayerCount(n);
        for (int i = 0; i < n; i++) a.sweep->uploadLayerOpacity(i, opacityOf(a, i));

        // 两张逐层通道只在这些层要重算时才需要重算 —— 它们由所有层决定，
        // 但只被"正在被照亮的那一层"使用，所以按需算即可。
        static std::vector<uint8_t> occl;
        static std::vector<RGBA8> emit;
        for (int i = 0; i < n; i++) {
            if (!a.lightDirtyLayers.empty() && !a.lightDirtyLayers[(size_t)i]) continue;
            buildOcclAbove(a.doc, i, all, occl);
            buildEmitFor(a.doc, i, all, emit);
            a.sweep->uploadLayerChannels(i, occl.data(), emit.data(), all);
            a.sweep->uploadLayer(i, a.doc.layer(i).albedo, a.doc.layer(i).material.data(),
                                 all);
            a.sweep->renderLayer(i);
        }
        a.sweep->composite();
    } else {
        if (!a.pt) return false;
        // 路径追踪是"每帧累积一个样本"，所以每一层每帧都要推进。
        // 只有画布真的变了才重置累积（否则会拖影），重置后所有层从头来。
        a.pt->setLayerCount(n);
        for (int i = 0; i < n; i++) a.pt->uploadLayerOpacity(i, opacityOf(a, i));

        static std::vector<uint8_t> occl;
        static std::vector<RGBA8> emit;
        for (int i = 0; i < n; i++) {
            const bool needChannels =
                a.lightAllLayers || (i < (int)a.lightDirtyLayers.size() &&
                                     a.lightDirtyLayers[(size_t)i]);
            if (needChannels) {
                buildOcclAbove(a.doc, i, all, occl);
                buildEmitFor(a.doc, i, all, emit);
                a.pt->uploadLayerChannels(i, occl.data(), emit.data(), all);
                a.pt->uploadLayer(i, a.doc.layer(i).albedo,
                                  a.doc.layer(i).material.data(), all);
            }
            a.pt->renderLayer(i);
        }
        a.pt->composite();
    }

    a.lightAllLayers = false;
    for (auto& v : a.lightDirtyLayers) v = 0;
    return true;
}

}  // namespace pb::app
