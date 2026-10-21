// tools/solid_probe.cpp — 复现"大面积纯色 + 分块扫描"下的条纹/接缝
//
// 场景刻意做得极简：6 层，只有最底层是整幅红色，其余空。没有自发光、没有细节
// —— 这种画面最能把**块的边界**暴露出来（真实光照的低频变化会掩盖接缝）。
//
// 用法：
//   solid_probe --out a.png                 经典路径
//   solid_probe --out b.png --tiled         分块路径
//   solid_probe --diff --radius 32          带扩散（画板默认档位）
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
    bool tiled = false, diffOn = false;
    int radius = 32, layers = 6, w = 320, h = 180;
    int tr = 0, gy = 0;   // 0 = 用默认（16/16）
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (!std::strcmp(a, "--out")) { const char* v = next(); if (v) outPath = v; }
        else if (!std::strcmp(a, "--tiled")) tiled = true;
        else if (!std::strcmp(a, "--diff")) diffOn = true;
        else if (!std::strcmp(a, "--radius")) { const char* v = next(); if (v) radius = atoi(v); }
        else if (!std::strcmp(a, "--tr")) { const char* v = next(); if (v) tr = atoi(v); }
        else if (!std::strcmp(a, "--gy")) { const char* v = next(); if (v) gy = atoi(v); }
        else if (!std::strcmp(a, "--layers")) { const char* v = next(); if (v) layers = atoi(v); }
        else if (!std::strcmp(a, "--size")) {
            const char* v = next();
            if (v) sscanf(v, "%dx%d", &w, &h);
        }
    }

    pb::Document doc(w, h);
    for (int i = 0; i < layers; i++) doc.addLayer("");
    {
        pb::Layer& L = doc.layer(0);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                L.setPixelRaw(x, y, pb::RGBA8{150, 46, 40, 255}, pb::Material{});
    }

    pb::gfx::GLContext ctx;
    pb::gfx::ContextDesc cd;
    cd.width = w; cd.height = h; cd.visible = false; cd.title = "solid_probe";
    if (!ctx.create(cd)) { std::fprintf(stderr, "[probe] GL init failed\n"); return 2; }

    pb::gfx::LightSweep sweep;
    if (!sweep.init(w, h)) return 3;
    sweep.settings().rayRadius = radius;
    sweep.settings().diffusionOn = diffOn;
    if (tiled) {
        sweep.setAutotune(false);
        sweep.setTiledScan(true);
        if (tr > 0) { sweep.setTileRow(tr); sweep.setTileCol(tr); }
        if (gy > 0) sweep.setDirGroupY(gy);
    }
    else       { sweep.setAutotune(false); sweep.setTiledScan(false); }
    sweep.setLayerCount(layers);

    const pb::Rect all{0, 0, w, h};
    for (int i = 0; i < layers; i++) {
        sweep.uploadLayer(i, doc.layer(i).albedo, doc.layer(i).material.data(), all);
        sweep.uploadLayerOpacity(i, 1.0f);
        sweep.renderLayer(i);
    }
    sweep.composite();

    std::vector<pb::RGBA8> px((size_t)w * h);
    if (!pb::gfx::readbackTexture(sweep.resultTexture(), w, h, px.data())) return 4;
    pb::Image img(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) img.at(x, y) = px[(size_t)y * w + x];
    if (!pb::writePNG(outPath, img)) return 5;

    // 打印每一行的平均亮度：条纹会表现为相邻行数值跳变
    for (int y = 0; y < h; y++) {
        long sum = 0;
        for (int x = 0; x < w; x++) sum += px[(size_t)y * w + x].r;
        if (y % 4 == 0) std::printf("y=%3d R=%3ld\n", y, sum / w);
    }
    std::printf("[probe] %s -> %s（%d 层 / R=%d / 分块=%d / 扩散=%d）\n", outPath, outPath, layers,
                radius, (int)tiled, (int)diffOn);
    return 0;
}
