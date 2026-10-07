// gfx/gl_texture.h — 纹理与脏矩形上传辅助
#pragma once

#include "gl_common.h"

#include "core/types.h"

namespace pb::gfx {

// 分配一张不可变尺寸的 2D 纹理，NEAREST 采样 + CLAMP_TO_EDGE
// （像素画必须 NEAREST，否则缩放会糊）
GLuint createTexture(GLenum internalFormat, int w, int h, GLenum format, GLenum type);

// 带 mip 层数的纹理（用 glTexStorage2D 分配）
GLuint createTextureWithStorage(GLenum internalFormat, int w, int h, int levels);

// 只上传 dirty 指定的子矩形。dirty 为空则什么都不做。
void uploadSubImage(GLuint tex, const RGBA8* pixels, int w, int h, const Rect& dirty);

// 上传一块**紧凑**数据（bw*bh，不是全图）到纹理的子矩形。
// 用于逐像素打包出来的副通道（例如材质），避免为了上传一小块而准备整张图。
void uploadSubImagePacked(GLuint tex, int x0, int y0, int bw, int bh, const RGBA8* tight);

// 同上，但数据是 4 个 float/像素（材质通道用，见 gfx/material_pack.h）
void uploadSubImagePackedF(GLuint tex, int x0, int y0, int bw, int bh, const float* tight);

// 把纹理内容读回 CPU（离线渲染出图用）
bool readbackTexture(GLuint tex, int w, int h, RGBA8* out);

// ---------------------------------------------------------------------------
// 2D 纹理数组（逐层光照用：每个图层一层）
// ---------------------------------------------------------------------------
//
// 为什么用数组纹理而不是"每层一张纹理 + 着色器里按索引分支"：
// 后者要么得写一堆 if，要么得把 N 张纹理绑定到 N 个 texture unit（上限只有 16+）。
// 数组纹理让着色器用 texelFetch(sampler2DArray, ivec3(x,y,layer), 0) 直接取任意层，
// 一次绑定搞定，而且层数变了只需重新分配。

GLuint createTextureArray(GLenum internalFormat, int w, int h, int layers, GLenum format,
                          GLenum type);

// 上传数组纹理的**某一层**的整层内容（紧凑数据，w*h）
void uploadArrayLayer(GLuint tex, int layer, int w, int h, const RGBA8* pixels);

// 上传数组纹理某一层的某个子矩形（紧凑数据 bw*bh）
void uploadArrayLayerSub(GLuint tex, int layer, int x0, int y0, int bw, int bh,
                         const RGBA8* tight);

// 把一组 float 值写进数组纹理（每层一个 float，存为 R32F）—— 图层不透明度用
void uploadFloatArrayLayer(GLuint tex, int layer, int w, int h, float value);

}  // namespace pb::gfx
