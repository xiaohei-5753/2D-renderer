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
    float gi = 1.0f;              // VCT GI 强度
    int maxMip = 3;               // VCT 最大 mip 层级
    int cones = 9;
    int plate = 0;                // 0=无板 1=带洞的板 2=整块无洞的板
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
        else if (a == "--gi") gi = (float)std::atof(next().c_str());
        else if (a == "--maxmip") maxMip = std::atoi(next().c_str());
        else if (a == "--cones") cones = std::atoi(next().c_str());
        else if (a == "--plate") plate = std::atoi(next().c_str());
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

    // 分图层遮挡的判定场景。
    //
    // 关键：**被遮挡的表面和遮挡物不能在同一列**，否则俯视下遮挡物自己就成了柱顶
    // （我第一版就是这样，量到的"漏光"其实是板子顶面被正常照亮）。
    // 所以遮挡板只铺 x < W/2，去看**板子范围之外**的地面（x >= W/2）：
    // 从那里射向光源的斜线会穿过板子 -> 应当明显变暗。
    // 板中央留一个洞（在光源正下方），洞下面的地面应当仍然很亮。
    //
    //   plate=1  放遮挡板
    //   plate=0  对照，不放板
    if (plate) {
        const int zlo = 5, zhi = 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W / 4; x++) {
                if (x >= W / 8 && x < W / 8 + 8 && y >= H / 3 && y < H / 3 + 8)
                    continue;                       // 洞
                for (int z = zlo; z < zhi; z++)
                    v.color[v.idx(x, y, z)] = RGBA8{120, 120, 130, 255};
            }
    }

    // 发光柱：放在板子**之上**
    const int ez0 = plate ? 8 : 0;
    for (int z = ez0; z < ez0 + 6; z++)
        for (int y = H / 3; y < H / 3 + 6; y++)
            for (int x = W / 6; x < W / 6 + 6; x++) {
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
    rs.gi = render::RenderSettings::Gi::ConeTrace;
    rs.cones = cones;
    rs.giMaxMip = maxMip;
    rs.coneMaxDist = 96.0f;
    rs.giIntensity = gi;
    r->setSettings(rs);

    std::vector<RGBA8> px;
    const int frames = 4;
    for (int f = 0; f < frames; f++) r->render(px);
    printf("  锥追踪（确定性）：跑了 %d 帧\n", r->accumFrames());

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

    // 数值检查 2：**遮挡板之外**的地面（x >= W/2）。板挡住斜射光 -> 应当变暗。
    printf("  板外地面 y=%d  x=%d..%d 的 G：\n   ", ly, W / 4, W / 4 + 23);
    for (int x = W / 4; x < W / 4 + 24; x++) printf("%4d", px[(size_t)ly * W + x].g);
    printf("\n");

    // 数值检查 3：高台右侧应当出现阴影（阳光从 az=135 即左上来）
    const int sy = H / 4 + 2;
    printf("  体素 %dx%dx%d  el=%.0f az=%.0f sun=%.2f sky=%.2f  最后一帧 %.2f ms\n", W, H, Z,
           el, az, sun, sky, r->lastMs());
    printf("  第 %d 行 R（高台 x=%d..%d）：\n   ", sy, W / 2, W * 3 / 4 - 1);
    for (int x = W / 2 - 6; x < W * 3 / 4 + 14 && x < W; x++)
        printf("%4d", px[(size_t)sy * W + x].r);
    printf("\n   -> %s\n", path.c_str());
    return 0;
}
