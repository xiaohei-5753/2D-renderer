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
    // ⚠️ 这里有一个非常容易搞错、而且一搞错就"泛光整片消失"的语义区别 ——
    // 它花了我很久才理清，所以写清楚：
    //
    //   alphaIsCoverage = true  —— alpha 就是**覆盖率**，直接决定这一层盖住下面多少。
    //       未受光的原始 albedo 走这条（那里的 alpha 就是"这个像素有多不透明"）。
    //
    //   alphaIsCoverage = false —— alpha 是**可见度**（1-覆盖率），要取反才是覆盖率。
    //       逐层照亮的**受光结果**走这条。
    //
    // 为什么受光结果要用"可见度"这种别扭的表示？因为它的 alpha 必须回答一个
    // 与 albedo 不同的问题：
    //
    //   albedo 的 alpha 回答"这个像素有多不透明" —— 空气是 0，就是空的，露出下层。
    //   受光结果的 alpha 回答"这一层的颜色要盖住下面多少" —— 空气的覆盖率是 0，
    //       但它带着这一层贡献的**光晕**，必须把颜色画上去。
    //
    // 如果受光结果也按 albedo 的语义解释，空气就会被当成"什么都没画"而整片丢掉，
    // 于是泛光全部消失、画面只剩几条线（实测：平均亮度 73 -> 0.47）。
    // 所以受光结果的 alpha 存 1-cov，合成时取反回 cov —— 空气的覆盖率因此是 1，
    // 它的光晕就被完整保留。
    //
    // 详见 layer_composite.cpp 顶部的推导。
    //
    // backdrop 是所有层都没盖住时显示的底色，可为 nullptr（= 黑）。
    void composite(GLuint layersTex, GLuint opacityTex, int layerCount, int w, int h,
                   GLuint outTex, bool alphaIsCoverage,
                   const float backdrop[3] = nullptr);

private:
    GLuint prog_ = 0;
    struct {
        GLint tex = -1, opacity = -1, cs = -1, n = -1, alphaIsCov = -1, backdrop = -1;
    } u_;
};

}  // namespace pb::gfx
