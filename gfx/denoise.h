// gfx/denoise.{h,cpp} — à-trous 联合双边去噪（给"渐进哈希采样"用）
//
// ## 为什么需要它
//
// 渐进哈希采样每帧每个像素只采 **一个方向**，于是画面是"每像素 N=1 的角向估计"，
// 噪声很大（点噪声，空间上不相关）。靠多帧累积能把噪声压下去，但每多一帧都是
// 真金白银。后处理去噪能用便宜得多的代价换掉一部分"必须再跑几帧"。
//
// ## 为什么是"联合"双边 + 空洞（à-trous）
//
//  * **空洞**：3 层 5×5、步长 1/2/4 ⇒ 等效核 ≈ 17×17，但每层只有 25 次取样。
//    直接在 17×17 上做双边会是 289 次取样，贵一个数量级。
//  * **联合**：引导图用**覆盖率（画布 alpha）**，它没有噪声。只用光场自己当引导
//    会把噪点当成"细节"保留下来；有了无噪声的几何向导，光晕能平滑、边缘不会糊。
//  * **值域宽度按逐像素噪声方差**：σr = c·√(Var(luma)/N)。噪声大的像素才允许
//    大范围平均、噪声小的地方几乎不动 —— 实测比"按亮度猜"把 1px 结构的残差
//    压下去 3 倍（见 tools/hash_accum.cpp 的 sigma 变体与 out/montage_*.png）。
//
// ## 方差从哪来
//
// 输入纹理的 **alpha 存 E[luma²]**（RGB 是 E[luma]），于是
//     Var(luma) = E[l²] − E[l]² ，均值的标准差 = √(Var/N)
// 一个原本空着的通道就够，不必多一张累积纹理。
//
// ## 不污染累积
//
// 读 src、写 dst（两张独立纹理），调用方让后续通道去读 dst。
// 如果写回 src，就会"每帧对上一帧的去噪结果再滤波"，渐进变糊。
#pragma once

#include "gl_common.h"

namespace pb::gfx {

class Denoise {
public:
    Denoise() = default;
    ~Denoise();

    Denoise(const Denoise&) = delete;
    Denoise& operator=(const Denoise&) = delete;

    bool init(int layers);
    void shutdown();
    bool valid() const { return prog_ != 0; }

    // 把数组纹理 src 的第 layer 层去噪，结果写进 dst 的同一层。
    //
    //   src   : GL_TEXTURE_2D_ARRAY，**fp32 或 fp16 都行**（渐进累积是 fp32）；
    //           alpha 必须是 E[luma²]
    //   guide : GL_TEXTURE_2D_ARRAY，RGBA8（用 .a 当引导，即覆盖率）
    //   dst   : GL_TEXTURE_2D_ARRAY，RGBA16F（与内部临时纹理同格式才能拷）
    //   invN  : 1/已累积样本数（噪声按 1/√N 缩小）
    //   cRange: σr = max(floor, cRange·√(Var/N))；实测甜点 ≈ 3.5
    //   iters : 迭代层数（1..5，空洞步长 1,2,4,8,16）
    void apply(GLuint src, GLuint guide, GLuint dst, int layer, int w, int h, int iters,
               float sigmaG, float cRange, float invN);

private:
    void ensureTemps(int w, int h);

    GLuint prog_ = 0;
    GLuint tmp_[2] = {0, 0};   // RGBA16F 数组纹理，乒乓
    int w_ = 0, h_ = 0;
    int layers_ = 1;

    struct {
        GLint src = -1, guide = -1, cs = -1, layer = -1, step = -1, sigmaG = -1;
        GLint cRange = -1, invN = -1, floorSr = -1;
    } u_;
};

}  // namespace pb::gfx
