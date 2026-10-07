// tools/scan_dump.cpp — 逐层光照的诊断工具：把每个中间阶段dump成 PNG
//
// 为什么需要它：这次逐层化改造里，"最终画面不对"可能出在四个地方
// （扫描 / SSBO->纹理 / 合成单层 / 逐层合成），而最终画面只有一个数字，
// 无法区分。把中间阶段直接dump出来，才能一次定位。
//
// 用法: scan_dump <场景> [--size WxH] [--radius R] [--out 前缀]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/image.h"
#include "core/material.h"
#include "core/types.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/layer_composite.h"
#include "gfx/light_sweep.h"

using namespace pb;

namespace {

// 与 render_cli 的 neon 场景保持一致，便于对照
void buildScene(Canvas& c, const std::string& name) {
    const int W = c.width(), H = c.height();
    c.clear(RGBA8{0, 0, 0, 0}, Material{});
    Material em;
    em.setEmissionFrom(0, 200, 255);
    // 一条霓虹线 + 一个实心块
    c.drawLine(W / 4, H / 4, W - W / 4, H / 4, RGBA8{90, 200, 255, 110}, em);
    c.fillRect(W / 2 - 10, H / 2 - 8, W / 2 + 10, H / 2 + 8, RGBA8{44, 44, 54, 255},
               Material{});
    (void)name;
}

std::string g_prefix = "out/dump";

void dumpTex(const char* tag, GLuint tex, int w, int h) {
    if (!tex) { printf("  %-12s <no texture>\n", tag); return; }
    Image img(w, h);
    if (!gfx::readbackTexture(tex, w, h, img.data())) {
        printf("  %-12s readback FAILED\n", tag);
        return;
    }
    double sum = 0, mx = 0;
    for (size_t i = 0; i < img.pixelCount(); i++) {
        const RGBA8 p = img.data()[i];
        const double l = 0.2126 * p.r + 0.7152 * p.g + 0.0722 * p.b;
        sum += l;
        if (l > mx) mx = l;
    }
    const double mean = img.pixelCount() ? sum / (double)img.pixelCount() : 0.0;
    char path[512];
    snprintf(path, sizeof(path), "%s_%s.png", g_prefix.c_str(), tag);
    writePNG(path, img);
    printf("  %-12s 平均亮度 %7.2f  最大 %6.1f  -> %s\n", tag, mean, mx, path);
}

}  // namespace

int main(int argc, char** argv) {
    int W = 320, H = 180, radius = 128;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &W, &H);
        } else if (std::strcmp(argv[i], "--radius") == 0 && i + 1 < argc) {
            radius = atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            g_prefix = argv[++i];
        }
    }

    gfx::ContextDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.visible = false;
    desc.title = "scan_dump";
    gfx::GLContext ctx;
    if (!ctx.create(desc)) { fprintf(stderr, "no context\n"); return 1; }

    Canvas c(W, H);
    buildScene(c, "neon");
    printf("画布 %dx%d\n", W, H);

    gfx::LightSweep sweep;
    if (!sweep.init(W, H)) { fprintf(stderr, "sweep init failed\n"); return 1; }
    sweep.settings().rayRadius = radius;
    sweep.settings().ambient[0] = 0.10f;
    sweep.settings().ambient[1] = 0.10f;
    sweep.settings().ambient[2] = 0.12f;

    printf("[阶段] 只跑扫描通道（不合成）\n");
    sweep.uploadAll(c.image(), c.materialData());
    sweep.render();
    GLenum e = glGetError();
    if (e) printf("  GL error 0x%04X\n", e);

    dumpTex("scan", sweep.scanTexture(), W, H);
    dumpTex("result", sweep.resultTexture(), W, H);

    // 逐层结果（数组纹理）要一层一层读：glGetTexImage 不能整体读数组纹理的某一层，
    // 所以用 LayerComposite 把它当作"只有一层、且不透明"来合成为一张 2D 图。
    // 这同时也是在验证 LayerComposite 本身。
    {
        gfx::LayerComposite comp;
        if (comp.init()) {
            GLuint flat = gfx::createTexture(GL_RGBA8, W, H, GL_RGBA, GL_UNSIGNED_BYTE);
            comp.composite(sweep.litTexture(), sweep.opacityTexture(), 1, W, H, flat,
                           false);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            dumpTex("lit0", flat, W, H);
            glDeleteTextures(1, &flat);
            comp.shutdown();
        }
    }

    sweep.shutdown();
    ctx.destroy();
    return 0;
}
