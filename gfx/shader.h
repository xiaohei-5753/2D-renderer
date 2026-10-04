// gfx/shader.h — 着色器编译/链接辅助
#pragma once

#include "gl_common.h"

namespace pb::gfx {

// 编译单个着色器。失败时打印 GLSL 编译日志并返回 0。
GLuint compileShader(GLenum type, const char* src, const char* label);

// 链接一个 compute program。失败返回 0。
GLuint createComputeProgram(const char* label, const char* src);

// 链接一个 vertex+fragment program。失败返回 0。
GLuint createGraphicsProgram(const char* label, const char* vsSrc, const char* fsSrc);

// 打印并清空 GL 错误队列；label 用于定位。返回是否发生过错误。
bool checkGLError(const char* label);

}  // namespace pb::gfx
