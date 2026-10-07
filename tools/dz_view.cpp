// tools/dz_view.cpp — 把 d-z 平面 2D 光追的结果画成 PNG，用于肉眼验收
//
// 输出两张图：
//   1. 剖面图：横轴 d、纵轴 z。每一格用"光到达这一格时的颜色"着色，
//      于是能看到光斜着穿过场景、并在障碍物后面拉出影子。实心像素用暗灰表示。
//   2. 结果条：每个 d 处"最高像素"的最终颜色（= albedo × 收到的光）。
//      这是真正会被看见的那一行 —— 有限阴影就是它上面的明暗边界被拉开了。
//
// 用法：dz_view --out out/dz --slopes 0,2,4,8 --size 160x10
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/dz_plane.h"
#include "core/image.h"

using namespace pb;

namespace {

const RGBA8 kWhite{255, 255, 255, 255};
const RGBA8 kBlack{0, 0, 0, 255};

// 造一个有"高度变化"的场景：
//   * 地面：z=1 铺满
//   * 三根不同高度的柱子（颜色也不同），用来比较影子长度
//   * 一根自发光的霓虹管（z=4），用来验证"光从高处照下来"
void buildScene(DzPlane& p, int W, int H) {
    p.init(W, H);
    for (int d = 0; d < W; d++) p.at(1, d).albedo = RGBA8{150, 150, 150, 255};

    auto pillar = [&](int d0, int w, int topZ, RGBA8 c) {
        for (int d = d0; d < d0 + w && d < W; d++)
            for (int z = 1; z <= topZ; z++) p.at(z, d).albedo = c;
    };
    // 柱子的水平位置要相对**画布中线**，不是相对高度。
    // 我一开始写成 H/2（=6，一个高度值），于是第一根柱子被摆到 d = -22，
    // 越界写了一整片、堆被写坏 —— 直到给 DzPlane::at 加了越界守卫才现形。
    const int mid = W / 2;
    pillar(mid - 60, 6, H - 1, RGBA8{255, 120, 90, 255});        // 高柱子
    pillar(mid - 20, 5, H / 2 + 1, RGBA8{120, 200, 255, 255});   // 中等
    pillar(mid + 30, 4, 3, RGBA8{255, 220, 120, 255});           // 矮的

    // 高处一条自发光霓虹管（跨在最上层）。
    // ⚠️ 层号是 1..maxHeight，而 H = maxHeight + 1 是**行数**，不是层号。
    // 这里也踩过一次 p.at(H, d) 越界。跨度要够宽，否则只有很窄的落点能被照到。
    for (int d = mid - 70; d <= mid - 40; d++) {
        if (d < 0 || d >= W) continue;
        p.at(p.maxHeight, d).albedo = RGBA8{255, 255, 255, 255};
        p.at(p.maxHeight, d).emR = 255;
        p.at(p.maxHeight, d).emG = 60;
        p.at(p.maxHeight, d).emB = 120;
    }
}
// 画剖面图：每一格的颜色 = 从天空射到这一格时携带的光（直观展示光路）
Image renderCrossSection(const DzPlane& p, float stepsPerLevel, int scale) {
    const int W = p.width, H = p.maxHeight + 1;
    Image img(scale * W, scale * H);

    // 逐列独立地重放一遍射线，把每一步的光画出来。
    // 复用 dzSampleLight 会拿不到中间状态，所以这里显式走一遍（与它同构）。
    const bool vertical = stepsPerLevel <= 0.0f;
    for (int d = 0; d < W; d++) {
        const int h = p.surfaceHeight(d);
        if (h <= 0) continue;
        float lt[3] = {1.0f, 1.0f, 1.0f};
        const float z0 = (float)h, d0 = (float)d + 0.5f;
        const float n = std::sqrt(1.0f + 1.0f / (stepsPerLevel * stepsPerLevel));
        const float tEnd = (float)(p.maxHeight + 2) *
                           (vertical ? 1.0f : std::sqrt(1.0f + stepsPerLevel * stepsPerLevel));
        for (float t = 0.0f; t <= tEnd; t += 0.25f) {
            float dd = d0, zz = z0 + t;
            if (!vertical) {
                dd = d0 - t / n;
                zz = z0 + (t / n) / stepsPerLevel;
            }
            if (zz > (float)(p.maxHeight + 1)) break;
            if (dd < 0.0f || dd >= (float)W) break;
            const int di = (int)std::floor(dd);
            const int zi = (int)std::floor(zz);
            if (di < 0 || di >= W || zi < 1 || zi > p.maxHeight) continue;
            const int sh = p.surfaceHeight(di);
            if (sh >= 1 && zz + 1e-3f < (float)sh) {
                const DzCell& c = p.at(sh, di);
                const float cov = (float)c.albedo.a / 255.0f;
                lt[0] = lt[0] * (1 - cov) * ((float)c.albedo.r / 255.0f) + (float)c.emR / 255.0f;
                lt[1] = lt[1] * (1 - cov) * ((float)c.albedo.g / 255.0f) + (float)c.emG / 255.0f;
                lt[2] = lt[2] * (1 - cov) * ((float)c.albedo.b / 255.0f) + (float)c.emB / 255.0f;
            }
            const int zb = p.maxHeight - zi;   // 画的时候 z 向上
            const int px = di * scale, py = zb * scale;
            auto put = [&](int x, int y, RGBA8 col) {
                if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) return;
                img.at(x, y) = col;
            };
            auto to8 = [](float v) {
                return (uint8_t)std::lround(v < 0 ? 0 : (v > 1 ? 255 : v * 255));
            };
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    put(px + sx, py + sy, RGBA8{to8(lt[0]), to8(lt[1]), to8(lt[2]), 255});
        }
    }

    // 把实心像素画成暗灰，好看出"障碍物在哪、影子从哪开始"
    for (int d = 0; d < W; d++) {
        const int sh = p.surfaceHeight(d);
        for (int z = 1; z <= sh; z++) {
            const int py = (p.maxHeight - z) * scale;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    const int x = d * scale + sx, y = py + sy;
                    if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) continue;
                    const RGBA8 old = img.at(x, y);
                    // 半透明地叠上灰色轮廓
                    img.at(x, y) = RGBA8{(uint8_t)((old.r + 90) / 2),
                                         (uint8_t)((old.g + 90) / 2),
                                         (uint8_t)((old.b + 90) / 2), 255};
                }
        }
    }
    return img;
}

// 结果条：每个 d 处最高像素的最终颜色
Image renderResultStrip(const DzPlane& p, const DzParams& prm, int scale, int rows) {
    const int W = p.width;
    Image img(scale * W, rows);
    for (int d = 0; d < W; d++) {
        const int h = p.surfaceHeight(d);
        float lt[3] = {0, 0, 0};
        if (h > 0) dzSampleLight(p, prm, d, lt);
        const RGBA8 c = dzShade(h > 0 ? p.at(h, d) : DzCell{}, lt, prm);
        for (int y = 0; y < rows; y++)
            for (int sx = 0; sx < scale; sx++) img.at(d * scale + sx, y) = c;
    }
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    std::string outPrefix = "out/dz";
    std::string slopesArg = "0,2,4,8";
    int W = 190, H = 12;
    int scale = 3;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--out") outPrefix = next();
        else if (a == "--slopes") slopesArg = next();
        else if (a == "--size") { /* 固定尺寸，保留参数位 */ }
        else if (a == "--scale") scale = std::atoi(next().c_str());
    }

    std::vector<float> slopes;
    for (size_t pos = 0; pos < slopesArg.size();) {
        const size_t comma = slopesArg.find(',', pos);
        slopes.push_back((float)std::atof(slopesArg.substr(pos, comma - pos).c_str()));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    for (float S : slopes) {
        DzPlane p;
        buildScene(p, W, H);

        DzParams prm;
        prm.stepsPerLevel = S;
        prm.dirSign = -1;
        prm.sky[0] = 0.85f; prm.sky[1] = 0.88f; prm.sky[2] = 1.0f;   // 偏冷的天光

        char name[512];
        const int si = (int)std::lround(S);
        std::snprintf(name, sizeof(name), "%s_s%d_cross.png", outPrefix.c_str(), si);
        Image cross = renderCrossSection(p, S, scale);
        if (!writePNG(name, cross)) {
            fprintf(stderr, "写不出 %s\n", name);
            return 1;
        }
        std::snprintf(name, sizeof(name), "%s_s%d_strip.png", outPrefix.c_str(), si);
        Image strip = renderResultStrip(p, prm, scale, 24);
        if (!writePNG(name, strip)) {
            fprintf(stderr, "写不出 %s\n", name);
            return 1;
        }
        printf("[dz_view] stepsPerLevel=%g -> %s_s%d_cross.png / _strip.png\n", S,
               outPrefix.c_str(), si);
    }
    return 0;
}
