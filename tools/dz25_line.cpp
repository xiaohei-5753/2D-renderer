// tools/dz25_line.cpp — 复现用户的场景：z=3 上一条粉色发光竖线，对比抖动
//
// 场景：背景层 z=0 是暗色（受光面）；z=3 上竖着一条粉色自发光线。
// 输出：开/关方向抖动两张图 + 条纹度量（沿水平方向的二阶差分能量，
//       条纹越强这个值越大）。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/image.h"
#include "core/voxel.h"
#include "gfx/dz_trace.h"
#include "gfx/gl_context.h"

using namespace pb;

// 条纹度量：对每一行求水平方向的 |f(x+1) - 2f(x) + f(x-1)| 之和。
// 平滑渐变 -> 接近 0；带状条纹 -> 明显变大。
static double bandiness(const std::vector<uint8_t>& px, int W, int H) {
    double acc = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 1; x + 1 < W; x++) {
            const int a = px[((size_t)y * W + x - 1) * 4];
            const int b = px[((size_t)y * W + x) * 4];
            const int c = px[((size_t)y * W + x + 1) * 4];
            acc += std::fabs((double)(a - 2 * b + c));
        }
    }
    return acc / (double)(W * H);
}

int main(int argc, char** argv) {
    int W = 320, H = 180, Z = 4, radius = 24, jitter = 3;
    float sun = 0.30f;
    std::string out = "out/dz25line";
    int seed = 0;
    float emit = 1.0f;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--size") {
            const std::string s = next();
            const size_t xp = s.find('x');
            if (xp != std::string::npos) {
                W = std::atoi(s.substr(0, xp).c_str());
                H = std::atoi(s.substr(xp + 1).c_str());
            }
        } else if (a == "--z") Z = std::atoi(next().c_str());
        else if (a == "--radius") radius = std::atoi(next().c_str());
        else if (a == "--jitter") jitter = std::atoi(next().c_str());
        else if (a == "--sun") sun = (float)std::atof(next().c_str());
        else if (a == "--seed") seed = std::atoi(next().c_str());
        else if (a == "--emit") emit = (float)std::atof(next().c_str());
        else if (a == "--out") out = next();
    }

    gfx::ContextDesc desc;
    desc.width = 64; desc.height = 64; desc.visible = false; desc.title = "dz25_line";
    gfx::GLContext ctx;
    if (!ctx.create(desc)) return 1;

    gfx::DzTrace tr;
    if (!tr.init(W, H, Z)) { fprintf(stderr, "init failed\n"); return 1; }

    VoxelGrid g;
    g.init(W, H, Z);
    // z=0：暗色受光面（模拟用户的暗背景）
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            g.at(x, y, 0).color = RGBA8{128, 128, 132, 255};
            // 让柱高有起伏（模拟画布上 z=1/z=2 有内容），这才是条纹的来源
            if (getenv("PB_HVAR") && ((x / 5 + y / 4) % 3 == 0))
                g.at(x, y, 1).color = RGBA8{110, 110, 114, 255};
        }
    // z=3：竖着一条粉色发光线（用户说粉色在 z=3 上）
    // 可选：铺一层水平条纹图案（PB_PATTERN=1 时）用来区分"内容"与"伪影"
    if (getenv("PB_PATTERN")) {
    // 再铺一层"水平条纹图案"在 z=1（模拟用户图里的横条）
    // 这些横条本身是画的内容，用不同亮度的灰
    for (int y = 0; y < H; y++) {
        if ((y / 3) % 2 == 0) continue;                 // 每 3 行一条
        const uint8_t v = (uint8_t)(40 + (y * 37) % 150);   // 伪随机亮度
        for (int x = 0; x < W; x++) {
            if (((x / 7) + (y / 5)) % 3 == 0) continue;      // 打散，别是整条直线
            g.at(x, y, 1).color = RGBA8{v, (uint8_t)(v * 2 / 3), (uint8_t)(v * 2 / 3), 255};
        }
    }

    }
    const int lx = W / 2;
    for (int y = (int)(H * 0.20f); y < (int)(H * 0.85f); y++) {
        Voxel& v = g.at(lx, y, 3);
        v.color = RGBA8{255, 90, 160, 255};
        v.mat.setEmissionFrom(255, 90, 160);
    }
    tr.uploadVolume(g);

    gfx::DzTraceParams prm;
    prm.radius = radius;
    prm.jitter = jitter;
    prm.sampleSeed = seed;
    prm.emitScale = emit;
    prm.sunCol[0] = prm.sunCol[1] = prm.sunCol[2] = sun * 0.5f;
    prm.wallCol[0] = prm.wallCol[1] = prm.wallCol[2] = 0.0f;
    if (!tr.render(prm)) { fprintf(stderr, "render failed\n"); return 1; }

    std::vector<uint8_t> px((size_t)W * H * 4u, 0);
    glBindTexture(GL_TEXTURE_2D, tr.resultTexture());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    Image img(W, H);
    for (int i = 0; i < W * H; i++)
        img.data()[i] = RGBA8{px[(size_t)i * 4], px[(size_t)i * 4 + 1], px[(size_t)i * 4 + 2], 255};
    const std::string path = out + "_jit" + std::to_string(jitter) + ".png";
    writePNG(path, img);

    printf("  jitter=%-3d 条纹度量 %.4f   -> %s\n", jitter, bandiness(px, W, H), path.c_str());
    tr.shutdown();
    ctx.destroy();
    return 0;
}
