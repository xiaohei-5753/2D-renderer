// tools/comp_test.cpp — 直接验证 LayerComposite 的数学（不经过光照/上传管线）
//
// 为什么值得单独测：这次排查我绕了很久，因为"最终画面不对"无法区分是
// 扫描、上传、照亮还是合成的问题。这个测试把合成单独拎出来，用**已知输入**
// 断言输出，让上面那些阶段再也无法互相掩盖。
#include <cstdio>
#include <vector>

#include "core/types.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/layer_composite.h"

using namespace pb;

namespace {

int g_fail = 0;

void expectPixel(const char* what, const RGBA8 got, int r, int g, int b) {
    const bool ok = got.r == r && got.g == g && got.b == b;
    printf("  %-46s 得到 (%3d,%3d,%3d)  期望 (%3d,%3d,%3d)  %s\n", what, got.r, got.g,
           got.b, r, g, b, ok ? "OK" : "FAIL");
    if (!ok) g_fail++;
}

}  // namespace

int main() {
    gfx::ContextDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.visible = false;
    desc.title = "comp_test";
    gfx::GLContext ctx;
    if (!ctx.create(desc)) { fprintf(stderr, "no context\n"); return 1; }

    const int W = 2, H = 2;
    gfx::LayerComposite comp;
    if (!comp.init()) { fprintf(stderr, "composite init failed\n"); return 1; }

    // 两层：
    //   L0（底）：4 个像素分别是 不透明红 / 半透明蓝(a=128) / 空气 / 不透明灰
    //   L1（上）：4 个像素分别是 空气 / 空气 / 半透明绿(a=128) / 不透明黄
    // 逐层照亮后的"受光结果"用 (颜色, 可见度=1-cov) 表示
    struct Px { uint8_t r, g, b, cov; };
    const Px lit0[4] = {{255, 0, 0, 255}, {0, 0, 255, 128}, {0, 0, 0, 0}, {128, 128, 128, 255}};
    const Px lit1[4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 255, 0, 128}, {255, 255, 0, 255}};
    // 未受光的 albedo 用 (颜色, 覆盖率) 表示 —— 注意半透明像素的颜色是**原样**的
    const Px alb[4] = {{255, 0, 0, 255}, {0, 0, 255, 128}, {0, 0, 0, 0}, {128, 128, 128, 255}};

    auto makeLayer = [&](const Px* px) {
        GLuint t = gfx::createTextureArray(GL_RGBA8, W, H, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        std::vector<RGBA8> data(4);
        for (int i = 0; i < 4; i++) data[(size_t)i] = RGBA8{px[i].r, px[i].g, px[i].b, px[i].cov};
        gfx::uploadArrayLayer(t, 0, W, H, data.data());
        return t;
    };
    auto makeOpacity = [&](float v) {
        GLuint t = gfx::createTextureArray(GL_R32F, W, H, 1, GL_RED, GL_FLOAT);
        gfx::uploadFloatArrayLayer(t, 0, W, H, v);
        return t;
    };

    GLuint out = gfx::createTexture(GL_RGBA8, W, H, GL_RGBA, GL_UNSIGNED_BYTE);
    auto read = [&](RGBA8* dst) {
        glBindTexture(GL_TEXTURE_2D, out);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        glBindTexture(GL_TEXTURE_2D, 0);
    };

    // ---------------------------------------------------------------
    // 1) 原始 albedo（单层，不透明度 1）
    //    alphaIsCoverage=false（alpha 是"有多不透明"）+ albedoAsOpaque=true
    //    期望：颜色**原样**显示 —— 半透明蓝也是 (0,0,255)，不被自己的 alpha 乘暗
    // ---------------------------------------------------------------
    printf("[1] 原始 albedo 单层：颜色应当原样显示\n");
    {
        GLuint l = makeLayer(alb), o = makeOpacity(1.0f);
        comp.composite(l, o, 1, W, H, out, true);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        RGBA8 got[4];
        read(got);
        expectPixel("L0 不透明红", got[0], 255, 0, 0);
        // 单层、背景为黑：看到的就是 alpha 之后的合成结果 255*0.5=128
        expectPixel("L0 半透明蓝(a=128) -> 128", got[1], 0, 0, 128);
        expectPixel("L0 空气", got[2], 0, 0, 0);
        expectPixel("L0 不透明灰", got[3], 128, 128, 128);
        glDeleteTextures(1, &l);
        glDeleteTextures(1, &o);
    }

    // ---------------------------------------------------------------
    // 2) 受光结果（单层）
    //    alphaIsCoverage=false（alpha 是可见度 1-cov）+ albedoAsOpaque=false
    //    期望：实心像素显示自身颜色；空气显示它的颜色（光晕）——**不能被丢掉**
    // ---------------------------------------------------------------
    printf("[2] 受光结果单层：空气的光晕必须留下\n");
    {
        // 把"空气"那格给一个颜色，模拟"这一层贡献的光晕"
        Px litA[4] = {{255, 0, 0, 255}, {0, 0, 255, 128}, {10, 20, 30, 0}, {128, 128, 128, 255}};
        GLuint l = makeLayer(litA), o = makeOpacity(1.0f);
        comp.composite(l, o, 1, W, H, out, false);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        RGBA8 got[4];
        read(got);
        expectPixel("实心红 -> 自身颜色", got[0], 255, 0, 0);
        expectPixel("半透明蓝 -> 自身颜色(不衰减)", got[1], 0, 0, 255);
        expectPixel("空气的光晕(10,20,30) -> 留下", got[2], 10, 20, 30);
        expectPixel("实心灰 -> 自身颜色", got[3], 128, 128, 128);
        glDeleteTextures(1, &l);
        glDeleteTextures(1, &o);
    }

    // ---------------------------------------------------------------
    // 3) 两层叠加（受光结果）：上层实心应盖住下层，上层空气应露出下层
    //    这次用两个数组纹理各含一层是不行的（一次 composite 只吃一个数组），
    //    所以把两层塞进同一个数组纹理来测多层的叠加顺序。
    // ---------------------------------------------------------------
    printf("[3] 两层叠加：上层盖住下层，上层空气露出下层\n");
    {
        GLuint arr = gfx::createTextureArray(GL_RGBA8, W, H, 2, GL_RGBA, GL_UNSIGNED_BYTE);
        auto up = [&](int layer, const Px* px) {
            std::vector<RGBA8> d(4);
            for (int i = 0; i < 4; i++)
                d[(size_t)i] = RGBA8{px[i].r, px[i].g, px[i].b, px[i].cov};
            gfx::uploadArrayLayer(arr, layer, W, H, d.data());
        };
        up(0, lit0);
        up(1, lit1);

        GLuint opa = gfx::createTextureArray(GL_R32F, W, H, 2, GL_RED, GL_FLOAT);
        gfx::uploadFloatArrayLayer(opa, 0, W, H, 1.0f);
        gfx::uploadFloatArrayLayer(opa, 1, W, H, 1.0f);

        // 这一段测的是**原始 albedo** 的多层叠加，所以 albedoAsOpaque=true
        comp.composite(arr, opa, 2, W, H, out, true);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        RGBA8 got[4];
        read(got);
        // 上层那格是空气(a=0)：颜色原样（这里是黑）、下面全透出
        expectPixel("上层空气 -> 露出底层红", got[0], 255, 0, 0);
        // 底层是半透明蓝(a=128)：颜色原样 + 只透出一半背景 => 128
        expectPixel("上层空气 -> 底层半透明蓝 -> 128", got[1], 0, 0, 128);
        // 上层半透明绿(a=128)、下层空气(黑)：颜色原样但只覆盖一半 => 128
        expectPixel("上层半透明绿 -> 128", got[2], 0, 128, 0);
        expectPixel("上层实心黄 -> 盖住底层灰", got[3], 255, 255, 0);

        glDeleteTextures(1, &arr);
        glDeleteTextures(1, &opa);
    }

    // ---------------------------------------------------------------
    // 4) 图层不透明度 0.5：半透明地混进下层
    // ---------------------------------------------------------------
    printf("[4] 图层不透明度 50%%：与下层各占一半\n");
    {
        GLuint arr = gfx::createTextureArray(GL_RGBA8, W, H, 2, GL_RGBA, GL_UNSIGNED_BYTE);
        auto up = [&](int layer, const Px* px) {
            std::vector<RGBA8> d(4);
            for (int i = 0; i < 4; i++)
                d[(size_t)i] = RGBA8{px[i].r, px[i].g, px[i].b, px[i].cov};
            gfx::uploadArrayLayer(arr, layer, W, H, d.data());
        };
        // 底：不透明红；上：不透明蓝，但图层不透明度 50%
        const Px bot[4] = {{255, 0, 0, 255}, {255, 0, 0, 255}, {255, 0, 0, 255}, {255, 0, 0, 255}};
        const Px top[4] = {{0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}};
        up(0, bot);
        up(1, top);
        GLuint opa = gfx::createTextureArray(GL_R32F, W, H, 2, GL_RED, GL_FLOAT);
        gfx::uploadFloatArrayLayer(opa, 0, W, H, 1.0f);
        gfx::uploadFloatArrayLayer(opa, 1, W, H, 0.5f);

        comp.composite(arr, opa, 2, W, H, out, false);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        RGBA8 got[4];
        read(got);
        // 上层不透明蓝、图层不透明度 50% -> 与底层红各占一半
        expectPixel("上层蓝 op=50%% -> 红蓝各半", got[0], 127, 0, 127);
        glDeleteTextures(1, &arr);
        glDeleteTextures(1, &opa);
    }

    glDeleteTextures(1, &out);
    comp.shutdown();
    ctx.destroy();

    printf("\n%s\n", g_fail == 0 ? "全部通过。" : "有失败项。");
    return g_fail == 0 ? 0 : 1;
}
