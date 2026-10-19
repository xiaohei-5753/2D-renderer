// app/lighting.cpp — 把"逐层照亮 + 合成"接到应用上
//
//   对每个图层 i：
//     1. 把这一层的 albedo / 材质传到 GPU
//     2. 跑一遍**独立的**平面内光照（扫描 GI），结果写进结果数组纹理的第 i 层，
//        同时把这一层的光场留下来
//   最后 LayerComposite 把各层的光场累加、乘到下面各层的颜色上，得到最终画面。
//
// 层与层之间没有耦合：每一层只看自己。所以"新建一个空图层"不会让画面变亮。
//
// （路径追踪分支已于 2026-10-19 移除，现在只有扫描 GI 一条路径。）
//
// ## 为什么只重算"脏层"
//
// 每多一层就多一整个光照 pass。如果每次改动都重算全部图层，5 个图层就是 5 倍开销。
// 所以这里记录每一层的内容版本（Layer::revision）以及可见性/不透明度，
// 只有真正变了的层才重算 —— 画一笔通常只影响当前图层，代价与单层时一样。
#include <cstdio>
#include <vector>

#include "app/app.h"

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
// 上一次认到的图层结构版本（见 Document::structureVersion）
uint32_t g_lastStructure = 0;

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
    g_lastStructure = 0;
}

void syncLightDirty(App& a) {
    const int n = a.doc.layerCount();
    const int nGpu = n < kMaxLayers ? n : kMaxLayers;
    a.lightDirtyLayers.assign((size_t)nGpu, 0);

    // --- 两个"强制全部重算"的来源 ---
    //
    // 1) App::lightDirty：菜单 / 快捷键 / 面板在所有"影响合成但不弄脏像素"的地方
    //    都会置它（切光照、改参数、图层增删移并…）。这个标志以前**只写不读**，
    //    于是"图层上移/下移"这种不改内容、也不改层数的操作不会触发重新光照，
    //    画面停在上一次的图层顺序上，直到用户再画一笔才恢复。现在在这里消费它。
    if (a.lightDirty) {
        a.lightDirty = false;
        a.lightAllLayers = true;
        g_hasLast = false;
    }

    // 2) 图层结构版本：下面的缓存键只有"每层的内容版本 + 可见性 + 不透明度"，
    //    **不含图层身份**。交换两层位置时这三个量在各自的位置上都不变，
    //    缓存会误判成"什么都没变"。所以排列一变就全部重算。
    if (!g_hasLast || g_lastStructure != a.doc.structureVersion()) {
        g_lastStructure = a.doc.structureVersion();
        a.lightAllLayers = true;
        g_hasLast = false;
    }

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
        if (getenv("PB_LIGHT_TRACE"))
            fprintf(stderr, "[light] L%d rev %u->%u vis %d->%d op %d->%d\\n", i,
                    g_last[(size_t)i].revision, now.revision,
                    (int)g_last[(size_t)i].visible, (int)now.visible,
                    (int)g_last[(size_t)i].opacity, (int)now.opacity);
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

    // 至少要有东西要算。渐进模式每帧都要推进，所以它另说（见下）。
    const bool anyDirty = anyLayerDirty(a);

    if (!a.sweep) return false;
    // 渐进哈希采样：每帧都要推进（哪怕没有任何层"脏"），否则永远不收敛
    a.sweep->setProgressive(a.progressive);
    a.sweep->setDenoise(a.denoise, a.denoiseStrength, a.denoiseIters);
    if (!anyDirty && !a.progressive) return false;   // 没有变化就不重算，省 GPU

    a.sweep->setLayerCount(n);
    for (int i = 0; i < n; i++) a.sweep->uploadLayerOpacity(i, opacityOf(a, i));

    // 渐进模式下：内容一变就重开累积（否则旧样本会和新内容混在一起）
    if (a.progressive && (anyDirty || a.lightAllLayers)) a.sweep->resetProgressive();
    a.sweep->beginFrame();

    // 只重算脏层：每层的光照是一次完整 pass，没变就没必要再跑。
    // 但渐进模式下**每层每帧都要推进**（每帧给每层一个新方向样本）。
    for (int i = 0; i < n; i++) {
        const bool dirty =
            a.lightAllLayers ||
            (i < (int)a.lightDirtyLayers.size() && a.lightDirtyLayers[(size_t)i]);
        if (!a.progressive && !a.lightDirtyLayers.empty() && !dirty) continue;
        if (dirty) {
            a.sweep->uploadLayer(i, a.doc.layer(i).albedo, a.doc.layer(i).material.data(),
                                 all);
        }
        a.sweep->renderLayer(i);
    }
    a.sweep->endFrame();
    a.sweep->setFieldStrength(a.fieldStrength);
    a.sweep->composite();

    a.lightAllLayers = false;
    for (auto& v : a.lightDirtyLayers) v = 0;
    return true;
}

}  // namespace pb::app
