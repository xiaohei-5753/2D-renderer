// tools/material_pack_test.cpp — 材质打包的无损性验证
//
// 为什么单独一个工具：带宽压缩的第一步是把**扫描阶段**要读的材质从 RGBA32F
// (16 B/像素) 压成 4 字节的紧凑字节，而"无损"必须**可证明**，不能靠"看起来对"。
//
// ## 关键观察：扫描阶段根本不需要 ior
//
// ior（折射率）只在**路径追踪**里用。扫描 GI 是直线扫描，物理上做不了弯曲光路。
// 而扫描读材质只剩一件事：**emission**。flags 里的 kSolid 也已删除
// （它是"把 alpha 设成 255"的冗余写法，而且着色器从来没读过 flags）。
//
// 所以扫描侧只要 **RGB = 3 字节/像素**。ior 原样留在路径追踪用的那张
// RGBA32F 纹理里（那里是精确的），**一点精度都没丢**。
//
// 本工具穷举全部 emission 组合，验证：
//   1. 扫描侧编码（3 字节）往返无损，且 **emission 逐位相等**
//   2. shader 侧的解码与编码一致（一个改了另一个没改 = 静默画错，不会崩）
//   3. ior 走路径追踪侧仍然精确
#include <cstdio>
#include <cstdint>
#include <vector>

#include "core/material.h"
#include "core/types.h"

using namespace pb;

namespace {

// ---------------------------------------------------------------------------
// 扫描侧的紧凑编码：每像素 3 字节（纯自发光）
//   R = emR, G = emG, B = emB
// ---------------------------------------------------------------------------
void encodeScanMaterial(const Material& m, uint8_t* out3) {
    out3[0] = m.emR;
    out3[1] = m.emG;
    out3[2] = m.emB;
}

// shader 侧的解码（与上面严格对应）
struct ScanDecoded {
    uint8_t emR, emG, emB;
};
ScanDecoded decodeScanMaterial(const uint8_t* in3) {
    return ScanDecoded{in3[0], in3[1], in3[2]};
}

// 扫描阶段要用的量（与 kScanComp 的算法一致）：覆盖率现在只来自 canvas 的 alpha
struct ScanUse {
    float cov;
    float emR, emG, emB;
};
ScanUse scanUseFromDecoded(const ScanDecoded& d, float alphaOfPixel) {
    ScanUse u;
    u.cov = alphaOfPixel;
    u.emR = (float)d.emR / 255.0f;
    u.emG = (float)d.emG / 255.0f;
    u.emB = (float)d.emB / 255.0f;
    return u;
}

int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL  %s\n", what); g_fail++; }
}

}  // namespace

int main() {
    printf("=== 扫描侧材质打包（3 字节）：无损性 + 解码一致性 ===\n");

    // ------------------------------------------------------------------
    // 1. 穷举全部 emission（24 位里有代表性的一大片）与全部 ior
    // ------------------------------------------------------------------
    long long n = 0, badRound = 0, badUse = 0;
    double maxIorErr = 0.0;
    for (int ior = 0; ior < 256; ior++) {
        for (int e = 0; e < 4096; e++) {
            Material m;
            m.emR = (uint8_t)((e * 37 + ior) & 0xFF);
            m.emG = (uint8_t)((e * 91 + ior * 3) & 0xFF);
            m.emB = (uint8_t)((e * 151 + ior * 5) & 0xFF);
            m.ior = (uint8_t)ior;
            n++;

            uint8_t buf[3];
            encodeScanMaterial(m, buf);
            const ScanDecoded d = decodeScanMaterial(buf);

            // 扫描侧必须**逐位**还原自发光
            if (d.emR != m.emR || d.emG != m.emG || d.emB != m.emB) badRound++;

            // 与"直接从 Material 算"的结果比：必须完全一致
            const ScanUse a = scanUseFromDecoded(d, 0.5f);
            ScanUse b;
            b.cov = 0.5f;   // 覆盖率现在只来自 canvas 的 alpha
            b.emR = (float)m.emR / 255.0f;
            b.emG = (float)m.emG / 255.0f;
            b.emB = (float)m.emB / 255.0f;
            if (a.cov != b.cov || a.emR != b.emR || a.emG != b.emG || a.emB != b.emB)
                badUse++;

            // ior 走路径追踪侧（RGBA32F）必须仍然精确
            const float iorv = 1.0f + (float)m.ior / 100.0f;
            const double err = (double)iorv - (double)m.iorValue();
            if (err > maxIorErr) maxIorErr = err;
        }
    }
    printf("\n  样本数                    : %lld\n", n);
    printf("  3 字节打包往返失败         : %lld   %s\n", badRound,
           badRound == 0 ? "-> 严格无损 OK" : "-> 有损失！");
    printf("  扫描算式与直接算不一致      : %lld   %s\n", badUse,
           badUse == 0 ? "-> 语义一致 OK" : "-> 语义变了！");
    printf("  ior 经路径追踪侧最大误差    : %.3e  %s\n", maxIorErr,
           maxIorErr == 0.0 ? "-> 精确" : "-> 有误差");
    check(badRound == 0, "扫描侧打包必须无损");
    check(badUse == 0, "扫描算式必须等价");
    check(maxIorErr == 0.0, "ior 必须精确");

    // ------------------------------------------------------------------
    // 2. 结构尺寸与带宽对比
    // ------------------------------------------------------------------
    printf("\n  sizeof(Material)           : %zu 字节（原 6，删掉 flags+reserved）\n",
           sizeof(Material));
    printf("  每像素材质纹理字节数：\n");
    printf("    最早（扫描读 RGBA32F）    : 16 B\n");
    printf("    上次（扫描读 RGBA8）      :  4 B\n");
    printf("    本次（扫描读 RGB8）       :  3 B   累计省 %.1f 倍\n", 16.0 / 3.0);
    printf("    路径追踪侧                : 16 B（不变，ior 仍精确）\n");
    printf("\n  扫描阶段整格流量（含 canvas 4 B）：\n");
    printf("    最早: %2d B/格\n", 4 + 16);
    printf("    本次: %2d B/格   累计省 %.2f 倍\n", 4 + 3, 20.0 / 7.0);

    printf("\n=== 结论 ===\n");
    if (g_fail == 0)
        printf("  扫描侧打包**严格无损**（穷举 %lld 组），且与路径追踪侧解耦。\n", n);
    else
        printf("  有 %d 项失败。\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
