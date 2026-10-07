// tools/dz25_bench.cpp — 量 gather 的开销：发光体素数 / 半径 / 尺寸的影响
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/dz25.h"
#include "core/voxel.h"

using namespace pb;

int main(int argc, char** argv) {
    int W = 320, H = 180, Z = 2, radius = 96, emitters = 120;
    float gain = 4.0f, falloff = 1.0f;
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
        else if (a == "--emitters") emitters = std::atoi(next().c_str());
    }

    VoxelGrid g;
    g.init(W, H, Z);
    // 底层：不透明受光面
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) g.at(x, y, 0).color = RGBA8{128, 128, 132, 160};
    // 上层：`emitters` 个发光体素，铺成若干**细线**（真实像素画的样子：
    // 光源集中成线，而不是均匀撒满）。均匀撒满会让块级网格全满、失去加速意义。
    int placed = 0;
    const int lines = 12;
    for (int li = 0; li < lines && placed < emitters; li++) {
        const int y = (H * (li + 1)) / (lines + 1);
        for (int x = 8; x < W - 8 && placed < emitters; x++) {
            Voxel& v = g.at(x, y, Z - 1);
            v.color = RGBA8{255, 180, 90, 255};
            v.mat.setEmissionFrom(255, 180, 90);
            placed++;
        }
    }

    Dz25Params prm;
    prm.radius = radius;
    prm.gain = gain;
    prm.falloff = falloff;
    prm.emitScale = 1.0f;

    // 预热一次（线程池/缓存）
    std::vector<RGBA8> px;
    trace25D(g, prm, px);
    dz25ResetStats();

    const int reps = 5;
    double best = 1e9;
    for (int r = 0; r < reps; r++) {
        const auto t0 = std::chrono::steady_clock::now();
        trace25D(g, prm, px);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best) best = ms;
    }
    long long np = 0, ns = 0, nc = 0;
    dz25Stats(&np, &ns, &nc);
    printf("  %dx%dx%d  发光 %d  半径 %d  ->  %.1f ms   | 配对 %.1fM  细步 %.1fM  粗步 %.1fM\n", W,
           H, Z, placed, radius, best, np / 1e6, ns / 1e6, nc / 1e6);
    return 0;
}
