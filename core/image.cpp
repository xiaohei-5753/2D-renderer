// core/image.cpp
#include "image.h"

#include <cstdio>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_DEPRECATION
#include "stb_image_write.h"

namespace pb {

void Image::resize(int w, int h) {
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    w_ = w;
    h_ = h;
    px_.resize((size_t)w_ * (size_t)h_);
}

void Image::fill(RGBA8 c) {
    for (auto& p : px_) p = c;
}

bool writePNG(const std::string& path, const Image& img) {
    if (img.empty()) {
        fprintf(stderr, "[png] refuse to write empty image to %s\n", path.c_str());
        return false;
    }
    return writePNG(path, img.width(), img.height(),
                    reinterpret_cast<const uint8_t*>(img.data()));
}

bool writePNG(const std::string& path, int w, int h, const uint8_t* rgba) {
    if (w <= 0 || h <= 0 || !rgba) {
        fprintf(stderr, "[png] invalid arguments (%dx%d)\n", w, h);
        return false;
    }
    // stb 的 stride 参数：0 表示 tightly packed
    const int ok = stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4);
    if (!ok) {
        fprintf(stderr, "[png] failed to write %s\n", path.c_str());
        return false;
    }
    return true;
}

}  // namespace pb
