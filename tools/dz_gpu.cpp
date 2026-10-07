// tools/dz_gpu.cpp — 验证 gfx/dz_sweep（GPU 版 d-z 平面 2D 光追）
//
// 最小场景：平地 + 一根高柱（用来看影子），出图 + 打印亮度。
// 用法：dz_gpu --out out/gpu --size 160x90 --dirs 16 --s 4
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/image.h"
#include "core/material.h"
#include "core/types.h"
#include "gfx/dz_sweep.h"
#include "gfx/gl_context.h"

using namespace pb;

// 造一个"上表面场"：地面在层 1，几根不同高度的柱子在更高层。
static gfx::SurfaceField makeScene(int W, int H, int L) {
    gfx::SurfaceField f;
    f.w = W;
    f.h = H;
    f.layers = L;
    const size_t n = (size_t)W * H;
    f.albedo.assign(n, RGBA8{0, 0, 0, 255});
    f.material.assign(n, Material{});
    f.height.assign(n, 0);

    auto put = [&](int x, int y, int z, RGBA8 c, Material m) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        const size_t i = (size_t)y * W + x;
        if (f.height[i] >= z) return;
        f.height[i] = (uint8_t)z;
        f.albedo[i] = c;
        f.material[i] = m;
    };

    // 地面
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) put(x, y, 1, RGBA8{110, 110, 118, 255}, Material{});

    // 柱子：越高的层号越大
    struct P { float xf, wf; int z; RGBA8 c; };
    const P ps[4] = {{0.18f, 0.06f, L - 1, RGBA8{235, 140, 110, 255}},
                     {0.40f, 0.05f, (L * 2) / 3, RGBA8{130, 200, 240, 255}},
                     {0.60f, 0.04f, L / 2, RGBA8{240, 220, 130, 255}},
                     {0.80f, 0.05f, (L * 5) / 6, RGBA8{180, 150, 240, 255}}};
    for (const P& p : ps) {
        const int x0 = (int)(p.xf * W), x1 = x0 + (int)(p.wf * W);
        const int y0 = H / 3;
        for (int y = y0; y < H; y++)
            for (int x = x0; x < x1; x++) put(x, y, p.z, p.c, Material{});
    }

    // 高处一盏自发光灯：照亮柱身与地面
    for (int x = (int)(0.25f * W); x < (int)(0.75f * W); x++) {
        Material em;
        em.setEmissionFrom(255, 200, 130);
        put(x, 2, L, RGBA8{255, 235, 200, 255}, em);
    }
    return f;
}

int main(int argc, char** argv) {
    std::string out = "out/dz_gpu";
    int W = 160, H = 90, L = 8;
    int dirs = 16;
    float S = 4.0f;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--out") out = next();
        else if (a == "--dirs") dirs = std::atoi(next().c_str());
        else if (a == "--s") S = (float)std::atof(next().c_str());
        else if (a == "--layers") L = std::atoi(next().c_str());
        else if (a == "--size") {
            const std::string s = next();
            const size_t xp = s.find('x');
            if (xp != std::string::npos) {
                W = std::atoi(s.substr(0, xp).c_str());
                H = std::atoi(s.substr(xp + 1).c_str());
            }
        }
    }

    gfx::ContextDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.visible = false;
    desc.title = "dz_gpu";
    gfx::GLContext ctx;
    if (!ctx.create(desc)) { fprintf(stderr, "no context\n"); return 1; }

    gfx::DzSweep dz;
    if (!dz.init(W, H, L)) { fprintf(stderr, "init failed\n"); return 1; }

    gfx::SurfaceField f = makeScene(W, H, L);
    int nonAir = 0;
    for (uint8_t z : f.height) if (z) nonAir++;
    printf("[dz_gpu] %dx%d layers=%d dirs=%d S=%g  非空气像素=%d\n", W, H, L, dirs, S, nonAir);

    dz.uploadField(f);

    gfx::DzSweepParams prm;
    prm.directions = dirs;
    prm.stepsPerLevel = S;
    prm.sky[0] = 0.40f; prm.sky[1] = 0.43f; prm.sky[2] = 0.50f;
    if (!dz.render(prm)) { fprintf(stderr, "render failed\n"); return 1; }

    // 回读
    std::vector<uint8_t> px((size_t)W * H * 4u, 0);
    glBindTexture(GL_TEXTURE_2D, dz.resultTexture());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    Image img(W, H);
    std::memcpy(img.data(), px.data(), px.size());
    double sum = 0;
    for (size_t i = 0; i < (size_t)W * H; i++)
        sum += ((double)px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3.0;
    printf("     平均亮度 %.2f\n", sum / ((double)W * H));

    const std::string path = out + "_s" + std::to_string((int)std::lround(S)) + ".png";
    if (!writePNG(path, img)) { fprintf(stderr, "写不出 %s\n", path.c_str()); return 1; }
    printf("     -> %s\n", path.c_str());

    dz.shutdown();
    ctx.destroy();
    return 0;
}
