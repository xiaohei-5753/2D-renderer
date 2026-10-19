// tools/common.h — tools/ 下几个诊断程序**确实重复**的那点样板
//
// 这里只放三件东西：
//   1. 隐藏窗口的 GL 上下文创建（render_cli / scan_dump / comp_test 各抄了一遍）
//   2. "WxH" 尺寸解析（render_cli 有具名函数，scan_dump 内联抄了同一行 sscanf）
//   3. Rec.709 亮度统计（scan_dump 回读后统计，png_stat 解码后统计，权重抄了 4 遍）
//
// 刻意**不**放进来的东西（看着像重复、其实语义不同，硬合并会改行为）：
//   - 场景构造：render_cli 的 neon 与 scan_dump 的 "neon" 是两套不同的坐标与材质，
//     合并会直接改掉 scan_dump 的输出像素（基线均值 74.03）。
//   - 命令行参数表：render_cli 的 --out 是"输出文件"，scan_dump 的 --out 是"文件名前缀"，
//     参数名相同而含义不同；--size/--radius 也只是名字相同。各自的用法文案与错误
//     提示都是文档依赖的输出，留在原处。
//   - png_stat 自带的 inflate + PNG 逆滤波：那是它独立实现的解码器，不是重复代码。
//   - PNG 写出：已经由 core/image.h 的 writePNG 提供，不需要再包一层。
//
// 全部是 inline：build.ps1 把 tools/*.cpp 各自编译成一个可执行文件，多一个
// common.cpp 就会多出一个没有 main 的链接目标，所以这里头文件即实现。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "core/image.h"      // pb::Image / pb::RGBA8
#include "gfx/gl_context.h"  // pb::gfx::ContextDesc / pb::gfx::GLContext

namespace pb::tools {

// ---------------------------------------------------------------------------
// GL 上下文
// ---------------------------------------------------------------------------
// tools 下的离线程序一律"隐藏窗口 + 离屏渲染"（GLFW_VISIBLE=false），差异只有
// 窗口尺寸和标题。各工具自己的失败文案（"[cli] 无法创建 OpenGL 上下文…" /
// "no context"）留在各自的 main 里，这里不产出任何输出。
inline bool createOffscreenContext(gfx::GLContext& ctx, int w, int h, const char* title) {
    gfx::ContextDesc desc;
    desc.width = w;
    desc.height = h;
    desc.visible = false;
    desc.title = title;
    return ctx.create(desc);
}

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------
// "320x180" -> w=320 h=180。sscanf 成功且宽高为正才算通过。
inline bool parseSize(const char* s, int& w, int& h) {
    return std::sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

// ---------------------------------------------------------------------------
// 亮度统计
// ---------------------------------------------------------------------------
// Rec.709 亮度权重。tools 下所有"平均亮度"都必须走这一个公式。
inline double luma(uint8_t r, uint8_t g, uint8_t b) {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
inline double luma(const RGBA8& c) { return luma(c.r, c.g, c.b); }

struct LumaStats {
    double mean = 0.0;
    double max = 0.0;   // 与旧代码一致：从 0 起算，空图时为 0
};

// 逐像素统计亮度。px 是 count 个像素、每像素 ch 个字节（RGB 或 RGBA），
// 只读前 3 个通道：png_stat 解出来的图是 3 或 4 通道，scan_dump 的回读图是 4。
inline LumaStats lumaStats(const uint8_t* px, size_t count, int ch) {
    LumaStats st;
    if (!px || count == 0 || ch < 3) return st;
    double sum = 0.0;
    const size_t stride = (size_t)ch;
    for (size_t i = 0; i < count; i++) {
        const double l = luma(px[i * stride], px[i * stride + 1], px[i * stride + 2]);
        sum += l;
        if (l > st.max) st.max = l;
    }
    st.mean = sum / (double)count;
    return st;
}

// pb::Image 版本（RGBA8，固定 4 字节/像素）
inline LumaStats lumaStats(const Image& img) {
    return lumaStats(reinterpret_cast<const uint8_t*>(img.data()), img.pixelCount(), 4);
}

}  // namespace pb::tools
