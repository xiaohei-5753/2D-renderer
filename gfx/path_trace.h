// gfx/path_trace.h — 逐像素 2D 路径追踪（高质量模式）
//
// 与 light_sweep 的分工：
//   light_sweep  方向扫描式 GI：每帧沿 N 个固定方向扫一遍，速度极快，
//                只有"穿过"与"挡住"两种交互，光呈**方向性条纹**。
//   path_trace   每像素投一条抖动射线，可反射/漫反射/折射，物理上更正确，
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
// 只依赖 OpenGL，不依赖 GLFW。
#pragma once

#include "gl_common.h"

#include "core/image.h"
#include "core/material.h"
#include "core/types.h"

namespace pb::gfx {

struct PathTraceSettings {
    // 每条射线最多走多少格。太小会让远处漏光，太大只是浪费
    int maxSteps = 220;
    // 最多几次弹射（反射/折射/漫反射）。1 = 只有直接光照
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
    bool valid() const { return accumTex_ != 0; }

    int width() const { return w_; }
    int height() const { return h_; }

    PathTraceSettings& settings() { return st_; }
    const PathTraceSettings& settings() const { return st_; }

    void upload(const Image& albedo, const Material* mats, const Rect& dirty);
    void uploadAll(const Image& albedo, const Material* mats);

    // 画布变了就必须重置，否则旧样本会和新几何混在一起（拖影）
    void resetAccumulation();
    int sampleCount() const { return sampleIndex_; }
    bool converged() const { return sampleIndex_ >= kConvergedSamples; }

    // 累积一帧
    void render();

    // 已解析（曝光+色调映射）的显示用纹理，RGBA8
    GLuint resultTexture() const { return displayTex_; }
    // 原始画布（关灯时显示）
    GLuint albedoTexture() const { return albedoTex_; }

    static constexpr int kConvergedSamples = 256;

private:
    int w_ = 0, h_ = 0;
    PathTraceSettings st_;

    GLuint albedoTex_ = 0;   // RGBA8: rgb=albedo a=覆盖率
    GLuint matTex_ = 0;      // RGBA8: 打包后的材质
    GLuint accumTex_ = 0;    // RGBA16F: 累积结果
    GLuint displayTex_ = 0;  // RGBA8: 解析后的显示用

    GLuint traceProg_ = 0, resolveProg_ = 0, clearProg_ = 0;

    struct {
        GLint alb = -1, mat = -1, cs = -1, emit = -1, amb = -1, steps = -1,
              bounces = -1, frame = -1, base = -1;
    } uTrace_;
    struct {
        GLint src = -1, cs = -1, exposure = -1, tonemap = -1;
    } uResolve_;
    struct {
        GLint cs = -1;
    } uClear_;

    int sampleIndex_ = 0;
    // 累积缓冲的清零要真正动 GPU，所以只做标记，等 render() 里再执行
    bool needsClear_ = true;
};

}  // namespace pb::gfx
