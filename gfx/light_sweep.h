// gfx/light_sweep.h — 方向扫描式全局光照（实时模式）
//
// 从原 renderer.cpp 的 renderScanline() 移植而来，是"实时 GI"这条路线：
// 每一帧沿 N 个方向各做一次扫描线累积，把自发光与透光叠加成柔和的泛光。
// 特点是实时（可随画笔逐帧重算），做不出反射/折射——那属于光追模式（P2）。
//
// 只依赖 OpenGL，不依赖 GLFW，可由任意宿主驱动。
#pragma once

#include "gl_common.h"

#include "core/image.h"
#include "core/types.h"

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

    // 画布内容上传。dirty 为空表示没有变化，直接返回。
    void upload(const Image& img, const Rect& dirty);
    void uploadAll(const Image& img);

    // 跑一遍完整的光照管线，结果在 resultTexture()
    void render();

    // 结果纹理（RGBA8，尺寸 = 画布尺寸）
    GLuint resultTexture() const { return renderTex_; }

    // 原始画布纹理（未受光）。画板需要能"关灯"看到干净的原图，
    // 否则取色和导出都会被光照污染。
    GLuint canvasTexture() const { return canvasTex_; }

    // 方向表中的方向数量（诊断/性能观察用）
    int directionCount() const { return dirCount_; }

private:
    void rebuildDirectionTable();
    void ensureDirectionTable();

    int w_ = 0, h_ = 0;
    LightSettings st_;

    GLuint canvasTex_ = 0;   // RGBA8: albedo.rgb + 覆盖率.a
    GLuint scanTex_ = 0;     // RGBA16F: 扫描累积结果
    GLuint renderTex_ = 0;   // RGBA8: 最终输出
    GLuint scanSSBO_ = 0;    // R32UI 定点累积缓冲（原子加）
    GLuint dirSSBO_ = 0;     // ivec2 方向表

    GLuint scanProg_ = 0, copyProg_ = 0, blendProg_ = 0;

    // 缓存的 uniform 位置（每帧都要设，避免反复字符串查找）
    struct {
        GLint cc = -1, cs = -1, amb = -1, ndirs = -1, cp = -1, emit = -1;
    } uScan_;
    struct {
        GLint cs = -1;
    } uCopy_;
    struct {
        GLint cc = -1, scan = -1, cs = -1, emit = -1;
    } uBlend_;

    int dirRadius_ = -1;  // 方向表当前对应的半径，用于判断是否需要重建
    int dirCount_ = 0;
};

}  // namespace pb::gfx
