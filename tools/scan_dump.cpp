// tools/scan_dump.cpp — 逐层光照的诊断工具：把每个中间阶段dump成 PNG
//
// 为什么需要它：这次逐层化改造里，"最终画面不对"可能出在四个地方
// （扫描 / SSBO->纹理 / 合成单层 / 逐层合成），而最终画面只有一个数字，
// 无法区分。把中间阶段直接dump出来，才能一次定位。
//
// 用法: scan_dump <场景> [--size WxH] [--radius R] [--out 前缀]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"
#include "core/canvas.h"
#include "core/image.h"
#include "core/material.h"
#include "core/types.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/layer_composite.h"
#include "gfx/light_sweep.h"

using namespace pb;
using namespace pb::tools;

namespace {

// 与 render_cli 的 neon 场景保持一致，便于对照
void buildScene(Canvas& c, const std::string& name) {
    const int W = c.width(), H = c.height();
    c.clear(RGBA8{0, 0, 0, 0}, Material{});
    Material em;
    em.setEmissionFrom(0, 200, 255);
    // 一条霓虹线 + 一个实心块
    c.drawLine(W / 4, H / 4, W - W / 4, H / 4, RGBA8{90, 200, 255, 110}, em);
    c.fillRect(W / 2 - 10, H / 2 - 8, W / 2 + 10, H / 2 + 8, RGBA8{44, 44, 54, 255},
               Material{});
    (void)name;
}

std::string g_prefix = "out/dump";

void dumpTex(const char* tag, GLuint tex, int w, int h) {
    if (!tex) { printf("  %-12s <no texture>\n", tag); return; }
    Image img(w, h);
    if (!gfx::readbackTexture(tex, w, h, img.data())) {
        printf("  %-12s readback FAILED\n", tag);
        return;
    }
    const LumaStats st = lumaStats(img);
    const double mean = st.mean, mx = st.max;
    char path[512];
    snprintf(path, sizeof(path), "%s_%s.png", g_prefix.c_str(), tag);
    writePNG(path, img);
    printf("  %-12s 平均亮度 %7.2f  最大 %6.1f  -> %s\n", tag, mean, mx, path);
}

}  // namespace

int main(int argc, char** argv) {
    int W = 320, H = 180, radius = 128;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            // 与 render_cli 同一套 WxH 解析。返回值刻意不看：旧代码就是"格式不对
            // 就保持原值"，parseSize 只是把同一行 sscanf 收到 common.h 里。
            (void)parseSize(argv[++i], W, H);
        } else if (std::strcmp(argv[i], "--radius") == 0 && i + 1 < argc) {
            radius = atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            g_prefix = argv[++i];
        }
    }

    gfx::GLContext ctx;
    if (!createOffscreenContext(ctx, 64, 64, "scan_dump")) {
        fprintf(stderr, "no context\n");
        return 1;
    }

    Canvas c(W, H);
    buildScene(c, "neon");
    printf("画布 %dx%d\n", W, H);

    gfx::LightSweep sweep;
    if (!sweep.init(W, H)) { fprintf(stderr, "sweep init failed\n"); return 1; }
    sweep.settings().rayRadius = radius;
    sweep.settings().ambient[0] = 0.10f;
    sweep.settings().ambient[1] = 0.10f;
    sweep.settings().ambient[2] = 0.12f;

    printf("[阶段] 只跑扫描通道（不合成）\n");
    sweep.uploadAll(c.image(), c.materialData());
    sweep.render();
    GLenum e = glGetError();
    if (e) printf("  GL error 0x%04X\n", e);

    // litArray 是数组纹理：直接量它的 layer 0
    {
        std::vector<RGBA8> lp((size_t)W * H);
        glBindTexture(GL_TEXTURE_2D_ARRAY, sweep.litTexture());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, lp.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        const RGBA8 c = lp[(size_t)45 * W + 80];
        printf("  lit层0 @(80,45) = (%d,%d,%d,a=%d)\\n", c.r, c.g, c.b, c.a);
    }
    dumpTex("scan", sweep.scanTexture(), W, H);
    dumpTex("result", sweep.resultTexture(), W, H);

    // -----------------------------------------------------------------------
    // 朝向自检
    //
    // 这个错**在界面上看不出来**：画布显示走 glReadPixels（那边有自己的翻转），
    // 只有导出的 PNG 会颠倒 —— 所以它绕过一切目视检查，直到用户导出才发现。
    //
    // 做法：造一张只有左上角有记号的图，跑**完整管线**后逐级回读，
    // 看记号落在哪一行。三张纹理都测，才能确定翻转该加在哪一级。
    // -----------------------------------------------------------------------
    {
        printf("[朝向自检]\n");
        const int TW = 16, TH = 16;
        Canvas tiny(TW, TH);
        tiny.image().fill(RGBA8{0, 0, 0, 255});
        // 左上角 4x4 **强自发光**记号。
        // 用自发光而不是普通白色：普通白色经光照后会被衰减到接近 0
        // （第一版就是这么测的，scan/result 全黑、什么也判不出来）。
        for (auto& m : tiny.materials()) m = Material{};
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                Material em;
                em.setEmissionFrom(255, 255, 255);
                tiny.image().at(x, y) = RGBA8{255, 255, 255, 255};
                tiny.materials()[(size_t)y * TW + x] = em;
            }

        gfx::LightSweep ts;
        if (ts.init(TW, TH)) {
            // 环境光拉满：记号必须够亮才能在扫描/合成后仍可辨认
            ts.settings().ambient[0] = 1.0f;
            ts.settings().ambient[1] = 1.0f;
            ts.settings().ambient[2] = 1.0f;
            ts.settings().rayRadius = 8;
            ts.uploadAll(tiny.image(), tiny.materialData());
            ts.render();   // 扫描 + 合成，走完整管线

            // 只报告"最亮行在第几行"，**不判断是顶还是底**：
            // 行号的上/下取决于坐标系，我之前就是在这一步把结论搞反的。
            // 真正的判据是"导出的 PNG 看起来对不对"（见 render_cli --scene orient）。
            auto markRow = [&](GLenum target, GLuint tex, const char* tag) -> int {
                std::vector<RGBA8> v((size_t)TW * TH);
                glBindTexture(target, tex);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glGetTexImage(target, 0, GL_RGBA, GL_UNSIGNED_BYTE, v.data());
                glBindTexture(target, 0);
                int row = -1, best = -1, last = -1;
                for (int y = 0; y < TH; y++) {
                    const int lum = (int)v[(size_t)y * TW].r + v[(size_t)y * TW].g + v[(size_t)y * TW].b;
                    if (lum > 100) last = y;
                    if (lum > best) { best = lum; row = y; }
                }
                printf("  %-8s 亮像素行区间 首=%d 末=%d（行号仅作记录，不代表上下）\n",
                       tag, row, last);
                return row;
            };
            const int rc = markRow(GL_TEXTURE_2D_ARRAY, ts.canvasTexture(), "canvas");
            const int rs = markRow(GL_TEXTURE_2D, ts.scanTexture(), "scan");
            const int rr = markRow(GL_TEXTURE_2D, ts.resultTexture(), "result");
            printf("  （三张纹理行号：canvas=%d scan=%d result=%d，三者相同即说明管线内部一致）\n",
                   rc, rs, rr);
            ts.shutdown();
        } else {
            printf("[朝向自检] tiny sweep init 失败，跳过\n");
        }
    }

    // canvas 数组纹理的 layer 0：用 glGetTexImage(GL_TEXTURE_2D_ARRAY) 直接量，
    // 不经过任何采样/合成，排除采样端的可能
    {
        std::vector<RGBA8> px((size_t)W * H);
        glBindTexture(GL_TEXTURE_2D_ARRAY, sweep.canvasTexture());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        const GLenum err = glGetError();
        int nonZero = 0, maxA = 0;
        for (const RGBA8& c : px) {
            if (c.r || c.g || c.b || c.a) nonZero++;
            if (c.a > maxA) maxA = c.a;
        }
        // 取一个非空像素报出来
        printf("  canvas层0      非空像素 %d  最大 alpha %d  (GL err 0x%04X)\n", nonZero,
               maxA, err);
        for (int i = 0; i < (int)px.size(); i++) {
            if (px[(size_t)i].a > 0) {
                printf("  canvas层0 首个非空像素 #%d = (%d,%d,%d,a=%d)\n", i, px[(size_t)i].r,
                       px[(size_t)i].g, px[(size_t)i].b, px[(size_t)i].a);
                break;
            }
        }
    }

    // 不透明度数组纹理（R32F，每层一个值）只能逐层回读，用 glGetTexImage 直接量
    {
        std::vector<float> op((size_t)W * H, 0.0f);
        glBindTexture(GL_TEXTURE_2D_ARRAY, sweep.opacityTexture());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RED, GL_FLOAT, op.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        const GLenum err = glGetError();
        double sum = 0, mx = 0;
        for (float v : op) { sum += v; if (v > mx) mx = v; }
        printf("  不透明度层0    平均 %7.4f  最大 %6.3f  (GL err 0x%04X)\n",
               (double)(sum / (double)op.size()), mx, err);
    }

    // 逐层结果（数组纹理）要一层一层读：glGetTexImage 不能整体读数组纹理的某一层，
    // 所以用 LayerComposite 把它当作"只有一层"来合成为一张 2D 图。
    // 这同时也是在验证 LayerComposite 与数组纹理上传本身。
    {
        gfx::LayerComposite comp;
        if (comp.init()) {
            GLuint flat = gfx::createTexture(GL_RGBA8, W, H, GL_RGBA, GL_UNSIGNED_BYTE);

            // 未受光的 albedo：alpha 是"有多不透明" -> (alphaIsCoverage=false,
            // albedoAsOpaque=true)：颜色原样显示，alpha 只决定透出下面多少
            comp.composite(sweep.canvasTexture(), sweep.opacityTexture(), 1, W, H, flat,
                           true);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            dumpTex("albedo", flat, W, H);
            // 受光结果：走 alphaIsCoverage=false（受光结果的 alpha 是可见度）
            comp.composite(sweep.litTexture(), sweep.opacityTexture(), 1, W, H, flat,
                           false);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            dumpTex("lit0", flat, W, H);

            glDeleteTextures(1, &flat);
            comp.shutdown();
        }
    }

    sweep.shutdown();
    ctx.destroy();
    return 0;
}
