// tools/dz_scene.cpp — 用 d-z 平面光追渲染"复杂场景"
//
// 流程：
//   1. 程序化生成一个**多图层**文档（图层越高 = 世界里的东西越高）
//   2. 压平成"上表面场"：每个像素取覆盖它的最高层 -> (高度 z, albedo, 材质)
//   3. 对每个收集方向 ω：把射线穿过的像素收集成 d-z 横截面（宽度 d、高度 z），
//      在上面跑 2D 光追（core/dz_plane），累加回该方向的结果图
//   4. 所有方向求平均 -> 最终画面
//
// 用法：
//   dz_scene --scene neon_city --size 240x135 --dirs 24 --slopes 0,6 --out out/city
#include <chrono>
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

// ---------------------------------------------------------------- 随机数
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int range(int a, int b) { return a + (int)(next() % (uint32_t)(b - a + 1)); }
    float f01() { return (float)(next() % 10000u) / 10000.0f; }
};

// ---------------------------------------------------------------- 场景构建
// 每个场景返回一个"上表面场"：z / albedo / 自发光。z 从 1 开始（1 = 最底层）。
struct SurfaceField {
    int w = 0, h = 0;
    int zMax = 0;
    std::vector<uint8_t> z;    // 高度 1..zMax
    std::vector<RGBA8> alb;
    std::vector<uint8_t> em;   // 自发光用"亮度系数"，具体颜色由下面 emRGB 决定
    std::vector<RGBA8> emRGB;
};

void put(SurfaceField& f, int x, int y, int zz, RGBA8 albedo, RGBA8 em = RGBA8{0, 0, 0, 0}) {
    if (x < 0 || y < 0 || x >= f.w || y >= f.h) return;
    const size_t i = (size_t)y * (size_t)f.w + (size_t)x;
    // 只保留"更高的那个"（相机看到的是最高的上表面）
    if (f.z[i] >= zz) return;
    f.z[i] = (uint8_t)zz;
    f.alb[i] = albedo;
    f.emRGB[i] = em;
    f.em[i] = (uint8_t)((em.r > em.g ? em.r : em.g) > em.b ? (em.r > em.g ? em.r : em.g) : em.b);
}

// --- 场景 1：霓虹城市（远处天际线 + 近处发光招牌）---
void buildNeonCity(SurfaceField& f, Rng& rng) {
    const int W = f.w, H = f.h;
    const RGBA8 sky{8, 10, 22, 255};
    // ① 天空：最底层铺满，给画面一个底
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) put(f, x, y, 1, sky);

    // ② 远山/云雾：稍高一点的一层，颜色偏紫
    for (int x = 0; x < W; x++) {
        const int top = (int)(H * 0.28f + 8.0f * std::sin(x * 0.05f));
        for (int y = H - 1 - top; y < H; y++)
            put(f, x, y, 2, RGBA8{26, 20, 44, 255});
    }

    // ③ 一排高楼（不同高度、不同色调），窗户随机点亮
    int x = 2;
    while (x < W - 4) {
        const int bw = rng.range(6, 16);
        const int bh = rng.range(H / 4, (int)(H * 0.62f));
        const int zz = rng.range(3, 5);
        const RGBA8 body{(uint8_t)rng.range(24, 42), (uint8_t)rng.range(24, 44),
                         (uint8_t)rng.range(38, 60), 255};
        for (int bx = x; bx < x + bw && bx < W; bx++)
            for (int y = H - 1 - bh; y < H; y++) put(f, bx, y, zz, body);
        // 窗户：一层更高的"灯"，自发光
        for (int bx = x + 1; bx < x + bw - 1 && bx < W; bx += 2) {
            for (int y = H - 1 - bh + 2; y < H - 2; y += 3) {
                if (rng.f01() < 0.45f) {
                    const RGBA8 glow{(uint8_t)rng.range(200, 255), (uint8_t)rng.range(150, 220),
                                     (uint8_t)rng.range(60, 140), 255};
                    put(f, bx, y, zz + 1, glow, glow);
                }
            }
        }
        x += bw + rng.range(1, 4);
    }

    // ④ 前景霓虹招牌：一条高处的亮管，用来投影到街道上
    for (int x = (int)(W * 0.10f); x < (int)(W * 0.42f); x++) {
        const int y = (int)(H * 0.30f);
        put(f, x, y, f.zMax, RGBA8{255, 90, 160, 255}, RGBA8{255, 60, 150, 255});
    }
    // ⑤ 街道：最底下一层压暗
    for (int x = 0; x < W; x++)
        for (int y = H - 5; y < H; y++) put(f, x, y, 1, RGBA8{14, 14, 20, 255});
}

// --- 场景 2：峡谷金字塔（层叠台地，用来展示有限阴影的长短）---
void buildCanyon(SurfaceField& f, Rng& rng) {
    const int W = f.w, H = f.h;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) put(f, x, y, 1, RGBA8{30, 26, 34, 255});

    // 三层台地，越靠上越高、颜色越亮
    struct Step { float yFrac; int zz; RGBA8 c; };
    const Step steps[3] = {{0.72f, 2, RGBA8{96, 62, 58, 255}},
                           {0.52f, 3, RGBA8{150, 96, 74, 255}},
                           {0.34f, 4, RGBA8{206, 148, 96, 255}}};
    for (const Step& s : steps) {
        for (int x = 0; x < W; x++) {
            const int y0 = (int)(H * s.yFrac);
            const int wob = (int)(3.0f * std::sin(x * 0.09f + s.yFrac * 10.0f));
            for (int y = y0 + wob; y < H; y++) put(f, x, y, s.zz, s.c);
        }
    }
    // 峡谷里的一条河：高处一条冷色带，会照亮下面的台地
    for (int x = (int)(W * 0.30f); x < (int)(W * 0.70f); x++) {
        const int y = (int)(H * 0.18f);
        put(f, x, y, f.zMax, RGBA8{120, 180, 255, 255}, RGBA8{70, 130, 255, 255});
    }
    // 点缀：几块随机巨石
    for (int k = 0; k < 14; k++) {
        const int bx = rng.range(0, W - 1), by = rng.range((int)(H * 0.45f), H - 1);
        const int s = rng.range(3, 9);
        for (int y = by; y < by + s && y < H; y++)
            for (int x = bx; x < bx + s && x < W; x++)
                put(f, x, y, rng.range(2, 4), RGBA8{(uint8_t)rng.range(60, 120),
                                                    (uint8_t)rng.range(50, 100),
                                                    (uint8_t)rng.range(45, 90), 255});
    }
}

// --- 场景 3：柱厅（几根很高的柱子，专门看影子被拉长）---
void buildPillars(SurfaceField& f, Rng& rng) {
    const int W = f.w, H = f.h;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) put(f, x, y, 1, RGBA8{58, 58, 66, 255});

    // 交错排布的高柱：越高影子越长
    for (int k = 0; k < 7; k++) {
        const int cx = (int)((k + 0.5f) * (float)W / 7.0f);
        const int r = rng.range(4, 9);
        const int top = rng.range((int)(H * 0.15f), (int)(H * 0.45f));
        const int zz = (int)(f.zMax * (1.0f - (float)top / (float)H) + 1.5f);
        const RGBA8 c{(uint8_t)rng.range(150, 235), (uint8_t)rng.range(140, 220),
                      (uint8_t)rng.range(130, 210), 255};
        for (int y = top; y < H; y++)
            for (int x = cx - r; x <= cx + r; x++) put(f, x, y, zz, c);
    }
    // 柱顶一盏暖灯：照亮柱身与地面
    for (int x = (int)(W * 0.20f); x < (int)(W * 0.80f); x++)
        put(f, x, (int)(H * 0.10f), f.zMax, RGBA8{255, 220, 150, 255},
            RGBA8{255, 190, 110, 255});
}

// ---------------------------------------------------------------- d-z 横截面
// 沿方向 ω 收集：每个采样点 d 查一次上表面，填进 (d, z) 平面。
//
// ⚠️ 关键：**必须把整列填满**（z=1 一直填到表面高度），而不是只写最上面那一格。
// 我第一版只写了表面那一格，于是"下面的层"全是空气 —— 遮挡关系消失，
// 射线直接穿过去，画面几乎全黑，还出现中心放射状的假条纹。
// 上表面场给的是"这一列被填到多高"，要还原成可被射线挡住的高度场，
// 就得把 [1, z] 都标成不透明；只有最上面那一格带 albedo 与自发光
// （这也是"只有上表面受光"的落点）。
DzPlane collectRay(const SurfaceField& f, float ox, float oy, float ux, float uy, int len) {
    DzPlane p;
    p.init(len, f.zMax);
    const RGBA8 opaque{200, 200, 200, 255};   // 下层：只负责挡光
    for (int i = 0; i < len; i++) {
        const float fx = ox + ux * (float)i;
        const float fy = oy + uy * (float)i;
        const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
        if (ix < 0 || iy < 0 || ix >= f.w || iy >= f.h) continue;
        const size_t k = (size_t)iy * (size_t)f.w + (size_t)ix;
        const int zz = f.z[k];
        if (zz < 1) continue;
        // 整列填实：下层只挡光
        for (int z = 1; z < zz; z++) p.at(z, i).albedo = opaque;
        // 最上面那一格带颜色与自发光（= 受光的表面）
        DzCell& c = p.at(zz, i);
        c.albedo = f.alb[k];
        c.emR = f.emRGB[k].r;
        c.emG = f.emRGB[k].g;
        c.emB = f.emRGB[k].b;
    }
    return p;
}

}  // namespace

int main(int argc, char** argv) {
    std::string scene = "neon_city";
    std::string outPrefix = "out/scene";
    std::string slopesArg = "0,8";
    int W = 240, H = 135;
    int dirs = 24;
    uint32_t seed = 20240607u;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--scene") scene = next();
        else if (a == "--out") outPrefix = next();
        else if (a == "--slopes") slopesArg = next();
        else if (a == "--dirs") dirs = std::atoi(next().c_str());
        else if (a == "--seed") seed = (uint32_t)std::atoi(next().c_str());
        else if (a == "--size") {
            const std::string s = next();
            const size_t xp = s.find('x');
            if (xp != std::string::npos) {
                W = std::atoi(s.substr(0, xp).c_str());
                H = std::atoi(s.substr(xp + 1).c_str());
            }
        }
    }
    if (W < 8 || H < 8) { fprintf(stderr, "尺寸太小\n"); return 1; }

    std::vector<float> slopes;
    for (size_t pos = 0; pos < slopesArg.size();) {
        const size_t comma = slopesArg.find(',', pos);
        slopes.push_back((float)std::atof(slopesArg.substr(pos, comma - pos).c_str()));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    // 1) 生成场景
    Rng rng(seed);
    SurfaceField f;
    f.w = W;
    f.h = H;
    f.zMax = scene == "canyon" ? 6 : (scene == "pillars" ? 7 : 6);
    f.z.assign((size_t)W * H, 0);
    f.alb.assign((size_t)W * H, RGBA8{0, 0, 0, 255});
    f.em.assign((size_t)W * H, 0);
    f.emRGB.assign((size_t)W * H, RGBA8{0, 0, 0, 0});

    if (scene == "canyon") buildCanyon(f, rng);
    else if (scene == "pillars") buildPillars(f, rng);
    else buildNeonCity(f, rng);

    printf("[dz_scene] 场景=%s %dx%d zMax=%d dirs=%d\n", scene.c_str(), W, H, f.zMax, dirs);
    {
        int hist[16] = {0};
        int emissive = 0;
        for (size_t i = 0; i < f.z.size(); i++) {
            if (f.z[i] < 16) hist[f.z[i]]++;
            if (f.emRGB[i].r || f.emRGB[i].g || f.emRGB[i].b) emissive++;
        }
        printf("    z 直方图:");
        for (int k = 0; k < 8; k++) printf(" %d:%d", k, hist[k]);
        printf("   自发光像素=%d\n", emissive);
    }

    for (float S : slopes) {
        const auto t0 = std::chrono::steady_clock::now();

        std::vector<float> light((size_t)W * H * 3u, 0.0f);   // 累加光
        std::vector<float> wsum((size_t)W * H, 0.0f);         // 权重之和（按法线）
        // 2) 沿多个方向收集 + 3) 在 d-z 平面上做 2D 光追
        //
        // 每个方向 ω 就是一个**入射方向**：光从 ω 那一侧射来。
        // 表面点判断可见性时要朝光源看，也就是沿 -ω 走。
        //
        // ⚠️ 我先前所有方向都用同一个 dirSign、而且不做法线加权，
        // 结果等于"光永远从左侧来"，背光面全黑、整体几乎无光。
        // 现在：收集方向为 -ω，并按 (法线 · 朝光源方向) 加权累加。
        const int diag = (int)std::ceil(std::sqrt((double)W * W + (double)H * H));
        for (int k = 0; k < dirs; k++) {
            const float ang = (float)(2.0 * 3.14159265358979 * (double)k / (double)dirs);
            const float ux = std::cos(ang), uy = std::sin(ang);
            // 起点在画布的 +ω 一侧之外，朝光源(-ω)走；d 方向与之一致
            const float cx = (float)W * 0.5f, cy = (float)H * 0.5f;
            const float rStart = 0.5f * (float)diag;
            const float ox = cx + ux * rStart, oy = cy + uy * rStart;

            DzPlane p = collectRay(f, ox, oy, -ux, -uy, diag);
            DzParams prm;
            prm.stepsPerLevel = (S <= 0.0f) ? 0.0f : S;
            // 朝光源走的横向分量决定 dirSign（横轴是 d 的推进方向）
            prm.dirSign = (std::fabs(ux) < 1e-4f) ? -1 : (ux > 0.0f ? 1 : -1);
            prm.sky[0] = 0.55f;
            prm.sky[1] = 0.58f;
            prm.sky[2] = 0.68f;

            std::vector<float> line;
            dzTraceLine(p, prm, line);

            // 累加回画布，并按法线是否朝向光源加权
            for (int i = 0; i < diag; i++) {
                const float fx = ox - ux * (float)i;
                const float fy = oy - uy * (float)i;
                const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
                if (ix < 0 || iy < 0 || ix >= W || iy >= H) continue;
                const size_t kk = (size_t)iy * (size_t)W + (size_t)ix;

                // 由高度场估计法线（高度归一化到 0..1）
                const float hC = (float)f.z[kk] / (float)f.zMax;
                const float hR = (ix + 1 < W) ? (float)f.z[kk + 1] / (float)f.zMax : hC;
                const float hD = (iy + 1 < H) ? (float)f.z[kk + (size_t)W] / (float)f.zMax : hC;
                // 沿 x/y 的坡度 -> 法线（z 分量给一个"平地面朝上"的基准）
                float nx = -(hR - hC) * 6.0f, ny = -(hD - hC) * 6.0f, nz = 1.0f;
                const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                nx /= nl; ny /= nl; nz /= nl;
                // 朝向光源的方向 = -ω
                const float ndl = (-ux) * nx + (-uy) * ny;
                const float wgt = (ndl > 0.0f ? ndl : 0.0f) + 0.25f;   // 加一点环境项

                light[kk * 3u + 0] += line[(size_t)i * 3 + 0] * wgt;
                light[kk * 3u + 1] += line[(size_t)i * 3 + 1] * wgt;
                light[kk * 3u + 2] += line[(size_t)i * 3 + 2] * wgt;
                wsum[kk] += wgt;
            }
        }

        // 4) 求平均并着色
        Image img(W, H);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const size_t k = (size_t)y * (size_t)W + (size_t)x;
                if (f.z[k] < 1) { img.at(x, y) = RGBA8{0, 0, 0, 255}; continue; }
                float lt[3];
                // 按权重和归一化（权重里含环境项，所以不会除零）
                const float ws = wsum[k] > 1e-6f ? wsum[k] : 1.0f;
                for (int c = 0; c < 3; c++) lt[c] = light[k * 3u + (size_t)c] / ws;
                DzCell surf;
                surf.albedo = f.alb[k];
                surf.emR = f.emRGB[k].r;
                surf.emG = f.emRGB[k].g;
                surf.emB = f.emRGB[k].b;
                DzParams prm;
                prm.addEmission = true;
                img.at(x, y) = dzShade(surf, lt, prm);
            }
        }

        char name[512];
        std::snprintf(name, sizeof(name), "%s_%s_s%d.png", outPrefix.c_str(), scene.c_str(),
                      (int)std::lround(S));
        if (!writePNG(name, img)) { fprintf(stderr, "写不出 %s\n", name); return 1; }

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("  stepsPerLevel=%-4g -> %s   (%.0f ms)\n", S, name, ms);
    }
    return 0;
}
