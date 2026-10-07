// tools/dz_glow.cpp — 判据测试：天光=0 时，黑底应当纯黑；上层红光应当洒出红色光晕
//
// 场景：
//   层 1：整张纯白不透明（受光面）
//   层 2：中间一条**红色自发光**（E=(255,0,0)）
//   天光 sky = 0
// 期望：
//   没有红光洒到的地方 -> I≈0 -> 按 mix(I+E, C, A) 得 C=白... 但白色会被 A=1 完全显示
//   所以这里把受光面也做成**不透明黑**，这样 I=0 就是纯黑、I>0 才有红晕。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/image.h"
#include "core/material.h"
#include "core/types.h"
#include "gfx/dz_sweep.h"
#include "gfx/gl_context.h"

using namespace pb;

int main(int argc, char** argv) {
    int W = 96, H = 48, L = 3;
    int dirs = 24, m = 128;
    float sky = 0.0f;
    std::string out = "out/dz_glow";
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--dirs") dirs = std::atoi(next().c_str());
        else if (a == "--m") m = std::atoi(next().c_str());
        else if (a == "--layers") L = std::atoi(next().c_str());
        else if (a == "--sky") sky = (float)std::atof(next().c_str());
        else if (a == "--out") out = next();
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
    desc.width = 64; desc.height = 64; desc.visible = false; desc.title = "dz_glow";
    gfx::GLContext ctx;
    if (!ctx.create(desc)) return 1;

    gfx::DzSweep dz;
    if (!dz.init(W, H, L)) return 1;

    gfx::SurfaceField f;
    f.w = W; f.h = H; f.layers = L;
    const size_t n = (size_t)W * H;
    // 层 1：不透明黑（受光面）—— I=0 时应当纯黑
    f.albedo.assign(n, RGBA8{0, 0, 0, 255});
    f.material.assign(n, Material{});
    f.height.assign(n, 1);

    // 中间一条红色自发光，放在最高层（悬空）
    int lit = 0;
    // 对照开关：PB_FLAT=1 时把光源压在最底层（层差 0）。
    // 两者结果**完全相同** -> 证明 z（层差）没有参与光传导。
    const bool flat = getenv("PB_FLAT") != nullptr;
    for (int y = H / 3; y < H * 2 / 3; y++) {
        for (int x = (int)(0.35f * W); x < (int)(0.65f * W); x++) {
            const size_t i = (size_t)y * W + (size_t)x;
            f.height[i] = flat ? 1 : L;
            f.albedo[i] = RGBA8{255, 0, 0, 255};
            f.albedo[i] = RGBA8{255, 0, 0, 255};   // 颜色红
            f.material[i].setEmissionFrom(255, 0, 0);
            lit++;
        }
    }
    dz.uploadField(f);

    gfx::DzSweepParams prm;
    prm.directions = dirs;
    prm.sky[0] = prm.sky[1] = prm.sky[2] = sky;   // 天光 = 0
    dz.setRayRadius(m);
    if (!dz.render(prm)) { fprintf(stderr, "render failed\n"); return 1; }

    std::vector<uint8_t> px((size_t)W * H * 4u, 0);
    glBindTexture(GL_TEXTURE_2D, dz.resultTexture());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    Image img(W, H);
    for (size_t i = 0; i < n; i++) img.data()[i] = RGBA8{px[i * 4], px[i * 4 + 1], px[i * 4 + 2], 255};
    const std::string path = out + ".png";
    writePNG(path, img);

    // 统计：黑底上应当只有被红光洒到的地方非零，且是红的
    int redish = 0, other = 0, black = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const size_t i = ((size_t)y * W + x) * 4u;
            const int r = px[i], g = px[i + 1], b = px[i + 2];
            if (r == 0 && g == 0 && b == 0) black++;
            else if (r > g && r > b) redish++;
            else other++;
        }
    }
    printf("[dz_glow] %dx%d 层=%d 方向=%d m=%d 天光=%.2f  红光像素=%d\n", W, H, L, dirs, m, sky,
           lit);
    printf("   纯黑 %d / %d   偏红 %d   其他 %d\n", black, W * H, redish, other);
    printf("   -> %s\n", path.c_str());

    dz.shutdown();
    ctx.destroy();
    return 0;
}
