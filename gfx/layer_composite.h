// gfx/layer_composite.{h,cpp} — 把逐层照亮的结果按图层顺序合成
//
// 这是"模型 B"的最后一步：**先逐层照亮，再合成**（见 core/layer.h 的整体说明）。
//
// 为什么这一步必须留在 GPU 上：逐层照亮的结果是 N 张纹理，回读到 CPU 再合成会让
// 每帧多出"下载 N 张全画布 + 上传 1 张"的往返，而画板每帧都在重算光照。
// 一个 compute pass 就能按普通 source-over 叠完，代价可以忽略。
//
// 合成规则刻意保持**普通 alpha 叠加**：图层语义（顺序 / 不透明度 / 隐藏）因此
// 完全可预期，层间影响只发生在"照亮"那一步。这是模型 B 的核心设计取舍。
#pragma once

#include "gl_common.h"

#include "core/types.h"

namespace pb::gfx {

class LayerComposite {
public:
    LayerComposite() = default;
    ~LayerComposite();

    LayerComposite(const LayerComposite&) = delete;
    LayerComposite& operator=(const LayerComposite&) = delete;

    bool init();
    void shutdown();
    bool valid() const { return prog_ != 0; }

    // 合成到 outTex（RGBA8，尺寸必须与输入一致）。
    //
    //   layersTex : GL_TEXTURE_2D_ARRAY，每层是已经算好的颜色
    //   opacityTex: GL_TEXTURE_2D_ARRAY (R32F)，每层一个不透明度 0..1
    //
    // ## 两个正交的开关（这里我连着栽了两次，所以写成正向公式并说明）
    //
    // 合成一层需要两个**独立**的量：
    //   1. 参与度 part —— 这一层在这里有没有内容（0 就直接看下面那层）
    //   2. alpha      —— 半透明像素要透出下面多少（只在画原始像素时用到）
    //
    // `alphaIsVisibility` 说明数组里的 alpha 是哪种：
    //   true  —— **可见度** = 1-覆盖率。逐层照亮的**受光结果**用它：
    //            空气 1（它的光晕必须留下）、半透明 1-cov、实心 0（盖住下面）。
    //   false —— **覆盖率**。**未受光的原始 albedo** 用它：
    //            空气 0、不透明 255。
    //
    // 关键：这两种数值**本身就都是参与度**，所以代码里 part = alpha，不做取反。
    // 我两次都是在这里多取了一次反，导致"空气被跳过、泛光消失"和
    // "整张图被自己的 alpha 乘暗"。别再取反。
    //
    // `albedoAsOpaque` 决定要不要用 alpha 与背景混合：
    //   true  —— 要。原始 albedo 走这条：**画上去什么颜色就显示什么颜色**，
    //            alpha 只决定透出下面多少（两件事数值相同但作用不同）。
    //   false —— 不要。受光结果走这条：颜色已经把材质与光照算进去了，
    //            再乘一次 alpha 就是二次衰减。
    //
    // backdrop 是所有层都没盖住时显示的底色，可为 nullptr（= 黑）。
    void composite(GLuint layersTex, GLuint opacityTex, int layerCount, int w, int h,
                   GLuint outTex, bool albedoAsOpaque = false,
                   const float backdrop[3] = nullptr,
                   GLuint fieldTex = 0, GLuint albedoTex = 0, float fieldStrength = 0.0f);

private:
    GLuint prog_ = 0;
    struct {
        GLint tex = -1, opacity = -1, cs = -1, n = -1, opaque = -1, backdrop = -1;
        GLint field = -1;         // 每层光场（sampler2DArray）
        GLint albedo2 = -1;       // 每层原始颜色
        GLint fieldStrength = -1;
    } u_;
};

}  // namespace pb::gfx
