// tools/vox_test.cpp — 渲染端独立测试：不启动 GUI，直接建体素空间出图
//
// 这就是拆分的目的：渲染端可以脱离画板单独开发、单独验证。
// 场景：底层受光面 + 一个高台 + 一根发光柱，看 +z 面光照与阴影。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/image.h"
#include "render/voxel_renderer.h"
#include "render/voxel_volume.h"

using namespace pb;

int main(int argc, char** argv) {
    int W = 160, H = 96, Z = 16;
    float el = 45.0f, az = 135.0f, sun = 1.0f, sky = 0.20f;
    std::string out = "out/vox_test";
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
        else if (a == "--el") el = (float)std::atof(next().c_str());
        else if (a == "--az") az = (float)std::atof(next().c_str());
        else if (a == "--sun") sun = (float)std::atof(next().c_str());
        else if (a == "--sky") sky = (float)std::atof(next().c_str());
        else if (a == "--out") out = next();
    }

    render::VoxelVolume v;
    v.init(W, H, Z);

    // 底层：整片中灰受光面（只有最底一层）
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) v.color[v.idx(x, y, 0)] = RGBA8{150, 150, 155, 255};

    // 一个高台：中间偏右，高度 10
    for (int z = 0; z < 10; z++)
        for (int y = H / 4; y < H / 2; y++)
            for (int x = W / 2; x < W * 3 / 4; x++)
                v.color[v.idx(x, y, z)] = RGBA8{190, 120, 70, 255};

    // 一根发光柱：左侧，高度 6，自发光青色
    for (int z = 0; z < 6; z++)
        for (int y = H / 3; y < H / 3 + 3; y++)
            for (int x = W / 6; x < W / 6 + 3; x++) {
                const size_t k = v.idx(x, y, z);
                v.color[k] = RGBA8{80, 200, 220, 255};
                v.emission[k] = RGBA8{80, 200, 220, 255};
            }

    auto r = render::createVoxelRenderer();
    if (!r->setVolume(v)) { fprintf(stderr, "setVolume failed\n"); return 1; }
    render::RenderSettings rs;
    const float azr = az * 3.14159265f / 180.0f, elr = el * 3.14159265f / 180.0f;
    rs.sunDir[0] = std::cos(azr) * std::cos(elr);
    rs.sunDir[1] = std::sin(azr) * std::cos(elr);
    rs.sunDir[2] = std::sin(elr);
    rs.sunIntensity = sun;
    rs.skyColor[0] = rs.skyColor[1] = rs.skyColor[2] = sky;
    rs.emissiveLights = true;
    rs.emissiveIntensity = 1.0f;
    rs.lightSamples = 8;
    rs.progressive = true;
    rs.maxAccumFrames = 64;
    r->setSettings(rs);

    // 渐进累积：跑若干帧，看噪点收敛
    std::vector<RGBA8> px;
    const int frames = 32;
    for (int f = 0; f < frames; f++) r->render(px);
    printf("  累积 %d 帧\n", r->accumFrames());

    Image img(W, H);
    for (int i = 0; i < W * H; i++) img.data()[i] = px[(size_t)i];
    const std::string path = out + ".png";
    writePNG(path, img);

    // 数值检查 1：穿过发光柱那一行的地面（应当被染成青色）
    const int ly = H / 3 + 1;
    printf("  发光柱所在行 y=%d（柱在 x=%d..%d）G 通道：\n   ", ly, W / 6, W / 6 + 2);
    for (int x = W / 6 - 10; x < W / 6 + 14; x++)
        printf("%4d", px[(size_t)ly * W + x].g);
    printf("\n  同位置 R 通道：\n   ");
    for (int x = W / 6 - 10; x < W / 6 + 14; x++)
        printf("%4d", px[(size_t)ly * W + x].r);
    printf("\n");

    // 数值检查 2：高台右侧应当出现阴影（阳光从 az=135 即左上来）
    const int sy = H / 4 + 2;
    printf("  体素 %dx%dx%d  el=%.0f az=%.0f sun=%.2f sky=%.2f  最后一帧 %.2f ms\n", W, H, Z,
           el, az, sun, sky, r->lastMs());
    printf("  第 %d 行 R（高台 x=%d..%d）：\n   ", sy, W / 2, W * 3 / 4 - 1);
    for (int x = W / 2 - 6; x < W * 3 / 4 + 14 && x < W; x++)
        printf("%4d", px[(size_t)sy * W + x].r);
    printf("\n   -> %s\n", path.c_str());
    return 0;
}
