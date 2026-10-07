// tools/dz_bench.cpp — 量一下 gather（精确解）在复杂场景上的真实代价
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/dz_plane.h"

using namespace pb;

int main(int argc, char** argv) {
    int W = 320, Zc = 12, dirs = 16;
    float S = 6.0f;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--w" && i + 1 < argc) W = std::atoi(argv[++i]);
        else if (a == "--z" && i + 1 < argc) Zc = std::atoi(argv[++i]);
        else if (a == "--dirs" && i + 1 < argc) dirs = std::atoi(argv[++i]);
        else if (a == "--s" && i + 1 < argc) S = (float)std::atof(argv[++i]);
    }

    // 造一个"复杂"场景：地面 + 一堆高低不一的柱子
    DzPlane p;
    p.init(W, Zc);
    for (int d = 0; d < W; d++) p.at(1, d).albedo = RGBA8{140, 140, 140, 255};
    unsigned rng = 12345;
    auto rnd = [&]() { rng = rng * 1103515245u + 12345u; return (rng >> 16) & 0x7fff; };
    for (int k = 0; k < W / 8; k++) {
        const int d0 = (int)(rnd() % (unsigned)W);
        const int w = 2 + (int)(rnd() % 4u);
        const int top = 2 + (int)(rnd() % (unsigned)(Zc - 1));   // 2..Zc，别越过 maxHeight
        for (int d = d0; d < d0 + w && d < W; d++)
            for (int z = 1; z <= top; z++) p.at(z, d).albedo = RGBA8{200, 120, 90, 255};
    }

    DzParams prm;
    prm.dirSign = -1;
    prm.sky[0] = 0.85f; prm.sky[1] = 0.9f; prm.sky[2] = 1.0f;

    const auto t0 = std::chrono::steady_clock::now();
    double acc = 0.0;
    // 只按方向数缩放 S：S 越大越斜、射线越长，但列数少一些也是同一量级
    for (int i = 0; i < dirs; i++) {
        prm.stepsPerLevel = S;
        std::vector<float> line;
        dzTraceLine(p, prm, line);
        for (float v : line) acc += v;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    printf("gather: W=%d Zc=%d dirs=%d S=%.1f -> %.0f ms  (acc=%.0f)\n", W, Zc, dirs, S, ms,
           acc);
    return 0;
}
