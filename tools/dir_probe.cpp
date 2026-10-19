// tools/dir_probe.cpp — 扫描 GI 的**方向性伪影**诊断台
//
// ## 它要回答的问题
//
// 目标：让 R=32（184 个方向）的效果逼近 R=128/256（728/1448 个方向）。
// 要做到这一点，必须先能**看见并量化**那部分误差，而不是凭感觉调。
//
// 这个工具做的事：
//   1. 搭一个**正确的图层结构**：底层纯白实心（alpha=255）+ 上层自发光图案。
//      ⚠️ 这一步是关键。受光结果是"把收集到的光乘到 albedo 上"，所以底下
//      必须有一层实心纸；否则画面全黑，什么伪影都看不见（我最初就栽在这，
//      用透明底测了好几轮，量到的一直是 0）。
//   2. 同一场景在多个半径下渲染，把结果与 R=<ref> 的基准逐像素比。
//   3. 额外输出**误差放大图**（误差 ×放大系数），这样条带/星芒这类结构性
//      伪影会直接显形 —— 只报一个"平均差 0.002"是看不出结构的。
//
// 用法：
//   dir_probe --scene point --sizes 32,64,128,256
//   dir_probe --scene line --ref 256
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "core/console.h"
#include "core/image.h"
#include "core/layer.h"
#include "core/material.h"
#include "core/types.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/light_sweep.h"

using namespace pb;
using namespace pb::tools;

namespace {

int g_w = 384, g_h = 216;
std::string g_scene = "point";
std::string g_out = "out/dir";
int g_ref = 256;
std::vector<int> g_radii = {16, 32, 64, 128, 256};
float g_emit = 1.0f;
float g_amplify = 40.0f;   // 误差放大系数（只为看图）
int g_ss = 1;              // 角度超采样倍率
float g_diff = 0.0f;       // 相邻射线扩散比例（0=关）
int g_diffstep = 8;        // 每多少步交换一次
int g_diffdbg = 0;         // 诊断：1只写不读 2完全不混
bool g_diffenable = false; // 是否启用扩散路径（kernel 改写版）

RGBA8 rgbx(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return RGBA8{r, g, b, a}; }

// ---------------------------------------------------------------------------
// 场景：一律"白底(层0) + 发光图案(层1)"
// ---------------------------------------------------------------------------
void buildScene(Document& doc, const std::string& name) {
    const int W = doc.width(), H = doc.height();
    doc.setActiveIndex(0);
    while (doc.layerCount() < 2) doc.addLayer("");
    while (doc.layerCount() > 2) doc.removeLayer(doc.layerCount() - 1);

    Layer& base = doc.layer(0);
    base.name = "白底";
    base.visible = true;
    base.opacity = 255;
    base.image().fill(rgbx(255, 255, 255, 255));
    for (auto& m : base.material) m = Material{};
    base.markAllDirty();

    Layer& top = doc.layer(1);
    top.name = "发光层";
    top.visible = true;
    top.opacity = 255;
    top.image().fill(RGBA8{0, 0, 0, 0});
    for (auto& m : top.material) m = Material{};
    Material em;
    em.setEmissionFrom(255, 255, 255);

    if (name == "line") {
        // 一条 1 像素宽的细亮线：phi(theta) 接近 δ，最能暴露方向性伪影
        for (int x = W / 4; x < W * 3 / 4; x++)
            top.setPixelRaw(x, H / 2, rgbx(255, 255, 255, 180), em);
    } else if (name == "grid") {
        // 若干孤立亮点
        for (int i = 0; i < 5; i++)
            top.setPixelRaw(W / 6 + i * (W / 6), H / 3 + (i % 2) * (H / 3),
                            rgbx(255, 255, 255, 200), em);
    } else if (name == "block") {
        // 一个较大的方块（对照：光源大则角度分布宽，伪影应明显更轻）
        top.fillRect(W / 2 - 16, H / 2 - 16, W / 2 + 16, H / 2 + 16,
                     rgbx(255, 255, 255, 200), em);
    } else {   // point（默认）
        const int cx = W / 2, cy = H / 2;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                top.setPixelRaw(cx + dx, cy + dy, rgbx(255, 255, 255, 200), em);
    }
    top.markAllDirty();
}

// 把图像 center 附近 w0 x h0 的区域最近邻放大 z 倍存出来。
//
// 为什么需要它：发光点附近的亮度很低（平均 <1/255），整幅缩略图根本看不清
// 辐条结构。放大光源附近那一小块，辐条才肉眼可辨 —— 这是肉眼验收的前提。
bool zoomSave(const std::string& path, const Image& src, int cx, int cy, int w0, int h0,
              int z) {
    const int dw = w0 * z, dh = h0 * z;
    Image out(dw, dh);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            const int sx = cx - w0 / 2 + x / z;
            const int sy = cy - h0 / 2 + y / z;
            RGBA8 c{0, 0, 0, 255};
            if (sx >= 0 && sy >= 0 && sx < src.width() && sy < src.height())
                c = src.data()[(size_t)sy * src.width() + sx];
            out.data()[(size_t)y * dw + x] = c;
        }
    }
    return writePNG(path, out);
}

void usage() {
    printf(
        "dir_probe — 扫描 GI 方向性伪影诊断\n"
        "  --scene point|line|grid|block\n"
        "  --size WxH      默认 384x216\n"
        "  --radii a,b,c   要测的半径，默认 16,32,64,128,256\n"
        "  --ref R         误差基准半径，默认 256\n"
        "  --emit F        自发光倍率，默认 1\n"
        "  --amp F         误差放大系数（看图用），默认 40\n"
        "  --ss N          角度超采样倍率（1=关）\n"
        "  --out PREFIX    输出前缀，默认 out/dir\n");
}

}  // namespace

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------
//
// 注意：Document/Layer 的"脏矩形上传"要用 LightSweep::uploadLayer(layer, albedo,
// mats, rect)。这里图省事每层整块上传一次，够用且不易错。
int main(int argc, char** argv) {
    setupConsoleUtf8();
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--scene") { const char* v = next(); if (!v) return 2; g_scene = v; }
        else if (a == "--size") { const char* v = next(); if (!v) return 2; if (!parseSize(v, g_w, g_h)) return 2; }
        else if (a == "--ref") { const char* v = next(); if (!v) return 2; g_ref = atoi(v); }
        else if (a == "--emit") { const char* v = next(); if (!v) return 2; g_emit = (float)atof(v); }
        else if (a == "--amp") { const char* v = next(); if (!v) return 2; g_amplify = (float)atof(v); }
        else if (a == "--out") { const char* v = next(); if (!v) return 2; g_out = v; }
        else if (a == "--ss") { const char* v = next(); if (!v) return 2; g_ss = std::max(1, atoi(v)); }
        else if (a == "--diff") { const char* v = next(); if (!v) return 2; g_diff = (float)atof(v); }
        else if (a == "--diffstep") { const char* v = next(); if (!v) return 2; g_diffstep = std::max(1, atoi(v)); }
        else if (a == "--diffdbg") { const char* v = next(); if (!v) return 2; g_diffdbg = atoi(v); }
        else if (a == "--diffenable") { g_diffenable = true; }
        else if (a == "--radii") {
            const char* v = next(); if (!v) return 2;
            g_radii.clear();
            std::string s = v;
            size_t pos = 0;
            while (pos < s.size()) {
                const size_t c = s.find(',', pos);
                g_radii.push_back(atoi(s.substr(pos, c - pos).c_str()));
                if (c == std::string::npos) break;
                pos = c + 1;
            }
        } else { fprintf(stderr, "[dir] 未知参数: %s\n", a.c_str()); return 2; }
    }
    if (g_radii.empty()) g_radii.push_back(g_ref);

    gfx::GLContext ctx;
    if (!createOffscreenContext(ctx, g_w, g_h, "dir_probe")) {
        fprintf(stderr, "[dir] 无 GL 上下文\n");
        return 1;
    }
    printf("[dir] GPU: %s\n", (const char*)glGetString(GL_RENDERER));

    Document doc;
    doc.resize(g_w, g_h);
    buildScene(doc, g_scene);
    printf("[dir] 场景 %s  画布 %dx%d  图层 %d（底=纯白实心，上层=发光）  角度超采样 x%d\n",
           g_scene.c_str(), g_w, g_h, doc.layerCount(), g_ss);

    const Rect all{0, 0, g_w - 1, g_h - 1};
    std::vector<Image> shots;
    std::vector<int> dirCounts;
    std::vector<double> times;

    for (int r : g_radii) {
        gfx::LightSweep sweep;
        if (!sweep.init(g_w, g_h)) { fprintf(stderr, "[dir] sweep init 失败\n"); return 1; }
        sweep.settings().rayRadius = r;
        sweep.settings().emitScale = g_emit;
        sweep.setAngularSS(g_ss);
        sweep.setDiffusion(g_diff, g_diffstep);
        sweep.setDiffusionDbg(g_diffdbg);
        sweep.setDiffusionEnable(g_diffenable);
        sweep.settings().ambient[0] = 0.0f;
        sweep.settings().ambient[1] = 0.0f;
        sweep.settings().ambient[2] = 0.0f;

        const int n = doc.layerCount();
        sweep.setLayerCount(n);
        // 先用一次 renderLayer 把方向表建起来（方向数要等它建完才准）
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) {
            const Layer& L = doc.layer(i);
            sweep.uploadLayer(i, L.albedo, L.material.data(), all);
            sweep.uploadLayerOpacity(i, (float)L.opacity / 255.0f);
            sweep.renderLayer(i);
        }
        sweep.setFieldStrength(1.0f);
        sweep.composite();
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();

        Image img(g_w, g_h);
        if (!gfx::readbackTexture(sweep.resultTexture(), g_w, g_h, img.data())) {
            fprintf(stderr, "[dir] 回读失败\n");
            return 1;
        }
        for (size_t k = 0; k < img.pixelCount(); k++) img.data()[k].a = 255;
        char path[512];
        snprintf(path, sizeof(path), "%s_%s_r%d.png", g_out.c_str(), g_scene.c_str(), r);
        writePNG(path, img);
        {
            char zp[512];
            snprintf(zp, sizeof(zp), "%s_%s_r%d_zoom.png", g_out.c_str(), g_scene.c_str(), r);
            zoomSave(zp, img, g_w / 2, g_h / 2, 96, 96, 6);
        }

        const LumaStats st = lumaStats(img);
        printf("  R=%-4d 方向 %-5d 平均亮度 %8.3f  最大 %6.1f  %.2f ms  -> %s\n", r,
               sweep.directionCount(), st.mean, st.max,
               std::chrono::duration<double, std::milli>(t1 - t0).count(), path);
        shots.push_back(std::move(img));
        dirCounts.push_back(sweep.directionCount());
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        sweep.shutdown();
    }

    // 找基准
    int refIdx = -1;
    for (size_t i = 0; i < g_radii.size(); i++)
        if (g_radii[i] == g_ref) refIdx = (int)i;
    if (refIdx < 0) refIdx = (int)g_radii.size() - 1;

    printf("\n[dir] 与 R=%d 基准的误差（误差 = |a-b| 的最大通道差）\n", g_radii[(size_t)refIdx]);
    printf("  半径   方向数   平均误差   RMS    最大误差   >2 的像素占比\n");
    const Image& ref = shots[(size_t)refIdx];
    for (size_t i = 0; i < shots.size(); i++) {
        double sum = 0, sq = 0;
        int mx = 0, over = 0;
        const size_t np = ref.pixelCount();
        for (size_t k = 0; k < np; k++) {
            const RGBA8& a = ref.data()[k];
            const RGBA8& b = shots[i].data()[k];
            const int d = std::max(std::abs((int)a.r - b.r),
                                   std::max(std::abs((int)a.g - b.g),
                                            std::abs((int)a.b - b.b)));
            sum += d;
            sq += (double)d * d;
            if (d > mx) mx = d;
            if (d > 2) over++;
        }
        printf("  %-6d %-8d %8.3f %8.3f %8d %12.4f%%\n", g_radii[i], dirCounts[i],
               sum / (double)np, std::sqrt(sq / (double)np), mx,
               100.0 * (double)over / (double)np);
    }

    // 误差放大图：只对非基准档位出，结构伪影会直接显形
    printf("\n[dir] 误差放大图（×%.0f，越亮说明该处与基准差越大）\n", (double)g_amplify);
    for (size_t i = 0; i < shots.size(); i++) {
        if ((int)i == refIdx) continue;
        Image amp(g_w, g_h);
        for (size_t k = 0; k < ref.pixelCount(); k++) {
            const RGBA8& a = ref.data()[k];
            const RGBA8& b = shots[i].data()[k];
            auto sc = [&](int d) {
                const int v = (int)std::lround((double)d * (double)g_amplify);
                return (uint8_t)(v > 255 ? 255 : v);
            };
            const uint8_t d = sc(std::max(std::abs((int)a.r - b.r),
                                          std::max(std::abs((int)a.g - b.g),
                                                   std::abs((int)a.b - b.b))));
            amp.data()[k] = RGBA8{d, d, d, 255};
        }
        char path[512];
        snprintf(path, sizeof(path), "%s_err_%s_r%d_x%d.png", g_out.c_str(), g_scene.c_str(),
                 g_radii[i], (int)g_amplify);
        writePNG(path, amp);
        printf("  R=%-4d -> %s\n", g_radii[i], path);
    }
    return 0;
}
