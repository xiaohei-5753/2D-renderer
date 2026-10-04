// core/image.h — RGBA8 图像缓冲 + PNG 导出
//
// 不依赖 OpenGL。用于：文档像素存储、离屏渲染回读结果、导出成 PNG。
#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace pb {

class Image {
public:
    Image() = default;
    Image(int w, int h) { resize(w, h); }

    // 改变尺寸并**清空**内容（不是裁切旧缓冲）。albedo 清成全透明。
    void resize(int w, int h);
    void fill(RGBA8 c);

    int width() const { return w_; }
    int height() const { return h_; }
    bool empty() const { return px_.empty(); }
    size_t pixelCount() const { return px_.size(); }

    RGBA8* data() { return px_.data(); }
    const RGBA8* data() const { return px_.data(); }

    // 不做边界检查，调用方负责保证 x/y 合法（内层循环里省掉两次比较）
    RGBA8& at(int x, int y) { return px_[(size_t)y * (size_t)w_ + (size_t)x]; }
    const RGBA8& at(int x, int y) const { return px_[(size_t)y * (size_t)w_ + (size_t)x]; }

    // 越界安全的写入，供画笔等可能越界的调用方使用
    bool setSafe(int x, int y, RGBA8 c) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return false;
        at(x, y) = c;
        return true;
    }

private:
    int w_ = 0, h_ = 0;
    std::vector<RGBA8> px_;
};

// 写出 PNG（内部使用 stb_image_write）。失败时返回 false 并打印原因。
bool writePNG(const std::string& path, const Image& img);
bool writePNG(const std::string& path, int w, int h, const uint8_t* rgba);

}  // namespace pb
