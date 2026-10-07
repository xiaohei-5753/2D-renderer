// tools/vox_test.cpp — 渲染端独立测试：不启动 GUI，直接建体素空间出图
//
// 这就是拆分的目的：渲染端可以脱离画板单独开发、单独验证。
// 场景：底层受光面 + 一个高台 + 一根发光柱，看 +z 面光照与阴影。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "core/image.h"
#include "render/voxel_renderer.h"
#include "gfx/gl_context.h"
#include "render/voxel_volume.h"

using namespace pb;

int main(int argc, char** argv) {
    int W = 160, H = 96, Z = 16;
    float el = 45.0f, az = 135.0f, sun = 1.0f, sky = 0.20f;
    float gi = 1.0f;              // VCT GI 强度
    float ap = 1.0f;              // 锥孔径缩放
    int alg = 3;                  // GI 算法
    bool compare = false;         // 与反向光追（参照物）对比
    bool gpu = false;             // 用 GPU 版
    int k0 = 4;                   // RC 最细一层的射线数
    float l0 = 2.0f;              // RC 最细一层的距离区间上限
    int ncs = 7;                  // RC 层数
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
        else if (a == "--ap") ap = (float)std::atof(next().c_str());
        else if (a == "--maxmip") maxMip = std::atoi(next().c_str());
        else if (a == "--cones") cones = std::atoi(next().c_str());
        else if (a == "--plate") plate = std::atoi(next().c_str());
        else if (a == "--alg") alg = std::atoi(next().c_str());   // 0关 1光源采样 2VCT 3RC
        else if (a == "--compare") compare = true;
        else if (a == "--gpu") gpu = true;
        else if (a == "--k0") k0 = std::atoi(next().c_str());
        else if (a == "--l0") l0 = (float)std::atof(next().c_str());
        else if (a == "--nc") ncs = std::atoi(next().c_str());
        else if (a == "--out") out = next();
    }

    // ring 场景：**单个发光体素** + 平坦地面。用来暴露锥的采样足迹：
    // 半球积分应当径向对称，沿等距圆环亮度应当基本一致（min≈max）。
    const bool ring = (getenv("PB_RING") != nullptr);

    render::VoxelVolume v;
    v.init(W, H, Z);

    // 底层：整片中灰受光面（只有最底一层）
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) v.color[v.idx(x, y, 0)] = RGBA8{150, 150, 155, 255};

    // 一个高台：中间偏右，高度 10（ring 模式下不要，它会盖住采样圆环）
    if (!ring) for (int z = 0; z < 10; z++)
        for (int y = H / 4; y < H / 2; y++)
            for (int x = W / 2; x < W * 3 / 4; x++)
                v.color[v.idx(x, y, z)] = RGBA8{190, 120, 70, 255};

    if (ring) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) v.color[v.idx(x, y, 0)] = RGBA8{150, 150, 155, 255};
        const int cx = W / 2, cy = H / 2;
        const size_t k = v.idx(cx, cy, 6);
        v.color[k] = RGBA8{255, 210, 140, 255};
        v.emission[k] = RGBA8{255, 210, 140, 255};
    }

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
    if (plate && !ring) {
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
    if (!ring) for (int z = ez0; z < ez0 + 6; z++)
        for (int y = H / 3; y < H / 3 + 6; y++)
            for (int x = W / 6; x < W / 6 + 6; x++) {
                const size_t k = v.idx(x, y, z);
                v.color[k] = RGBA8{80, 200, 220, 255};
                v.emission[k] = RGBA8{80, 200, 220, 255};
            }

    // 按需构造：CPU 路径下不能有无条件构造的 GLContext
    // （它没被 create，析构时会对未初始化状态 destroy -> 访问违例 0xC0000005）
    std::unique_ptr<gfx::GLContext> ctx;
    if (gpu) {
        gfx::ContextDesc cd;
        cd.width = 64; cd.height = 64; cd.visible = false; cd.title = "vox_gpu";
        ctx = std::make_unique<gfx::GLContext>();
        if (!ctx->create(cd)) { fprintf(stderr, "GL context failed\n"); return 1; }
    }
    auto r = gpu ? render::createVoxelGpuRenderer() : render::createVoxelRenderer();
    if (!r) { fprintf(stderr, "renderer create failed\\n"); return 1; }
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
    rs.coneAperture = ap;
    rs.cascadeRays = k0;
    rs.cascadeBase = l0;
    rs.cascades = ncs;
    rs.gi = (alg == 1)   ? render::RenderSettings::Gi::LightSampling
            : (alg == 2) ? render::RenderSettings::Gi::ConeTrace
            : (alg == 3) ? render::RenderSettings::Gi::RadianceCascades
                         : render::RenderSettings::Gi::None;
    r->setSettings(rs);

    std::vector<RGBA8> px;
    const int frames = 256;
    double firstMs = 0.0;
    for (int f = 0; f < frames; f++) { r->render(px); if (f == 0) firstMs = r->lastMs(); }
    printf("  单帧（第一帧）%.1f ms\n", firstMs);
    if (compare) {
        // 用**反向光追（光源采样）**当参照物：两种算法都应收敛到同一张图
        std::vector<RGBA8> ref;
        rs.gi = render::RenderSettings::Gi::LightSampling;
        rs.lightSamples = 16;
        r->setSettings(rs);
        for (int f = 0; f < 512; f++) r->render(ref);
        double sum = 0, mx = 0; int nd = 0; size_t mi = 0;
        for (size_t i = 0; i < ref.size(); i++) {
            const int a0 = ref[i].r, a1 = px[i].r;
            const double d = std::abs(a0 - a1);
            sum += d; if (d > mx) { mx = d; mi = i; } if (d > 2) nd++;
        }
        printf("  最大差在 (%zu,%zu)：本算法 R=%d  参照物 R=%d\n", mi % (size_t)W, mi / (size_t)W,
               px[mi].r, ref[mi].r);
        printf("  vs 反向光追：平均差 %.2f  最大差 %.0f  明显不同 %d/%zu\\n",
               sum / (double)ref.size(), mx, nd, ref.size());
        Image ri(W, H);
        for (int i = 0; i < W * H; i++) ri.data()[i] = ref[(size_t)i];
        writePNG(out + "_ref.png", ri);
    }
    printf("  锥追踪：跑了 %d 帧（逐帧旋转锥集合 + 帧间平均）\n", r->accumFrames());

    Image img(W, H);
    for (int i = 0; i < W * H; i++) img.data()[i] = px[(size_t)i];
    const std::string path = out + ".png";
    writePNG(path, img);

    if (ring) {
        // 沿半径 R 的圆环统计：半球积分径向对称 -> min 应当接近 max
        const double cxf = (double)(W / 2) + 0.5, cyf = (double)(H / 2) + 0.5;
        printf("  圆环统计（亮度的最小/最大/均值，理想情况 min≈max）：\\n");
        for (int R = 6; R <= 40; R += 4) {
            int mn = 999, mx = -1; double sum = 0; int n = 0;
            for (int i = 0; i < 720; i++) {
                const double a = i * 3.14159265 / 360.0;
                const int x = (int)std::lround(cxf + R * std::cos(a));
                const int y = (int)std::lround(cyf + R * std::sin(a));
                if (x < 0 || y < 0 || x >= W || y >= H) continue;
                const int g = px[(size_t)y * W + x].g;
                if (g < mn) mn = g;
                if (g > mx) mx = g;
                sum += g; n++;
            }
            if (n == 0) continue;
            const double mean = sum / n;
            printf("    R=%2d  min=%3d  max=%3d  mean=%6.1f  不匀度=%.1f%%\\n", R, mn, mx, mean,
                   mean > 0.5 ? 100.0 * (mx - mn) / mean : 0.0);
        }
    }

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
    if (ctx) ctx->destroy();
    return 0;
}
