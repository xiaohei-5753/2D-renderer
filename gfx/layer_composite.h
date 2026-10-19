// gfx/layer_composite.{h,cpp} — 把逐层照亮的结果按图层顺序合成
//
// 这是整条"先逐层照亮、再合成"管线的最后一步（见 core/layer.h 的整体说明）。
//
// 为什么这一步必须留在 GPU 上：逐层照亮的结果是 N 张纹理，回读到 CPU 再合成会让
// 每帧多出"下载 N 张全画布 + 上传 1 张"的往返，而画板每帧都在重算光照。
// 一个 compute pass 就能叠完，代价可以忽略。
#pragma once

#include "gl_common.h"

#include <vector>

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
    bool valid() const { return prog_ != 0 && emitProg_ != 0; }

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
    //
    // ## 自发光的**垂直**传输（materialTex / emitScale）
    //
    // 除了"平面内的光场"，还有一条**垂直**方向的光路：一个发光像素把自己的
    // 亮度沿着视线方向（Z）发出去，途中被**上方**各层的覆盖率吸收，剩下的才是
    // 相机看到的那部分。
    //
    // 原实现漏了这条，于是：下方图层的发光永远照不到上方图层的内容，
    // 而且发光像素的**自遮挡**会把光晕吃掉（`C = A×(F+S)` 在实心像素上 S=0，
    // 光晕只有 1 个像素宽）。
    //
    // 现在用一个独立的小 pass 先算好"垂直传输后的自发光"：
    //
    //     transAbove[i] = Π(k>i) (1 - cov_k × op_k)      第 i 层上方的累计透过率
    //     emit[i]       = 该层自发光 × 不透明度 × 覆盖率 × transAbove[i]
    //
    // 再把它按**与受光结果完全相同的 alpha 规则**叠进主合成。方向相反的两件事
    // （合成自下而上、遮挡自上而下）因此各自都只有几行，不必互相纠缠。
    //
    // materialTex 是 RGBA32F 的逐层材质数组（RGB = 自发光，见 gfx/material_pack.h）。
    // 传 0（或 emitScale <= 0）就不启用这条光路。
    //
    // albedoTex / fieldStrength 是"逐层光场"那一套的开关：albedoTex 非 0 且
    // fieldStrength > 0 才启用（见 gfx/layer_composite.cpp 的公式）。
    // **不需要**另外传一份"光场纹理" —— 光场就是受光结果里空气像素的颜色。
    void composite(GLuint layersTex, GLuint opacityTex, int layerCount, int w, int h,
                   GLuint outTex, bool albedoAsOpaque = false,
                   const float backdrop[3] = nullptr,
                   GLuint albedoTex = 0, float fieldStrength = 0.0f,
                   GLuint materialTex = 0, float emitScale = 0.0f);

private:
    GLuint ensureEmitTexture(int w, int h);

    // 自发光逐层纹理的层数上限。给足上限，图层数变化就不必重建纹理
    // （只有前 n 层会被写入/读取，多余的层不影响结果）。
    static constexpr int kMaxLayers = 16;

    GLuint prog_ = 0;         // 主合成
    GLuint emitProg_ = 0;     // 自发光的垂直传输（逐层）
    GLuint emitAddProg_ = 0;  // 把自发光总和加法叠加到画面上

    GLuint emitTex_ = 0;      // RGBA16F 逐层：垂直传输后到达相机的自发光
    int emitW_ = 0, emitH_ = 0;

    struct {
        GLint tex = -1, opacity = -1, cs = -1, n = -1, opaque = -1, backdrop = -1;
        GLint albedo2 = -1;       // 每层原始颜色
        GLint fieldStrength = -1;
    } u_;
    struct {
        GLint albedo = -1, opacity = -1, material = -1, cs = -1, n = -1, emitScale = -1;
    } uEmit_;
    struct {
        GLint emit = -1, cs = -1;
    } uEmitAdd_;
};

}  // namespace pb::gfx
