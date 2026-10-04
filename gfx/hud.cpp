// gfx/hud.cpp
#include "hud.h"

#include <algorithm>

#include "shader.h"

namespace pb::gfx {

static const char* kHudVert = R"(#version 330 core
layout(location=0)in vec2 aPos;
layout(location=1)in vec4 aColor;
uniform vec2 u_view;      // 帧缓冲尺寸（像素）
out vec4 vColor;
void main(){
  // 像素坐标（原点左上，y 向下）-> NDC
  vec2 ndc=vec2(aPos.x/u_view.x*2.0-1.0, 1.0-aPos.y/u_view.y*2.0);
  gl_Position=vec4(ndc,0,1);
  vColor=aColor;
})";

static const char* kHudFrag = R"(#version 330 core
in vec4 vColor;
out vec4 oC;
void main(){ oC=vColor; })";

Hud::~Hud() { shutdown(); }

bool Hud::init() {
    shutdown();
    prog_ = createGraphicsProgram("hud", kHudVert, kHudFrag);
    if (!prog_) return false;

    uView_ = glGetUniformLocation(prog_, "u_view");

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    verts_.reserve(4096);
    return !checkGLError("hud/init");
}

void Hud::shutdown() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (prog_) glDeleteProgram(prog_);
    vbo_ = vao_ = prog_ = 0;
    uView_ = -1;
    verts_.clear();
}

void Hud::begin(int fbW, int fbH) {
    fbW_ = std::max(0, fbW);
    fbH_ = std::max(0, fbH);
    verts_.clear();
    lastVertexCount_ = 0;
}

void Hud::pushQuad(float x0, float y0, float x1, float y1, RGBA8 c) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    if (x1 <= x0 || y1 <= y0) return;

    const Vertex a{x0, y0, c.r, c.g, c.b, c.a};
    const Vertex b{x1, y0, c.r, c.g, c.b, c.a};
    const Vertex d{x1, y1, c.r, c.g, c.b, c.a};
    const Vertex e{x0, y1, c.r, c.g, c.b, c.a};

    verts_.push_back(a);
    verts_.push_back(b);
    verts_.push_back(d);
    verts_.push_back(a);
    verts_.push_back(d);
    verts_.push_back(e);
}

void Hud::fillRect(float x, float y, float w, float h, RGBA8 c) {
    pushQuad(x, y, x + w, y + h, c);
}

void Hud::rectOutline(float x, float y, float w, float h, float thickness, RGBA8 c) {
    if (w <= 0.0f || h <= 0.0f) return;
    const float t = std::clamp(thickness, 1.0f, std::min(w, h));
    fillRect(x, y, w, t, c);                  // 上
    fillRect(x, y + h - t, w, t, c);          // 下
    fillRect(x, y + t, t, h - 2 * t, c);      // 左
    fillRect(x + w - t, y + t, t, h - 2 * t, c);  // 右
}

void Hud::flush() {
    if (verts_.empty() || !prog_ || fbW_ <= 0 || fbH_ <= 0) return;

    glUseProgram(prog_);
    glUniform2f(uView_, (float)fbW_, (float)fbH_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(Vertex)),
                 verts_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts_.size());
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    lastVertexCount_ = (int)verts_.size();
}

void Hud::end() {
    if (!prog_) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    flush();
    glDisable(GL_BLEND);

    verts_.clear();
}

}  // namespace pb::gfx
