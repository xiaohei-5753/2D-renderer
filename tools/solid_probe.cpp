// tools/solid_probe.cpp — 大面积纯色/渐变 + 多层的最小复现器
//
// 用途：画板里出现"横向条纹"这类低频、大面积伪影时，把渲染器单独拎出来看。
// 带纹在纯色上不出现、在渐变上出现，往往是"沿射线每隔 K 步做一次交换"这类
// 节奏类 bug（diffusion 的 diffusionStep）。
//
//   solid_probe --out a.png                            纯色 6 层
//   solid_probe --out b.png --grad                     纵向渐变
//   solid_probe --out c.png --grad --diff              开方向扩散
//   solid_probe --out d.png --grad --diff --prog --frames 60   渐进累积
//   solid_probe --out e.png --tiled                    分块路径
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/console.h"
#include "core/image.h"
#include "gfx/gl_texture.h"
#include "gfx/renderer.h"

int main(int argc, char** argv) {
    pb::setupConsoleUtf8();
    const char* outPath = "out/solid.png";
    bool tiled = false, diffOn = false, prog = false, grad = false;
    int radius = 32, layers = 6, w = 320, h = 180, frames = 1;
    float dw = 0.1f;
    int dk = 8, tr = 0, gy = 0;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (!std::strcmp(a, "--out")) { const char* v = next(); if (v) outPath = v; }
        else if (!std::strcmp(a, "--tiled")) tiled = true;
        else if (!std::strcmp(a, "--diff")) diffOn = true;
        else if (!std::strcmp(a, "--prog")) prog = true;
        else if (!std::strcmp(a, "--grad")) grad = true;
        else if (!std::strcmp(a, "--radius")) { const char* v = next(); if (v) radius = atoi(v); }
        else if (!std::strcmp(a, "--layers")) { const char* v = next(); if (v) layers = atoi(v); }
        else if (!std::strcmp(a, "--frames")) { const char* v = next(); if (v) frames = atoi(v); }
        else if (!std::strcmp(a, "--w")) { const char* v = next(); if (v) dw = (float)atof(v); }
        else if (!std::strcmp(a, "--k")) { const char* v = next(); if (v) dk = atoi(v); }
        else if (!std::strcmp(a, "--tr")) { const char* v = next(); if (v) tr = atoi(v); }
        else if (!std::strcmp(a, "--gy")) { const char* v = next(); if (v) gy = atoi(v); }
        else if (!std::strcmp(a, "--size")) {
            const char* v = next();
            if (v) sscanf(v, "%dx%d", &w, &h);
        }
    }

    pb::Document doc(w, h);
    for (int i = 0; i < layers; i++) doc.addLayer("");
    {
        pb::Layer& L = doc.layer(0);
        for (int y = 0; y < h; y++) {
            const int v = grad ? (150 - y * 60 / (h > 1 ? h - 1 : 1)) : 150;
            for (int x = 0; x < w; x++)
                L.setPixelRaw(x, y, pb::RGBA8{(uint8_t)v, 46, 40, 255}, pb::Material{});
        }
    }

    pb::gfx::GLContext ctx;
    pb::gfx::ContextDesc cd;
    cd.width = w; cd.height = h; cd.visible = false; cd.title = "solid_probe";
    if (!ctx.create(cd)) { std::fprintf(stderr, "[probe] GL init failed\n"); return 2; }

    pb::gfx::LightSweep sweep;
    if (!sweep.init(w, h)) return 3;
    sweep.settings().rayRadius = radius;
    sweep.settings().diffusionOn = diffOn;
    sweep.settings().diffusion = dw;
    sweep.settings().diffusionStep = dk;
    sweep.setProgressive(prog);
    sweep.setAutotune(false);
    sweep.setTiledScan(tiled);
    if (tiled) {
        if (tr > 0) { sweep.setTileRow(tr); sweep.setTileCol(tr); }
        if (gy > 0) sweep.setDirGroupY(gy);
    }
    sweep.setLayerCount(layers);

    const pb::Rect all{0, 0, w, h};
    for (int i = 0; i < layers; i++) {
        sweep.uploadLayer(i, doc.layer(i).albedo, doc.layer(i).material.data(), all);
        sweep.uploadLayerOpacity(i, 1.0f);
    }
    for (int f = 0; f < frames; f++) {
        if (frames > 1) sweep.beginFrame();
        for (int i = 0; i < layers; i++) sweep.renderLayer(i);
        if (frames > 1) sweep.endFrame();
        sweep.composite();
    }

    std::vector<pb::RGBA8> px((size_t)w * h);
    if (!pb::gfx::readbackTexture(sweep.resultTexture(), w, h, px.data())) return 4;
    pb::Image img(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) img.at(x, y) = px[(size_t)y * w + x];
    if (!pb::writePNG(outPath, img)) return 5;

    for (int y = 0; y < h; y++) {
        long sum = 0;
        for (int x = 0; x < w; x++) sum += px[(size_t)y * w + x].r;
        if (y % 4 == 0) std::printf("y=%3d R=%3ld\n", y, sum / w);
    }
    std::printf("[probe] %s (%d layers, R=%d, tiled=%d, diff=%d w=%.2f K=%d, prog=%d x%d, grad=%d)\n",
                outPath, layers, radius, (int)tiled, (int)diffOn, dw, dk, (int)prog, frames,
                (int)grad);
    return 0;
}