// tools/dz25_test.cpp — 按规格验收 2.5D 光追
//
// 用例 1（你给的判据）：
//   层 z=0：纯白材质（受光面）
//   层 z=1：中间一条红色自发光
//   太阳/环境光 = 0（"天光为 0"）
//   期望：没有光的地方纯黑；光源下方及周围出现红色光晕
//
// 用例 2：把受光面做成不透明黑、只留红光 -> 直接看光晕形状
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

static void dumpStats(const char* tag, const std::vector<uint8_t>& px, int W, int H) {
    int black = 0, red = 0, other = 0;
    long sumr = 0;
    for (int i = 0; i < W * H; i++) {
        const int r = px[(size_t)i * 4], g = px[(size_t)i * 4 + 1], b = px[(size_t)i * 4 + 2];
        sumr += r;
        if (r == 0 && g == 0 && b == 0) black++;
        else if (r > g && r > b) red++;
        else other++;
    }
    printf("  %s: 纯黑 %d / %d   偏红 %d   其他 %d   平均R %.1f\n", tag, black, W * H, red,
           other, (double)sumr / (double)(W * H));
}

int main(int argc, char** argv) {
    int W = 96, H = 48, Z = 2, radius = 96;
    float skyScale = 0.0f;      // "天光为 0"
    std::string out = "out/dz25";
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
        else if (a == "--sky") skyScale = (float)std::atof(next().c_str());
        else if (a == "--out") out = next();
    }

    gfx::ContextDesc desc;
    desc.width = 64; desc.height = 64; desc.visible = false; desc.title = "dz25_test";
    gfx::GLContext ctx;
    if (!ctx.create(desc)) return 1;

    gfx::DzTrace tr;
    if (!tr.init(W, H, Z)) { fprintf(stderr, "init failed\n"); return 1; }

    // 体素空间
    VoxelGrid g;
    g.init(W, H, Z);

    // z=0：纯白**不透明**受光面
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            Voxel& v = g.at(x, y, 0);
            v.color = RGBA8{255, 255, 255, 255};   // C = 白, A = 1
        }
    // z=1：中间一条红色自发光（悬空在受光面之上）
    int emitCount = 0;
    for (int y = H / 3; y < H * 2 / 3; y++)
        for (int x = W / 3; x < W * 2 / 3; x++) {
            Voxel& v = g.at(x, y, 1);
            v.color = RGBA8{255, 0, 0, 255};        // 红
            v.mat.setEmissionFrom(255, 0, 0);       // 自发光红
            emitCount++;
        }
    printf("体素空间 %dx%dx%d，受光面=纯白不透明(z=0)，红光=%d 个体素(z=1)，半径=%d\n", W, H,
           Z, emitCount, radius);

    tr.uploadVolume(g);

    gfx::DzTraceParams prm;
    prm.radius = radius;
    prm.emitScale = 1.0f;
    // 上爬率：让射线在半径范围内扫过整个高度带（否则光晕铺不开）
    prm.zRate = getenv("PB_DZ_H") ? 0 : std::max(1, (int)std::ceil((double)Z / std::max(1.0, (double)radius / 4.0)));
    prm.sunCol[0] = prm.sunCol[1] = prm.sunCol[2] = skyScale * 1.0f;
    prm.wallCol[0] = prm.wallCol[1] = prm.wallCol[2] = skyScale * 0.8f;
    if (!tr.render(prm)) { fprintf(stderr, "render failed\n"); return 1; }

    std::vector<uint8_t> px((size_t)W * H * 4u, 0);
    glBindTexture(GL_TEXTURE_2D, tr.resultTexture());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    Image img(W, H);
    for (int i = 0; i < W * H; i++)
        img.data()[i] = RGBA8{px[(size_t)i * 4], px[(size_t)i * 4 + 1], px[(size_t)i * 4 + 2], 255};
    const std::string path = out + ".png";
    writePNG(path, img);

    dumpStats("天光=0", px, W, H);
    printf("  -> %s\n", path.c_str());

    tr.shutdown();
    ctx.destroy();
    return 0;
}
