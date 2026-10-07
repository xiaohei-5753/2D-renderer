// tools/dz25_cpu.cpp — 验证 CPU 版正向层间递推：无条带、无"紧贴光源处黑洞"
//
// 复现用户的场景：暗背景(z=0) + z=3 上一条粉色发光竖线。
// 关键检查：沿一条水平扫描线打印辐照度，看它是否**单调平滑地**离开光源衰减。
//   反向投射的伪影表现：紧贴光源处=0，隔几格才亮起来（非单调，出现空洞）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/dz25.h"
#include "core/dz25slice.h"
#include "core/image.h"
#include "core/voxel.h"

using namespace pb;

int main(int argc, char** argv) {
    int W = 320, H = 180, Z = 4, S = 1, radius = 64;
    float emit = 1.0f;
    int baseA = 110;
    float gain = 400.0f;
    float amb = 0.0f;
    std::string out = "out/dz25cpu";
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
        else if (a == "--emit") emit = (float)std::atof(next().c_str());
        else if (a == "--amb") amb = (float)std::atof(next().c_str());
        else if (a == "--gain") gain = (float)std::atof(next().c_str());
        else if (a == "--basea") baseA = std::atoi(next().c_str());
        else if (a == "--out") out = next();
    }

    VoxelGrid g;
    g.init(W, H, Z);
    // z=0：中灰受光面
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            g.at(x, y, 0).color = RGBA8{128, 128, 132, (uint8_t)baseA};
    // z=Z-1：粉色发光竖线
    const int lx = W / 2;
    for (int y = (int)(H * 0.20f); y < (int)(H * 0.85f); y++) {
        Voxel& v = g.at(lx, y, Z - 1);
        v.color = RGBA8{255, 90, 160, 255};
        v.mat.setEmissionFrom(255, 90, 160);
    }

    std::vector<RGBA8> px;
    if (getenv("PB_SLICE")) {
        // 新方案：截面 + 「顶边→底边」斜线扫描
        Dz25SliceParams sp;
        sp.ambientOn = (amb > 0.0f);
        sp.ambient[0] = sp.ambient[1] = sp.ambient[2] = amb;
        sp.emitScale = emit;
        sp.directions = 4;
        trace25DSlice(g, sp, px);
    } else {
        // 旧方案：解析 gather（作为对照）
        Dz25Params prm;
        prm.radius = radius;
        prm.ambientOn = (amb > 0.0f);
        prm.ambient[0] = prm.ambient[1] = prm.ambient[2] = amb;
        prm.emitScale = emit;
        prm.gain = gain;
        trace25D(g, prm, px);
    }

    Image img(W, H);
    for (int i = 0; i < W * H; i++) img.data()[i] = px[(size_t)i];
    const std::string path = out + ".png";
    writePNG(path, img);

    // 沿竖线中间那一行打印 R 通道，检查单调性
    const int my = H / 2;
    printf("  %dx%dx%d  R=%d  emit=%.1f  amb=%.2f\n", W, H, Z, radius, emit, amb);
    printf("  第 %d 行 R（光源在 x=%d）：\n   ", my, lx);
    const int x0 = std::max(0, lx - 14), x1 = std::min(W, lx + 15);
    for (int x = x0; x < x1; x++) printf("%4d", px[(size_t)my * W + x].r);
    printf("\n     x=");
    for (int x = x0; x < x1; x++) printf("%4d", x);
    printf("\n");

    // 单调性检查：从光源向两侧走，R 应该**不增**（光只会越走越暗）
    int violations = 0, holes = 0;
    for (int side = -1; side <= 1; side += 2) {
        int prev = 9999;
        for (int d = 2; d < std::min(lx, W - 1 - lx); d++) {
            const int x = lx + side * d;
            const int r = px[(size_t)my * W + x].r;
            if (r > prev + 2) {          // 明显变亮 = 空洞后重新亮起来
                holes++;
                if (holes <= 6)
                    printf("    空洞：x=%d  d=%d  R=%d > 前一个 %d\n", x, d, r, prev);
            }
            if (r > prev + 2) violations++;
            prev = r;
        }
    }
    printf("  单调性：非单调(空洞) %d 处    -> %s\n", holes, path.c_str());

    return 0;
}
