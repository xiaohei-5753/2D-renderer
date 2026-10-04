// gfx/viewport.cpp
#include "viewport.h"

#include <algorithm>
#include <cmath>

#include "shader.h"

namespace pb::gfx {

static const char* kVert = R"(#version 330 core
layout(location=0)in vec2 aP;
layout(location=1)in vec2 aU;
out vec2 vU;
void main(){gl_Position=vec4(aP,0,1);vU=aU;})";

static const char* kFrag = R"(#version 330 core
uniform sampler2D u_t;uniform vec2 u_cp;uniform vec2 u_uv;
in vec2 vU;out vec4 oC;
void main(){
  vec2 ab=u_uv*(vU-0.5)+u_cp;
  oC=texture(u_t,ab);
})";

Viewport::~Viewport() { shutdown(); }

bool Viewport::init() {
    shutdown();
    prog_ = createGraphicsProgram("viewport/blit", kVert, kFrag);
    if (!prog_) return false;

    uTex_ = glGetUniformLocation(prog_, "u_t");
    uCenter_ = glGetUniformLocation(prog_, "u_cp");
    uViewUV_ = glGetUniformLocation(prog_, "u_uv");

    // 一个覆盖整个裁剪空间的大三角形（比两个三角形少一次光栅化接缝）
    const float verts[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         3.0f, -1.0f, 2.0f, 0.0f,
        -1.0f,  3.0f, 0.0f, 2.0f,
    };
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return !checkGLError("viewport/init");
}

void Viewport::shutdown() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (prog_) glDeleteProgram(prog_);
    vbo_ = vao_ = prog_ = 0;
    uTex_ = uCenter_ = uViewUV_ = -1;
}

void Viewport::setCanvasSize(int w, int h) {
    cw_ = std::max(1, w);
    ch_ = std::max(1, h);
}

void Viewport::zoomBy(double factor) {
    if (!(factor > 0.0)) return;
    cam_.zoom *= factor;
    cam_.zoom = std::clamp(cam_.zoom, 0.02, 64.0);
}

void Viewport::viewUV(int fbW, int fbH, float& du, float& dv) const {
    const float a = (float)fbW / (float)cw_;
    const float b = (float)fbH / (float)ch_;
    const float m = std::max(a, b);
    if (!(m > 0.0f)) {
        du = dv = 1.0f;
        return;
    }
    double z = cam_.zoom;
    if (!(z > 0.0)) z = 1.0;
    du = (a / m) * (float)z;
    dv = (b / m) * (float)z;
}

void Viewport::screenToCanvasF(double sx, double sy, int winW, int winH, int fbW,
                               int fbH, double& cx, double& cy) const {
    cx = cy = 0.0;
    if (winW <= 0 || winH <= 0) return;
    if (fbW <= 0 || fbH <= 0) {
        fbW = winW;
        fbH = winH;
    }

    const double u = sx / (double)winW;
    const double v = 1.0 - sy / (double)winH;   // 屏幕 Y 向下，UV 的 V 向上

    float du = 1.0f, dv = 1.0f;
    viewUV(fbW, fbH, du, dv);

    const double abx = (double)du * (u - 0.5) + cam_.x;
    const double aby = (double)dv * (v - 0.5) + cam_.y;

    cx = abx * (double)cw_;
    cy = aby * (double)ch_;
}

void Viewport::screenToCanvas(double sx, double sy, int winW, int winH, int fbW, int fbH,
                              int& cx, int& cy) const {
    double fx = 0.0, fy = 0.0;
    screenToCanvasF(sx, sy, winW, winH, fbW, fbH, fx, fy);
    if (winW <= 0 || winH <= 0) {
        cx = cy = 0;
        return;
    }
    cx = std::clamp((int)fx, 0, cw_ - 1);
    cy = std::clamp((int)fy, 0, ch_ - 1);
}

void Viewport::canvasToFramebuffer(double cx, double cy, int fbW, int fbH, double& fx,
                                   double& fy) const {
    fx = fy = 0.0;
    if (fbW <= 0 || fbH <= 0) return;

    float du = 1.0f, dv = 1.0f;
    viewUV(fbW, fbH, du, dv);
    if (!(du > 0.0f) || !(dv > 0.0f)) return;

    const double abx = cx / (double)cw_;
    const double aby = cy / (double)ch_;
    const double u = (abx - cam_.x) / (double)du + 0.5;
    const double v = (aby - cam_.y) / (double)dv + 0.5;

    fx = u * (double)fbW;
    fy = (1.0 - v) * (double)fbH;   // 帧缓冲的 y 轴向下，UV 的 V 向上
}

double Viewport::pixelsPerCell(int fbW, int fbH) const {
    float du = 1.0f, dv = 1.0f;
    viewUV(fbW, fbH, du, dv);
    const double visW = (double)du * (double)cw_;   // 可见范围相当于多少画布像素宽
    if (!(visW > 0.0)) return 1.0;
    return (double)fbW / visW;
}

void Viewport::draw(GLuint texture, int fbW, int fbH) {
    if (!prog_ || fbW <= 0 || fbH <= 0) return;

    glViewport(0, 0, fbW, fbH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    float du = 1.0f, dv = 1.0f;
    viewUV(fbW, fbH, du, dv);

    glUseProgram(prog_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(uTex_, 0);
    glUniform2f(uCenter_, (float)cam_.x, (float)cam_.y);
    glUniform2f(uViewUV_, du, dv);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace pb::gfx
