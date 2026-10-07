// gfx/light_sweep.h — 方向扫描式全局光照（实时模式），**逐层**
//
// 从原 renderer.cpp 的 renderScanline() 移植而来，是"实时 GI"这条路线：
// 每一帧沿 N 个方向各做一次扫描线累积，把自发光与透光叠加成柔和的泛光。
// 特点是实时（可随画笔逐帧重算），做不出反射——那属于路径追踪模式。
//
// ## 逐层是怎么进来的（模型 B 的落地）
//
// 管线是**先逐层照亮、再合成**：
//
//     对每个图层 i：
//        扫一遍平面内的光（只看第 i 层自己的像素），但两步额外衰减/增益：
//          * 沿途每一步额外乘 (1 - occl_i)     <- 上层给第 i 层投的影
//          * 每一步加上 emit_i                  <- 其他层透过中间层照过来的光
//        -> 得到"第 i 层受光后的颜色"
//     然后按普通 alpha 叠加从下往上合成   -> 最终画面
//
// occl / emit 两张通道由 core/composite.h 在 CPU 侧算好（它是纯函数、可 headless
// 测试），这里只负责上传与使用。这样 gfx 层不需要知道图层栈的存在。
//
// ## 为什么用数组纹理
//
// 一层的 albedo / 材质 / occl / emit 各存成 GL_TEXTURE_2D_ARRAY 的一层，着色器用
// `texelFetch(sampler2DArray, ivec3(x,y,layer), 0)` 直接取。这样"哪一层"只是一个
// 参数（uniform u_layer），不必为每层写一套分支，也不必把 N 张纹理绑到 N 个 unit
// （硬件上限一般只有 16 个）。
//
// 只依赖 OpenGL，不依赖 GLFW，可由任意宿主驱动。
#pragma once

#include "gl_common.h"

#include <vector>

#include "core/image.h"
#include "core/material.h"
#include "core/types.h"
#include "layer_composite.h"

namespace pb::gfx {

struct LightSettings {
    // 方向表的半径。方向数约为 8 * R * (1 - 1/sqrt2)，R 越大光越平滑、越慢。
    int rayRadius = 128;
    // 基础环境光。原实现里这一项同时充当"墙色"（g_wallClr）
    float ambient[3] = {0.25f, 0.25f, 0.25f};
    // 自发光倍率（原 g_emit）
    float emitScale = 1.0f;
    // 沿程增益（原 u_cp，硬编码为 1.0）。>1 会让光在传播中增强
    float propagate = 1.0f;
};

class LightSweep {
public:
    LightSweep() = default;
    ~LightSweep();

    LightSweep(const LightSweep&) = delete;
    LightSweep& operator=(const LightSweep&) = delete;

    // 需要已有一个 current 的 GL 上下文
    bool init(int w, int h);
    void shutdown();
    bool valid() const { return renderTex_ != 0; }

    int width() const { return w_; }
    int height() const { return h_; }

    LightSettings& settings() { return st_; }
    const LightSettings& settings() const { return st_; }

    // 参与光照的层数（= 文档的图层数，但最多 kMaxLayers）。
    //
    // 改变层数会重新分配所有数组纹理。调用方必须在层数变化后重新 upload* 一次，
    // 否则那些层的纹理是未定义内容。
    int layerCount() const { return layers_; }
    void setLayerCount(int n);

    // 所有层都当成"没画东西"（空气 + 不透明），用于退出逐层模式
    void clearAllLayers();

    // --- 上传 ---
    //
    // 只上传 region 指定的矩形。region 为空表示没有变化，直接返回。
    // mats 可为 nullptr（全部按默认材质处理）。
    void uploadLayer(int layer, const Image& albedo, const Material* mats,
                     const Rect& region);

    // occl / emit 是逐像素的整张图（尺寸必须与画布一致，紧凑排列）。
    // occl：0 = 上方完全通透，255 = 完全被盖住
    // emit：其他层透过中间层到达这一层的光（RGB）
    void uploadLayerChannels(int layer, const uint8_t* occl, const RGBA8* emit,
                             const Rect& region);

    // 第 layer 层的不透明度（0..1），供合成用
    void uploadLayerOpacity(int layer, float opacity);

    // --- 渲染 ---
    //
    // 只算第 layer 层（把它写进内部的结果数组纹理的那一层）。
    // 逐层调用 + 最后 composite() 就是模型 B 的完整管线。
    void renderLayer(int layer);

    // 把已经算好的各层光结果按图层不透明度合成到 resultTexture()。
    void composite();

    // --- 单层便捷入口 ---
    //
    // 单层时 occl / emit 恒为 0，整套公式退化成原来的平面内扫描。
    // tools/render_cli 与"只有一个图层"的画布走的都是这条路。
    void uploadAll(const Image& albedo, const Material* mats) {
        uploadLayer(0, albedo, mats, Rect{0, 0, w_ - 1, h_ - 1});
    }
    void render() {
        renderLayer(0);
        composite();
    }

    // 合成后的最终结果纹理（RGBA8，尺寸 = 画布尺寸）。
    // Viewport 与 PNG 回读都用它，所以调用方不必知道图层的事。
    GLuint resultTexture() const { return renderTex_; }

    // 逐层光照结果（GL_TEXTURE_2D_ARRAY）—— 给 LayerComposite 用
    GLuint litTexture() const { return litArray_; }
    GLuint opacityTexture() const { return opacityArray_; }

    // 原始画布数组纹理（未受光）。画板需要能"关灯"看到干净的原图，
    // 否则取色和导出都会被光照污染。
    GLuint canvasTexture() const { return canvasArray_; }

    // 诊断：单层的平面内扫描结果（RGBA16F，2D）。仅供离线诊断读取，不要拿去显示。
    GLuint scanTexture() const { return scanTex_; }

    // 方向表中的方向数量（诊断/性能观察用）
    int directionCount() const { return dirCount_; }

    // 图层数上限。每多一层就多一整个扫描 pass，所以必须有上限。
    static constexpr int kMaxLayers = 16;

private:
    void releaseLayerTextures();
    void ensureLayerTextures();
    void rebuildDirectionTable();
    void ensureDirectionTable();

    int w_ = 0, h_ = 0;
    int layers_ = 0;
    LightSettings st_;

    // --- 输入（每层一层）---
    GLuint canvasArray_ = 0;    // RGBA8 : albedo.rgb + 覆盖率.a
    GLuint matArray_ = 0;       // RGBA32F: 自发光 RGB + (ior + flags*8)
    GLuint occlArray_ = 0;      // RGBA8 : r = 上方累计遮盖
    GLuint emitArray_ = 0;      // RGBA8 : rgb = 其他层照过来的光

    // --- 中间与输出 ---
    GLuint scanTex_ = 0;        // RGBA16F: 单层的扫描累积结果（逐层复用）
    GLuint litArray_ = 0;       // RGBA8 : 每层的受光结果
    GLuint opacityArray_ = 0;   // R32F  : 每层的不透明度（合成用）
    GLuint renderTex_ = 0;      // RGBA8 : 合成后的最终输出
    GLuint scanSSBO_ = 0;       // R32UI 定点累积缓冲（原子加）
    GLuint dirSSBO_ = 0;        // ivec2 方向表

    // 各层合成用（一个 compute pass，见 layer_composite.h）
    LayerComposite comp_;

    GLuint scanProg_ = 0, copyProg_ = 0, blendProg_ = 0;

    // 缓存的 uniform 位置（每帧都要设，避免反复字符串查找）
    struct {
        GLint cc = -1, mat = -1, occl = -1, emit = -1, cs = -1, ndirs = -1, cp = -1;
        GLint ambient = -1, emitScale = -1, layer = -1, crossLayers = -1;
    } uScan_;
    struct {
        GLint cs = -1;
    } uCopy_;
    struct {
        GLint cc = -1, scan = -1, mat = -1, emit = -1, cs = -1, emitScale = -1;
        GLint layer = -1, crossLayers = -1;
    } uBlend_;

    int dirRadius_ = -1;  // 方向表当前对应的半径，用于判断是否需要重建
    int dirCount_ = 0;
};

}  // namespace pb::gfx
