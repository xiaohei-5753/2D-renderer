// tools/dz_dump.cpp — 数值验证：白底层 + 高墙，看影子和亮度对不对
#include <cstdio>
#include <vector>

#include "core/material.h"
#include "core/types.h"
#include "gfx/dz_sweep.h"
#include "gfx/gl_context.h"

using namespace pb;

int main() {
    const int W = 32, H = 8, L = 4;
    gfx::ContextDesc desc;
    desc.width = 64; desc.height = 64; desc.visible = false; desc.title = "dz_dump";
    gfx::GLContext ctx;
    if (!ctx.create(desc)) return 1;

    gfx::DzSweep dz;
    if (!dz.init(W, H, L)) return 1;

    // 场景：**纯白底层**（层 1）保证一定有受光像素 + 第 15 列高墙（层 3）
    gfx::SurfaceField f;
    f.w = W; f.h = H; f.layers = L;
    const size_t n = (size_t)W * H;
    // 底层半透明(a=64)：这样 mix(I+E, C, A) 里辐照度 I 才看得见。
    // 若 a=255，规格给出的就是"显示自身颜色 C"，与光照无关（我先前被这一点误导）
    f.albedo.assign(n, RGBA8{255, 255, 255, 64});
    f.material.assign(n, Material{});
    f.height.assign(n, 1);
    for (int y = 0; y < H; y++) {
        const size_t i = (size_t)y * W + 15;
        f.height[i] = 3;
        f.albedo[i] = RGBA8{255, 100, 100, 255};   // 墙：不透明
    }
    dz.uploadField(f);

    gfx::DzSweepParams prm;
    prm.directions = 1;
    dz.setRayRadius(64);
    prm.sky[0] = 0.5f; prm.sky[1] = 0.5f; prm.sky[2] = 0.5f;
    if (!dz.render(prm)) { fprintf(stderr, "render failed\n"); return 1; }

    std::vector<uint8_t> px((size_t)W * H * 4u, 0);
    glBindTexture(GL_TEXTURE_2D, dz.resultTexture());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    printf("canvas %dx%d  半透明白底层(层1, a=64) + 第15列高墙(层3)  dirs=1  m=64  sky=0.5\n", W, H);
    printf("  x:  ");
    for (int x = 0; x < W; x++) printf("%4d", x);
    printf("\n  R:  ");
    for (int x = 0; x < W; x++) printf("%4d", px[((size_t)(H / 2) * W + x) * 4]);
    printf("\n");

    int nonZero = 0;
    for (size_t i = 0; i < (size_t)W * H; i++)
        if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2]) nonZero++;
    printf("  非零像素 %d / %d\n", nonZero, W * H);

    dz.shutdown();
    ctx.destroy();
    return 0;
}
