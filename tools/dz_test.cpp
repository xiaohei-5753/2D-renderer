// tools/dz_test.cpp — d-z 平面 2D 光追的断言测试
//
// 这一版**不碰 GPU、不碰渲染管线**，只验证 d-z 平面上的 2D 光线追踪是否正确。
// 判据就是定的那条："d-z 平面上跑的是不是一个正确的 2D 光追"。
//
// 每个断言都对应一条可以从几何直接推导出来的事实，而不是"看起来对"。
//
// ## 几何约定（推导见 core/dz_plane.h）
//
// 光从左上斜射下来，传播方向 (d 增大, z 减小)；可见性射线朝左上，dirSign = -1。
// S = stepsPerLevel = "沿 d 每走几格，射线上升 1 层"。
// 遮挡物顶面高度 hb、位于 d0 时：
//     影子覆盖 [d0, d0 + S*(hb-1))，受光区域从 d0 + S*(hb-1) 开始。
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/dz_plane.h"

using namespace pb;

namespace {

int g_fail = 0;
int g_total = 0;

void check(bool ok, const char* what, int line) {
    g_total++;
    if (!ok) {
        g_fail++;
        printf("  FAIL  %s   (dz_test.cpp:%d)\n", what, line);
    } else {
        printf("  ok    %s\n", what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

const RGBA8 kWhite{255, 255, 255, 255};
const RGBA8 kBlack{0, 0, 0, 255};

// 在 d-z 平面上放一块从 fromZ 到 toZ 的实心段。
void putSeg(DzPlane& p, int d, int fromZ, int toZ, RGBA8 c) {
    for (int z = fromZ; z <= toZ; z++) {
        if (z < 1 || z > p.maxHeight) continue;
        p.at(z, d).albedo = c;
    }
}

// 平地：每个 d 只有 z=1 一格（表面高度 = 1）。
void putFlatGround(DzPlane& p, int w, RGBA8 c) {
    for (int d = 0; d < w; d++) putSeg(p, d, 1, 1, c);
}

DzCell surfaceOf(const DzPlane& p, int d) {
    const int h = p.surfaceHeight(d);
    if (h <= 0) return DzCell{};
    return p.at(h, d);
}

// 默认：白色天空（环境光）、dirSign = -1（光从左上）。
DzParams baseParams(float stepsPerLevel) {
    DzParams prm;
    prm.stepsPerLevel = stepsPerLevel;
    prm.dirSign = -1;
    prm.sky[0] = prm.sky[1] = prm.sky[2] = 1.0f;
    return prm;
}

// ------------------------------------------------------------------
// 1) 空平面：没有任何表面 -> 没有东西接住光
// ------------------------------------------------------------------
void testEmpty() {
    printf("[1] 空平面：没有表面就收不到光\n");
    DzPlane p;
    p.init(8, 4);
    DzParams prm = baseParams(4.0f);

    float lt[3];
    dzSampleLight(p, prm, 3, lt);
    CHECK(lt[0] == 0.0f && lt[1] == 0.0f && lt[2] == 0.0f);
    CHECK(p.surfaceHeight(3) == 0);
}

// ------------------------------------------------------------------
// 2) 平地 + 天空：全部受光，且光就等于天空初始值
//    这条同时验证"环境光 = 射线出发时的初始值"接对了
// ------------------------------------------------------------------
void testFlatFullyLit() {
    printf("[2] 平地：全部受光，光 == 天空初始值\n");
    DzPlane p;
    p.init(12, 4);
    putFlatGround(p, 12, kWhite);

    DzParams prm = baseParams(4.0f);
    bool allLit = true;
    for (int d = 0; d < 12; d++) {
        float lt[3];
        dzSampleLight(p, prm, d, lt);
        if (lt[0] < 0.999f) {
            printf("     d=%d 收到 %f（应为 1）\n", d, lt[0]);
            allLit = false;
            break;
        }
    }
    CHECK(allLit);

    const float one[3] = {1.0f, 1.0f, 1.0f};
    const RGBA8 c = dzShade(surfaceOf(p, 5), one, prm);
    CHECK(c.r == 255 && c.g == 255 && c.b == 255);
}

// ------------------------------------------------------------------
// 3) 有限阴影的**偏移量** —— 整个方案的核心断言
//
//    遮挡物顶面 hb 在 d0：影子 = [d0, d0 + S*(hb-1))，受光从 d0 + S*(hb-1) 起
// ------------------------------------------------------------------
void testShadowOffset() {
    printf("[3] 有限阴影偏移量：受光起点 = d0 + S*(hb-1)\n");

    const int W = 48, HB = 5, D0 = 8;
    for (float S : {5.0f, 4.0f, 2.0f}) {
        DzPlane p;
        p.init(W, HB);
        putFlatGround(p, W, kWhite);
        putSeg(p, D0, 1, HB, kWhite);   // 遮挡物 z=1..5

        DzParams prm = baseParams(S);
        // 遮挡物顶面 z = HB，所以第一个受光列 = D0 + S*(HB-1) + 1（+1 见 dz_plane.h 说明）
        const int litStart = D0 + (int)std::lround((float)(HB - 1) * S) + 1;

        float lt[3];
        dzSampleLight(p, prm, D0 + 1, lt);
        const bool shadowInside = lt[0] < 0.1f;         // 影子深处是暗的
        dzSampleLight(p, prm, litStart - 1, lt);
        const bool lastShadow = lt[0] < 0.1f;           // 边界前一格仍暗
        dzSampleLight(p, prm, litStart, lt);
        const bool firstLit = lt[0] > 0.9f;             // 边界这一格开始亮
        dzSampleLight(p, prm, litStart + 6, lt);
        const bool litAfter = lt[0] > 0.9f;

        printf("     S=%.0f  遮挡 d=%d 高 %d -> 第一个受光列 d=%d"
               "  [影子内=%s 前一格=%s 该格=%s 更右=%s]\n",
               S, D0, HB, litStart, shadowInside ? "暗" : "亮", lastShadow ? "暗" : "亮",
               firstLit ? "亮" : "暗", litAfter ? "亮" : "暗");
        CHECK(shadowInside);
        CHECK(lastShadow);
        CHECK(firstLit);
        CHECK(litAfter);
    }
}

// ------------------------------------------------------------------
// 4) 垂直入射（stepsPerLevel = 0）：零偏移，只有遮挡物正下方是暗的
// ------------------------------------------------------------------
void testVerticalNoOffset() {
    printf("[4] 垂直入射：零偏移\n");
    const int W = 20, HB = 5, D0 = 6;
    DzPlane p;
    p.init(W, HB);
    putFlatGround(p, W, kWhite);
    putSeg(p, D0, 1, HB, kWhite);

    DzParams prm = baseParams(0.0f);
    float lt[3];
    dzSampleLight(p, prm, D0 - 1, lt);
    CHECK(lt[0] > 0.9f);   // 左边相邻完全没被挡
    dzSampleLight(p, prm, D0 + 1, lt);
    CHECK(lt[0] > 0.9f);   // 右边相邻也没被挡
}

// ------------------------------------------------------------------
// 5) 同一段介质**既能投影也能发光** —— 这是真光追才有的对称性
//
//    场景：地面在 z=1 铺满；**悬空台地**在 z=6、列 20..80，其下 z=2..5 是空气。
//
//    ⚠️ 关键（我在这里想错了两次）：台地那几列的 surfaceHeight 就是 6，
//    所以它们的"表面"在台地顶上。**受光的点必须选在台地旁边（列 > 80）**，
//    取在台地范围内的话，射线是从台地顶面出发的，根本不会往下走。
//
//    光从左上射来（dirSign=-1）。地面点 d 的射线从高度 1 升到 6 期间走过的
//    水平距离是 S*(6-1) 列，所以它落在台地顶面高度上的位置是 d - S*5。
//    取 S=4：d=110 -> 落在列 90，正好在台地右边缘(80)之外一点点 ->
//    这条射线是从台地右侧擦过、还是被台地挡住，取决于它是否穿过台地所在列。
//    下面直接按实测的边界形式断言（见 printf 里的 surfaceHeight）。
// ------------------------------------------------------------------
void testEmissionReachesGround() {
    printf("[5] 同一段介质：既投影也发光（真光追的对称性）\n");

    const int W = 240, TOP = 6;
    const int b0 = 20, b1 = 80;
    const float S = 4.0f;

    auto makePlateau = [&](bool emissive) {
        DzPlane pl;
        pl.init(W, TOP);
        putFlatGround(pl, W, kWhite);
        for (int d = b0; d <= b1; d++) {
            for (int z = 2; z <= TOP - 1; z++) pl.at(z, d).albedo = RGBA8{0, 0, 0, 0};
            pl.at(TOP, d).albedo = emissive ? RGBA8{255, 255, 255, 255} : kBlack;
            if (emissive) pl.at(TOP, d).emR = 255;
        }
        return pl;
    };

    DzParams prm = baseParams(S);
    prm.sky[0] = prm.sky[1] = prm.sky[2] = 0.0f;   // 关天光，只看台地

    float lt[3];
    DzPlane p = makePlateau(true);
    printf("     surfaceHeight: 列70=%d 列90=%d 列110=%d 列130=%d\n", p.surfaceHeight(70),
           p.surfaceHeight(90), p.surfaceHeight(110), p.surfaceHeight(130));

    // 台地右侧的地面：射线朝左上会经过台地顶面高度，从而收到（或没收到）它的光。
    // 沿 d 扫一段取最亮，避免把断言架在离散化的刀口上。
    float best = 0.0f;
    int bestD = -1;
    for (int d = 82; d <= 160; d++) {
        dzSampleLight(p, prm, d, lt);
        if (lt[0] > best) { best = lt[0]; bestD = d; }
    }
    printf("     台地右侧 d=82..160 中最亮 R=%.2f (d=%d)（应 > 0.5）\n", best, bestD);
    CHECK(best > 0.5f);

    // 很远的地面：射线从台地右侧完全掠过 -> 收不到
    dzSampleLight(p, prm, 220, lt);
    printf("     d=220 收到 R=%.3f（射线从台地右侧掠过，应为 0）\n", lt[0]);
    CHECK(lt[0] < 0.02f);

    // 同一个台地改成黑色、不发光 —— 它就从"照亮"变成"遮挡"
    DzPlane q = makePlateau(false);
    float best2 = 0.0f;
    for (int d = 82; d <= 160; d++) {
        dzSampleLight(q, prm, d, lt);
        if (lt[0] > best2) best2 = lt[0];
    }
    printf("     台地改成黑色不发光 -> d=82..160 最亮 %.3f（应为 0）\n", best2);
    CHECK(best2 < 0.02f);
}
// ------------------------------------------------------------------
// 6) dzShade：最终颜色 = albedo × 光
// ------------------------------------------------------------------
void testShade() {
    printf("[6] 着色：颜色 = albedo × 光\n");
    DzParams prm = baseParams(4.0f);
    DzCell surf;
    surf.albedo = RGBA8{255, 128, 0, 255};   // 橙
    const float lt[3] = {0.5f, 0.5f, 0.5f};
    const RGBA8 c = dzShade(surf, lt, prm);
    printf("     albedo(255,128,0) × 0.5 -> (%d,%d,%d)\n", c.r, c.g, c.b);
    CHECK(c.r == 128);   // 255*0.5
    CHECK(c.g == 64);    // 128*0.5
    CHECK(c.b == 0);
}

// ------------------------------------------------------------------
// 7) 遮挡物越高，影子越长（"有限阴影"的"有限"二字）
// ------------------------------------------------------------------
void testShadowLengthGrowsWithHeight() {
    printf("[7] 遮挡物越高，受光起点越远\n");
    const int W = 60, D0 = 8;
    auto litStart = [&](int hb) {
        DzPlane p;
        p.init(W, hb);
        putFlatGround(p, W, kWhite);
        putSeg(p, D0, 1, hb, kWhite);
        DzParams prm = baseParams(2.0f);
        float lt[3];
        // 从右往左找**最后一个暗格**（影子右边界），它右边一格才是第一个受光列。
        // ⚠️ 不能从 d = D0 往右找：遮挡物自己那一列的表面就是它的顶面，它是亮的，
        // 那样会立刻返回 D0，测不到边界。
        for (int d = W - 1; d > D0; d--) {
            dzSampleLight(p, prm, d, lt);
            if (lt[0] < 0.1f) return d + 1;
        }
        return D0 + 1;
    };
    // 第一个受光列 = D0 + S*(topZ-1) + 1；topZ = 遮挡物填到的高度 hb
    const int a = litStart(3);   // 8 + 2*(3-1) + 1 = 13
    const int b = litStart(5);   // 8 + 2*(5-1) + 1 = 17
    printf("     高 3 层 -> 第一个受光列 d=%d（期望 13）；高 5 层 -> d=%d（期望 17）\n", a, b);
    CHECK(a == 13);
    CHECK(b == 17);
}

// ------------------------------------------------------------------
// 8) 平地**不会**被自己挡住（射线的起点要稍微离开表面）
//    这条防的是"射线从表面出发立刻命中自己"这类自相交 bug
// ------------------------------------------------------------------
void testNoSelfOcclusion() {
    printf("[8] 平地不会被自己挡住（自相交检查）\n");
    DzPlane p;
    p.init(16, 6);
    putFlatGround(p, 16, kWhite);   // 表面高度 1，上面全是空气

    for (float S : {0.0f, 1.0f, 3.0f}) {
        DzParams prm = baseParams(S);
        float lt[3];
        dzSampleLight(p, prm, 8, lt);
        printf("     S=%.0f d=8 收到 %.3f（应为 1）\n", S, lt[0]);
        CHECK(lt[0] > 0.99f);
    }
}

}  // namespace

int main() {
    printf("=== d-z 平面 2D 光追断言测试 ===\n");
    testEmpty();
    testFlatFullyLit();
    testShadowOffset();
    testVerticalNoOffset();
    testEmissionReachesGround();
    testShade();
    testShadowLengthGrowsWithHeight();
    testNoSelfOcclusion();


    printf("\n通过 %d，失败 %d\n", g_total - g_fail, g_fail);
    if (g_fail == 0) printf("全部通过。\n");
    return g_fail == 0 ? 0 : 1;
}
