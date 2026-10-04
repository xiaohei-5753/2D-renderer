// gfx/hud.h — 屏幕空间的极简 HUD（纯色矩形）
//
// P1 阶段还没有 UI 框架（Dear ImGui 在 P3 接入），但一个看不见当前颜色和笔刷大小的
// 画板是没法用的。这里只做两件事：把顶点攒成一批三角形，然后一次画完。
//
// 刻意只提供"填充矩形"和"描边矩形"两个原语——像素风界面需要的色板条、选区框、
// 笔刷光标，用这两个就够拼出来了。坐标以**帧缓冲像素**为单位，原点在左上角。
//
// P3 接入 ImGui 后，这个模块仍然有用：笔刷光标和选区框画在画布图层上更自然，
// 而 ImGui 负责面板。
#pragma once

#include <cstdint>
#include <vector>

#include "core/types.h"
#include "gl_common.h"

namespace pb::gfx {

class Hud {
public:
    Hud() = default;
    ~Hud();

    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    bool init();
    void shutdown();
    bool valid() const { return prog_ != 0; }

    // 一帧的绘制范围。begin 会设定视口、开启混合、清空批次。
    void begin(int fbW, int fbH);
    void end();  // 提交所有已攒的顶点

    void fillRect(float x, float y, float w, float h, RGBA8 c);
    void rectOutline(float x, float y, float w, float h, float thickness, RGBA8 c);

    // 统计（诊断用）
    int lastVertexCount() const { return lastVertexCount_; }

private:
    struct Vertex {
        float x, y;
        uint8_t r, g, b, a;
    };

    void pushQuad(float x0, float y0, float x1, float y1, RGBA8 c);
    void flush();

    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLint uView_ = -1;
    int fbW_ = 0, fbH_ = 0;
    int lastVertexCount_ = 0;
    std::vector<Vertex> verts_;
};

}  // namespace pb::gfx
