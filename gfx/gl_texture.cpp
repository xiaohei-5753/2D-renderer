// gfx/gl_texture.cpp
#include "gl_texture.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace pb::gfx {

GLuint createTexture(GLenum internalFormat, int w, int h, GLenum format, GLenum type) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}
// 3D 纹理：体素空间用。参数与 createTexture 一致，只是多一维深度。
GLuint createTexture3D(GLenum internalFormat, int w, int h, int d, GLenum format,
                       GLenum type) {
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_3D, tex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, (GLint)internalFormat, w, h, d, 0, format, type, nullptr);
    glBindTexture(GL_TEXTURE_3D, 0);
    return tex;
}

GLuint createTextureWithStorage(GLenum internalFormat, int w, int h, int levels) {
    if (levels < 1) levels = 1;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, levels, internalFormat, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void uploadSubImage(GLuint tex, const RGBA8* pixels, int w, int h, const Rect& dirtyIn) {
    if (!pixels || !tex) return;

    Rect r = dirtyIn;
    r.clipTo(w, h);
    if (r.empty()) return;

    const int bw = r.width();
    const int bh = r.height();

    // 打包成紧凑的临时缓冲：只有脏矩形区域，避免整张重新上传
    static std::vector<uint8_t> buf;
    buf.resize((size_t)bw * (size_t)bh * 4);
    for (int y = 0; y < bh; y++) {
        const RGBA8* src = pixels + (size_t)(r.y0 + y) * (size_t)w + (size_t)r.x0;
        uint8_t* dst = buf.data() + (size_t)y * (size_t)bw * 4;
        for (int x = 0; x < bw; x++) {
            dst[x * 4 + 0] = src[x].r;
            dst[x * 4 + 1] = src[x].g;
            dst[x * 4 + 2] = src[x].b;
            dst[x * 4 + 3] = src[x].a;
        }
    }

    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, r.x0, r.y0, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE,
                    buf.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool readbackTexture(GLuint tex, int w, int h, RGBA8* out) {
    if (!tex || w <= 0 || h <= 0 || !out) return false;
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glBindTexture(GL_TEXTURE_2D, 0);
    const GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "[gl] readbackTexture failed: 0x%04X\n", e);
        return false;
    }
    return true;
}

void uploadSubImagePacked(GLuint tex, int x0, int y0, int bw, int bh, const RGBA8* tight) {
    if (!tex || !tight || bw <= 0 || bh <= 0) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE, tight);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void uploadSubImagePackedF(GLuint tex, int x0, int y0, int bw, int bh, const float* tight) {
    if (!tex || !tight || bw <= 0 || bh <= 0) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, bw, bh, GL_RGBA, GL_FLOAT, tight);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------------------
// 2D 纹理数组
// ---------------------------------------------------------------------------

GLuint createTextureArray(GLenum internalFormat, int w, int h, int layers, GLenum format,
                          GLenum type) {
    if (w <= 0 || h <= 0 || layers <= 0) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, internalFormat, w, h, layers, 0, format, type,
                 nullptr);
    // 与画布纹理保持同样的采样约定：NEAREST（像素画必须，否则缩放糊）
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return tex;
}

void uploadArrayLayer(GLuint tex, int layer, int w, int h, const RGBA8* pixels) {
    if (!tex || !pixels || w <= 0 || h <= 0 || layer < 0) return;
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                    pixels);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void uploadArrayLayerSub(GLuint tex, int layer, int x0, int y0, int bw, int bh,
                         const RGBA8* tight) {
    if (!tex || !tight || bw <= 0 || bh <= 0 || layer < 0) return;
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, x0, y0, layer, bw, bh, 1, GL_RGBA,
                    GL_UNSIGNED_BYTE, tight);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void uploadFloatArrayLayer(GLuint tex, int layer, int w, int h, float value) {
    if (!tex || w <= 0 || h <= 0 || layer < 0) return;
    static std::vector<float> buf;
    buf.assign((size_t)w * (size_t)h, value);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, w, h, 1, GL_RED, GL_FLOAT,
                    buf.data());
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

}  // namespace pb::gfx
