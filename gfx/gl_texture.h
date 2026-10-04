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

// 把纹理内容读回 CPU（离线渲染出图用）
bool readbackTexture(GLuint tex, int w, int h, RGBA8* out);

}  // namespace pb::gfx
