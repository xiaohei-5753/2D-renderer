// tools/renderer_api_smoke.cpp — 渲染器库的**边界测试**
//
// 这个文件只 include `gfx/renderer.h`（渲染器的公开门面），不碰 app/、不碰 ui/、
// 不碰 scenes/。它能编译并跑通，就说明"渲染器可以独立当库用"这件事是成立的，
// 而不是只有仓库内部能编过。
//
// 它做的事就是头文件注释里那段最小用法：
//   建 Document -> 画两个像素 -> 上传 -> 渲染两层 -> 合成 -> 回读一个像素。
//
// 退出码 0 = 通过；非 0 = 失败（用于 CI / 打包前自检）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/console.h"
#include "gfx/renderer.h"

int main() {
    pb::setupConsoleUtf8();

    pb::Document doc(64, 48);
    doc.addLayer("bg");
    doc.addLayer("fg");

    // 底层：一整块中间灰的墙 + 一个自发光像素
    {
        pb::Layer& L = doc.layer(0);
        for (int y = 0; y < doc.height(); y++)
            for (int x = 0; x < doc.width(); x++)
                L.setPixelRaw(x, y, pb::RGBA8{110, 116, 128, 255}, pb::Material{});
        pb::Material em;
        em.setEmissionFrom(255, 80, 160);
        L.setPixelRaw(20, 24, pb::RGBA8{255, 255, 255, 255}, em);
    }
    // 上层：一个半透明的青色方块（验证分层 + 覆盖率）
    {
        pb::Layer& L = doc.layer(1);
        for (int y = 10; y < 30; y++)
            for (int x = 30; x < 50; x++)
                L.setPixelRaw(x, y, pb::RGBA8{80, 220, 230, 140}, pb::Material{});
    }

    pb::gfx::GLContext ctx;
    pb::gfx::ContextDesc cd;
    cd.width = 64;
    cd.height = 48;
    cd.visible = false;
    cd.title = "renderer_api_smoke";
    if (!ctx.create(cd)) {
        std::fprintf(stderr, "[smoke] GL 上下文创建失败\n");
        return 2;
    }
    pb::gfx::LightSweep sweep;
    if (!sweep.init(doc.width(), doc.height())) {
        std::fprintf(stderr, "[sweep] 初始化失败\n");
        return 3;
    }
    sweep.setLayerCount(doc.layerCount());
    sweep.settings().rayRadius = 8;   // 测试只求通，不求画质
    const pb::Rect all{0, 0, doc.width(), doc.height()};
    for (int i = 0; i < doc.layerCount(); i++) {
        sweep.uploadLayer(i, doc.layer(i).albedo, doc.layer(i).material.data(), all);
        sweep.uploadLayerOpacity(i, 1.0f);
        sweep.renderLayer(i);
    }
    sweep.composite();

    std::vector<pb::RGBA8> out((size_t)doc.width() * doc.height());
    if (!pb::gfx::readbackTexture(sweep.resultTexture(), doc.width(), doc.height(), out.data())) {
        std::fprintf(stderr, "[smoke] 回读失败\n");
        return 4;
    }
    // 自发光像素附近的墙应当被染色（> 纯灰的 R 分量），且整体不为全黑
    const pb::RGBA8 wall = out[(size_t)24 * doc.width() + 22];
    const pb::RGBA8 dark = out[(size_t)2 * doc.width() + 2];
    const int sum = (int)wall.r + wall.g + wall.b;
    std::printf("[smoke] 墙(22,24)=(%d,%d,%d) 远处(2,2)=(%d,%d,%d)\n", wall.r, wall.g, wall.b,
                dark.r, dark.g, dark.b);
    if (sum < 30) {
        std::fprintf(stderr, "[smoke] 画面过暗，光照没跑起来\n");
        return 5;
    }
    std::printf("[smoke] OK：渲染器库可独立使用（层数 %d，方向 %d）\n", doc.layerCount(),
                sweep.directionCount());
    return 0;
}
