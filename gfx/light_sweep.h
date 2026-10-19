// gfx/light_sweep.h — 方向扫描式全局光照（实时模式），**逐层**
//
// 从原 renderer.cpp 的 renderScanline() 移植而来，是"实时 GI"这条路线：
// 每一帧沿 N 个方向各做一次扫描线累积，把自发光与透光叠加成柔和的泛光。
// 特点是实时（可随画笔逐帧重算），做不出反射——那属于路径追踪模式。
//
// ## 逐层是怎么进来的
//
// 管线是**先逐层照亮、再合成**：
//
//     对每个图层 i：
//        扫一遍平面内的光（只看第 i 层自己的像素与材质）
//        -> 得到"第 i 层受光后的颜色"，同时留下这一层的**光场**
//     再由 LayerComposite 把各层的光场累加、乘到下面各层的颜色上
//
// 所以 gfx 层不需要知道图层栈的存在：它只认识"若干张同尺寸的平图"，
// 以及"哪一层"这个 uniform。
//
// ## 为什么用数组纹理
//
// 一层的 albedo / 材质各存成 GL_TEXTURE_2D_ARRAY 的一层，着色器用
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
#include "denoise.h"
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

    // ------------------------------------------------------------------
    // 方向扩散（"相邻 Bresenham 射线互相交换携带的光"）
    // ------------------------------------------------------------------
    //
    // 解决的问题：用有限个离散方向近似角度积分，会产生**方向性辐条**
    // （细光源周围射出的一圈放射状条纹）。让相邻射线每走 K 步交换一部分
    // 携带的光，等价于在角度维做一次扩散，把辐条之间的角度空隙填上。
    //
    //   diffusion     w：交换比例。L_i <- (L_i + w*L_{i-1} + w*L_{i+1})/(1+2w)
    //                   权重和恒为 1，所以**总能量守恒**（实测平均亮度不变）
    //   diffusionStep K：每走多少步交换一次
    //
    // 实测（384x216，相对 R=128 的误差）：w=0.1 / K=8 时
    //   point -36%、grid -52%、block -24%、line +3%（可忽略）
    //   而耗时 15.0ms vs R=128 的 24.6ms —— **比直接加大半径划算**
    //
    // ⚠️ 细长光源对 w 敏感：w 太大（如 K=1 时 w≥0.05）会把线的锐利度抹掉。
    //    默认值取的是"四个测试场景都不变差"的那一档。
    float diffusion = 0.0f;
    int diffusionStep = 8;
    // 扩散要换一个 kernel（要走逐射线暂存），所以单独一个开关。
    // 关掉时走原来的 kernel，结果与加这个功能之前**逐位相同**。
    bool diffusionOn = false;
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

    // 第 layer 层的不透明度（0..1），供合成用
    void uploadLayerOpacity(int layer, float opacity);

    // --- 渲染 ---
    //
    // 只算第 layer 层（把它写进内部的结果数组纹理的那一层）。
    // 逐层调用 + 最后 composite() 就是完整管线。
    void renderLayer(int layer);

    // 把已经算好的各层光结果按图层不透明度合成到 resultTexture()。
    void composite();

    // ------------------------------------------------------------------
    // 渐进哈希采样（可选路径，**不是**路径追踪）
    //
    // 确定性扫描是"每帧把 D 个方向全扫一遍"，成本 Θ(D·W·H)、瓶颈是原子加
    // （实测约 2/3 的时间）。打开这个开关后换成：
    //   **每帧每个像素只采一个方向** —— 由 (哈希(像素) + 帧号·黄金步长) 决定，
    //   从边界走到该像素算出 s_d(p)，再并入滑动平均。于是
    //     * 每个像素只写自己的槽 ⇒ 零原子；
    //     * 一帧一个 dispatch      ⇒ 帧内零 barrier；
    //     * 单帧成本 ≈ 0.1–0.35× 一次确定性扫描；
    //     * 代价：要多帧累积，**第 D 帧才与确定性扫描精确相等**。
    //
    // ⚠️ 打开后调用方必须**每帧对每一层都调用 renderLayer()**（不能只调脏层），
    //    并在内容变化时调用 resetProgressive()；每帧还要各调一次
    //    beginFrame()/endFrame()（它们负责清累积与推进帧号）。
    // ------------------------------------------------------------------
    void setProgressive(bool on);
    bool progressive() const { return progressive_; }
    // 去噪（à-trous 联合双边，作用在累积值上、写进独立纹理，不污染累积）
    void setDenoise(bool on, float strength = 3.5f, int iters = 3);
    bool denoiseOn() const { return denoise_ && dn_.valid(); }
    void resetProgressive();          // 清累积、帧号归零
    int progressiveSamples() const { return hashFrame_; }
    void beginFrame();                // 每帧一次：必要时清累积
    void endFrame();                  // 每帧一次：帧号 +1

    // 这里原来还有一个 fieldTexture()：每层单独拷一份"光场"（RGBA16F）。
    // 它**从未被采样过** —— 合成的着色器声明了 u_field、绑了，但函数体里的光场
    // 全部取自受光结果里空气像素的颜色。于是每层每帧白拷一整张画布，
    // 还按 kMaxLayers=16 分配（2048² 时 537 MB）。已删除。

    // "上层光场照亮下层"的强度。必须 > 0：0 是合成着色器里"关掉光场模型"的
    // 哨兵值（那条分支是旧的加法混合），这里是**唯一**的合成路径。
    void setFieldStrength(float s) { fieldStrength_ = s; }

    // --- 诊断：导出"每格的角度光场" phi(theta)，用来判断角度域能不能压缩 ---
    //
    // 背景：如果把每格的 k 个方向换成一个低阶角度基（WHT/Fourier/球谐），
    // 就能把 k 大幅降下来。这能不能成立，取决于 phi(theta) 的**角度频谱**：
    //   * 能量集中在前几阶 -> 低阶基够用，压缩可行
    //   * 前几十阶都很平（δ 函数的特征）-> 压不动，低阶基只会把光"抹糊"
    //
    // 这个函数把每个方向**单独**写进一张 [k][W*H] 的浮点缓冲（不做原子加），
    // 调用方回读后做 DFT 看谱。写的是"该方向这条射线在每格沉积的光"，
    // 也就是 phi(theta) 的物理来源。
    //
    // 需要 GL 4.3 的 SSBO，尺寸 k*W*H*4 float —— 320x180/728 方向约 336 MB，
    // 所以只在离线诊断里用（tools/field_spectrum）。
    // --- 诊断：只跑方向表的前 n 个方向（0 = 全部）---
    //
    // 用途：把"并行度"和"每条射线的长度"分开 —— 方向数变了但射线长度不变。
    // 这是判断内核缺并行度（延迟受限）还是缺算力的直接手段：
    // 耗时随方向数**成比例**下降 = GPU 还没喂饱、被延迟拖住；
    // 降到某点就不动 = 那里撞上固定开销。
    void setDirectionLimit(int n) { dirLimit_ = std::max(0, n); }
    int directionLimit() const { return dirLimit_; }

    // 诊断：只跑"角度落在某个扇区"的方向（0=不过滤，1=轴向，2=斜向）。
    //
    // 用途：验证扫描是不是**内存局部性受限**。
    //   轴向（dx 或 dy 为 0）在纹理上顺序访问，一条射线只碰少数 cache line；
    //   斜向每走一步都跨行，几乎每步都换 cache line（720 字节/行）。
    // 同一份数据、同样步数、同一段代码，**唯一差异是访问模式** ——
    // "轴向每方向成本 << 斜向"就说明局部性是瓶颈（此时你说的"剪切变换让射线
    // 变直"才有意义），否则瓶颈在算力/原子，改布局没用。
    void setDirectionBand(int band) { dirBand_ = band; }
    int directionBand() const { return dirBand_; }

    // --- 角度超采样 ---
    //
    // 把方向表里每个方向按角度细分成 ss 个子方向（射线长度不变）。
    // 用来验证"少方向 + 角度细分"能否逼近"大半径"的效果：
    //   半径 R 的成本 ∝ R（射线更长），而角度细分的成本只 ∝ 射线条数。
    // ss=1 为关闭（默认）。改这个值会让方向表失效重建。
    void setAngularSS(int ss) {
        const int v = std::max(1, ss);
        if (v == ss_) return;
        ss_ = v;
        dirRadius_ = -1;   // 让 ensureDirectionTable 重建
        dirCount_ = 0;
    }
    int angularSS() const { return ss_; }

    // --- 相邻射线扩散（B1/B2 实验）---
    //
    // 让同一方向的**相邻射线**每走 diffuseStep 步，把携带的光互相混合 diffuse 比例。
    // 等价于在角度维加一个扩散项，用来填补离散方向之间的角度空隙
    // （R=32 时表现为从光源发出的放射状"辐条"）。
    //
    //   diffuse = 0    关闭（走原来的 kernel，结果与改动前逐位相同）
    //   diffuse = 1    完全取邻居平均（B1）
    //   diffuse = 小值 保守混合（B2：只轻轻抹平，避免削掉薄光源的尖峰）
    //
    // 代价：走这条路径时是**逐方向 dispatch**，且每 diffuseStep 步一次 barrier。
    // 写进 settings() —— 面板与诊断工具改的是同一份状态，不要另设一份，
    // 否则 renderLayer 里"从 settings 同步"那步会把这里的值覆盖掉。
    void setDiffusion(float amount, int step) {
        st_.diffusion = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
        st_.diffusionStep = std::max(1, step);
    }
    float diffusion() const { return st_.diffusion; }
    int diffusionStep() const { return st_.diffusionStep; }
    void setDiffusionDbg(int m) { diffuseDbg_ = m; }   // 诊断用，见 kernel 注释
    void setDiffusionEnable(bool on) { st_.diffusionOn = on; }

    // 诊断：取样地址改用"行内连续"模式（模拟剪切布局的访问模式）。
    // 沉积地址仍按 DDA，所以步数/取样数/原子数完全一致，只有地址连续性变了。
    void setContiguousFetch(bool on) { contiguousFetch_ = on; }
    bool contiguousFetch() const { return contiguousFetch_; }

    // --- 单层便捷入口 ---
    //
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
    void blendLayer(int layer);              // scanTex_ -> litArray_ 的第 layer 层
    void renderLayerProgressive(int layer);  // 哈希采样路径
    void clearAccumLayer(int layer);

    int w_ = 0, h_ = 0;
    int layers_ = 0;
    // "上层光场照亮下层"的强度（见 setFieldStrength）
    float fieldStrength_ = 1.0f;
    LightSettings st_;

    // --- 输入（每层一层）---
    GLuint canvasArray_ = 0;    // RGBA8  : albedo.rgb + 覆盖率.a
    GLuint matArray_ = 0;       // RGBA32F: 自发光 RGB + (ior + flags*8)

    // --- 中间与输出 ---
    GLuint scanTex_ = 0;        // RGBA16F: 单层的扫描累积结果（逐层复用）
    GLuint litArray_ = 0;       // RGBA8  : 每层的受光结果
    GLuint opacityArray_ = 0;   // R32F   : 每层的不透明度（合成用）
    GLuint renderTex_ = 0;      // RGBA8  : 合成后的最终输出
    GLuint scanSSBO_ = 0;       // R32UI 定点累积缓冲（原子加）
    GLuint dirSSBO_ = 0;        // ivec2 方向表

    // --- 渐进哈希采样（可选路径）---
    bool progressive_ = false;
    bool denoise_ = false;
    float denoiseStrength_ = 3.5f;
    int denoiseIters_ = 3;
    int hashFrame_ = 0;              // 已累积的帧数（0 = 还没有样本）
    int hashStride_ = 1;             // 哈希轮换步长（与方向数互素，见 ensureDirectionTable）
    bool hashNeedsClear_ = true;     // 累积缓冲需要在下一帧清零
    GLuint hashAccum_ = 0;           // RGBA16F 数组：RGB=E[lt]，A=E[luma²]
    GLuint hashDenoise_ = 0;         // RGBA16F 数组：去噪结果（独立纹理，不污染累积）
    GLuint hashProg_ = 0, clearAccumProg_ = 0, hashCopyProg_ = 0;
    Denoise dn_;

    // 各层合成用（一个 compute pass，见 layer_composite.h）
    LayerComposite comp_;

    GLuint scanProg_ = 0, scanDiffProg_ = 0, copyProg_ = 0, blendProg_ = 0;

    // 缓存的 uniform 位置（每帧都要设，避免反复字符串查找）
    struct {
        GLint cc = -1, mat = -1, cs = -1, ndirs = -1, cp = -1;
        GLint ambient = -1, emitScale = -1, layer = -1;
        GLint band = -1, bandn = -1;   // 诊断：扇区过滤（见 setDirectionBand）
        GLint contig = -1;             // 诊断：连续取样（见 setContiguousFetch）
        // --- 扩散版 program 的同一批 uniform（后缀 _d）---
        GLint cc_d = -1, mat_d = -1, cs_d = -1, ndirs_d = -1, cp_d = -1;
        GLint ambient_d = -1, emitScale_d = -1, layer_d = -1;
        GLint band_d = -1, bandn_d = -1, contig_d = -1;
        GLint diff = -1, diffstep = -1, diffdbg = -1;
    } uScan_;
    struct {
        GLint cs = -1;
    } uCopy_;
    struct {
        GLint cc = -1, scan = -1, mat = -1, cs = -1, emitScale = -1, layer = -1;
    } uBlend_;
    struct {
        GLint cc = -1, mat = -1, cs = -1, ndirs = -1, frame = -1, ambient = -1, cp = -1,
              emitScale = -1, layer = -1, stride = -1;
    } uHash_;
    struct {
        GLint cs = -1, layer = -1;
    } uClearAccum_;
    struct {
        GLint cs = -1, layer = -1, src = -1;
    } uHashCopy_;

    // 扩散交换用的全局暂存（[方向][射线] 各一个 vec4）。见 kScanDiffComp。
    GLuint carrySSBO_ = 0;
    size_t carryCap_ = 0;                      // 已分配容量（元素数）
    static constexpr int kCarryRays = 65536;   // 每方向射线数上限（与 shader 的 HRAYS 一致）

    int dirRadius_ = -1;  // 方向表当前对应的半径，用于判断是否需要重建
    int dirCount_ = 0;
    int dirLimit_ = 0;    // > 0 时只跑前这么多个方向（诊断用，见 setDirectionLimit）
    int dirBand_ = 0;     // 0=不过滤 1=只轴向 2=只斜向（诊断用，见 setDirectionBand）
    float diffuse_ = 0.0f;   // 相邻射线扩散比例（0=关，见 setDiffusion）
    int diffuseStep_ = 8;    // K：每多少步交换一次
    int diffuseDbg_ = 0;     // 诊断：0正常 1只写不读 2完全不混（见 setDiffusion）
    // 扩散路径的总开关。**默认关闭**：改写后的 kernel 与原来不等价
    // （沉积能量差 ~10 倍），原因尚未定位。见 light_sweep.cpp 里的说明。
    bool diffuseEnable_ = false;
    int ss_ = 1;          // 角度超采样倍率（1=关，见 setAngularSS）
    int dirSS_ = 1;       // 方向表**建成时**用的 ss（用于判断是否需要重建）
    bool contiguousFetch_ = false; // 诊断：连续取样（见 setContiguousFetch）
};

}  // namespace pb::gfx
