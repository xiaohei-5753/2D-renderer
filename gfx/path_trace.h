// gfx/path_trace.h — 逐像素 2D 路径追踪（高质量模式），**逐层**
//
// 与 light_sweep 的分工：
//   light_sweep  方向扫描式 GI：每帧沿 N 个固定方向扫一遍，速度极快，
//                只有"穿过"与"挡住"两种交互，光呈**方向性条纹**。
//   path_trace   每像素投一条抖动射线，可折射/漫反射，物理上更正确，
//                光是各向同性的，代价也更高。
//
// ## 怎么做到实时
//
// 核心是**每像素每帧只投 1 条射线**，靠时间累积收敛：
//   * 画布在停笔时是静止的，累积 N 帧后等效于每像素 N 个样本 -> 约 1 秒就很干净
//   * 用户一画画就重置累积，于是绘图期间是"1 spp 的噪声预览"，保证响应
//   * 每帧成本固定（1 条射线），不随累积帧数增长
// 这比"一帧内投 64 条射线"要快 64 倍，而画板场景恰好允许这么做。
//
// ## 逐层（模型 B）
//
// 与 sweep 完全一致的思路：每层各自追踪，沿途额外乘 (1 - occl) 表示"被上层遮住"，
// 并加上 emit（其他层透过中间层照过来的光）。因为每像素每帧本来只投一条射线，
// 逐层的额外成本就是**多跑几遍同一个 pass**，而层数通常是 2~5。
//
// 累积缓冲是 RGBA16F 的**数组纹理**（每层一层），所以"重置累积"就是清掉所有层。
//
// 只依赖 OpenGL，不依赖 GLFW。
#pragma once

#include "gl_common.h"

#include <vector>

#include "core/image.h"
#include "core/material.h"
#include "core/types.h"
#include "layer_composite.h"

namespace pb::gfx {

struct PathTraceSettings {
    // 每帧投几条射线（每个像素）。
    //
    // 这是收敛速度最直接的旋钮：噪声幅度按 1/sqrt(N) 下降。
    // 单帧成本只有 2-3 ms（约 400 fps），有大量余量可以换画质：
    // 4 spp/帧 仍能跑到 100 fps 以上，而收敛快 4 倍。
    int samplesPerFrame = 4;
    // 一条射线最多跨越多少格。DDA 会访问射线经过的**每一个**格子，
    // 所以斜穿画布需要的格数比"固定步长"多一倍左右，这个上限要留够。
    int maxSteps = 420;
    // 最多几次弹射（折射/漫反射）。1 = 只有直接光照
    int maxBounces = 3;
    // 逃出画布的射线得到的环境光。
    // 路径追踪里这一项同时决定"空白处有多亮"，所以要比扫描 GI 略高一点。
    float ambient[3] = {0.12f, 0.12f, 0.14f};
    float emitScale = 1.0f;
    // 表面的"基础可见度"。
    //
    // 严格物理下，实体像素的亮度 = albedo × 入射辐照度，而环境光只有 0.1 量级，
    // 于是画上去的任何颜色都会接近全黑——对像素画板不可接受。
    // 所以给表面一个 albedo × albedoBase 的基础项，保证"画什么颜色就看得见什么颜色"，
    // 光传输的结果再叠加在它上面。这是一个刻意的风格化取舍，不是物理量。
    float albedoBase = 0.78f;
    // 曝光与色调映射（累积值可能远超 1，直接截断会丢掉高光）
    float exposure = 1.25f;
    bool tonemap = true;
};

class PathTrace {
public:
    PathTrace() = default;
    ~PathTrace();

    PathTrace(const PathTrace&) = delete;
    PathTrace& operator=(const PathTrace&) = delete;

    bool init(int w, int h);
    void shutdown();
    bool valid() const { return accumArray_ != 0; }

    int width() const { return w_; }
    int height() const { return h_; }

    PathTraceSettings& settings() { return st_; }
    const PathTraceSettings& settings() const { return st_; }

    // 参与追踪的层数（最多 kMaxLayers）。改变层数会重新分配数组纹理。
    int layerCount() const { return layers_; }
    void setLayerCount(int n);

    // --- 上传（与 LightSweep 完全同构）---
    void uploadLayer(int layer, const Image& albedo, const Material* mats,
                     const Rect& region);
    void uploadLayerChannels(int layer, const uint8_t* occl, const RGBA8* emit,
                             const Rect& region);
    // 这一层的不透明度（0..1），供逐层合成用
    void uploadLayerOpacity(int layer, float opacity);
    // 原始 albedo 数组纹理的一层（"关灯看原图"时用）
    void uploadLayerAlbedoOnly(int layer, const Image& albedo, const Rect& region);

    // --- 追踪 ---
    //
    // 每层累积一帧。逐层调用它，最后 composite()。
    // 同一层的累积帧数由 sampleIndex() 统一表示（各层同步推进，所以一个计数就够）。
    void renderLayer(int layer);

    // 把各层累积结果解析（曝光 + 色调映射）后按不透明度合成到 resultTexture()
    void composite();

    // 画布变了就必须重置，否则旧样本会和新几何混在一起（拖影）
    void resetAccumulation();
    int sampleCount() const { return sampleIndex_; }
    bool converged() const { return sampleIndex_ >= kConvergedSamples; }

    // --- 单层便捷入口（tools/render_cli 用）---
    void uploadAll(const Image& albedo, const Material* mats) {
        uploadLayer(0, albedo, mats, Rect{0, 0, w_ - 1, h_ - 1});
    }
    // 累积一帧（单层）。多次调用即可逐步收敛。
    void render() {
        renderLayer(0);
        composite();
    }

    // 已解析（曝光+色调映射）并合成的显示用纹理，RGBA8
    GLuint resultTexture() const { return displayTex_; }
    // 原始画布（关灯时显示）
    GLuint albedoTexture() const { return albedoArray_; }

    // 逐层受光结果（给外部的 LayerComposite 用；内部 composite 也用它）
    GLuint litTexture() const { return litArray_; }
    GLuint opacityTexture() const { return opacityArray_; }

    static constexpr int kConvergedSamples = 256;
    static constexpr int kMaxLayers = 16;

private:
    void releaseLayerTextures();
    void ensureLayerTextures();

    int w_ = 0, h_ = 0;
    int layers_ = 0;
    PathTraceSettings st_;

    GLuint albedoArray_ = 0;   // RGBA8 : rgb=albedo a=覆盖率
    GLuint matArray_ = 0;      // RGBA32F: 打包后的材质
    GLuint occlArray_ = 0;     // RGBA8 : r = 上方累计遮盖
    GLuint emitArray_ = 0;     // RGBA8 : rgb = 其他层照过来的光
    GLuint accumArray_ = 0;    // RGBA16F: 逐层累积结果
    GLuint litArray_ = 0;      // RGBA8 : 解析后的逐层结果
    GLuint opacityArray_ = 0;  // R32F  : 每层不透明度
    GLuint displayTex_ = 0;    // RGBA8 : 合成后的显示用

    LayerComposite comp_;

    GLuint traceProg_ = 0, resolveProg_ = 0, blendProg_ = 0, clearProg_ = 0;

    struct {
        GLint alb = -1, mat = -1, occl = -1, emit = -1, cs = -1, emitScale = -1;
        GLint amb = -1, steps = -1, bounces = -1, frame = -1, base = -1, layer = -1;
        GLint cross = -1;
    } uTrace_;
    struct {
        GLint src = -1, alb = -1, cs = -1, exposure = -1, tonemap = -1, layer = -1;
    } uResolve_;
    struct {
        GLint cs = -1, n = -1;
    } uClear_;

    int sampleIndex_ = 0;
    // 累积缓冲的清零要真正动 GPU，所以只做标记，等 renderLayer() 里再执行
    bool needsClear_ = true;
};

}  // namespace pb::gfx
