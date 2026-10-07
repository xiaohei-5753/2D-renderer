// core/composite.h — 图层合成，以及"层间光照"需要的辅助通道
//
// 这一层是纯 CPU、可 headless 验证的，而且**两个光照方案都要它**：
//   * 显示：无论如何都需要把图层按 alpha 压平成一张图
//   * 模型 B：需要下面两张逐层通道（occlAbove / emitFor）
//   * 模型 A：压平的结果就是"相机沿 -Z 看到的第一个不透明体素"，同样要用
//
// ## 为什么需要 occlAbove / emitFor（回顾一下难点）
//
// 光照在画布平面(XY)内传播，图层沿视线方向(Z)堆叠，
// 而**无限薄的层不会挡住另一层平面内的光** —— 所以"上层给下层投影"不会自然发生。
// 模型 B 显式补上这个垂直方向的关系：
//
//   occlAbove[i](x,y) = 1 - Π(k>i) (1 - cov_k(x,y))
//       第 i 层**上方**所有层的累计遮盖。用它给该层的平面内光线额外衰减 = 投影。
//
//   emitFor_i(x,y)    = Σ(k≠i) em_k(x,y) · Π(中间层的 (1-cov))
//       其他层的光透过**中间层**到达第 i 层的量。把它当作第 i 层的额外光源 = 层间照亮。
//       这里的"中间层"是 i 与 k 之间真正夹着的那些层，所以下层的光能照上来、
//       上层的光也能照下去，且都会被子透明层染色/衰减。
//
// 这是**风格化**的模型（不是严格物理），但良定义、可预期，且保留了平面观感。
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "image.h"
#include "layer.h"
#include "material.h"
#include "types.h"

namespace pb {

// 把图层从下往上按 alpha 压平。
//
// 材质合并规则：**最上层有覆盖的像素，其材质胜出**（简单、可预期）。
// outAlbedo / outMaterial 必须已经是 doc 的尺寸；只重算 region 指定的区域。
inline void flattenDocument(const Document& doc, Image& outAlbedo,
                           std::vector<Material>& outMaterial, const Rect& regionIn) {
    if (doc.empty() || outAlbedo.width() != doc.width() ||
        outAlbedo.height() != doc.height()) {
        return;
    }
    Rect r = regionIn;
    r.clipTo(doc.width(), doc.height());
    if (r.empty()) return;

    for (int y = r.y0; y <= r.y1; y++) {
        for (int x = r.x0; x <= r.x1; x++) {
            const size_t k = (size_t)y * (size_t)doc.width() + (size_t)x;
            float accR = 0.0f, accG = 0.0f, accB = 0.0f, accA = 0.0f;
            Material mat{};          // 默认无材质
            bool matSet = false;

            // 从最底层往上叠
            for (int i = 0; i < doc.layerCount(); i++) {
                const Layer& L = doc.layer(i);
                if (!L.visible || L.opacity == 0) continue;
                if ((int)L.pixelCount() != doc.width() * doc.height()) continue;

                const RGBA8 s = L.albedo.data()[k];
                const float sa = (s.a / 255.0f) * (L.opacity / 255.0f);
                if (sa <= 0.0f) continue;

                // 非预乘的 source-over
                const float oa = sa + accA * (1.0f - sa);
                if (oa > 0.0f) {
                    accR = (s.r / 255.0f * sa + accR * accA * (1.0f - sa)) / oa;
                    accG = (s.g / 255.0f * sa + accG * accA * (1.0f - sa)) / oa;
                    accB = (s.b / 255.0f * sa + accB * accA * (1.0f - sa)) / oa;
                }
                accA = oa;
                // 最上层有覆盖的像素，材质胜出
                mat = L.material[k];
                matSet = true;
            }

            auto to255 = [](float v) {
                return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255);
            };
            outAlbedo.data()[k] = RGBA8{to255(accR), to255(accG), to255(accB), to255(accA)};
            outMaterial[k] = matSet ? mat : Material{};
        }
    }
}

// 第 i 层上方的累计遮盖（0 = 完全通透，255 = 完全被盖住）
inline void buildOcclAbove(const Document& doc, int layerIndex, const Rect& regionIn,
                          std::vector<uint8_t>& out) {
    if (doc.empty()) return;
    Rect r = regionIn;
    r.clipTo(doc.width(), doc.height());
    if (r.empty()) return;
    out.resize((size_t)doc.width() * (size_t)doc.height());

    // 只有 region 里的像素会被写；调用方按需读取
    for (int y = r.y0; y <= r.y1; y++) {
        for (int x = r.x0; x <= r.x1; x++) {
            const size_t k = (size_t)y * (size_t)doc.width() + (size_t)x;
            float trans = 1.0f;   // 透过比例
            for (int i = layerIndex + 1; i < doc.layerCount(); i++) {
                const Layer& L = doc.layer(i);
                if (!L.visible || L.opacity == 0) continue;
                if ((int)L.pixelCount() != doc.width() * doc.height()) continue;
                const float cov = (L.albedo.data()[k].a / 255.0f) * (L.opacity / 255.0f);
                trans *= (1.0f - cov);
            }
            out[k] = (uint8_t)std::clamp((int)std::lround((1.0f - trans) * 255.0f), 0, 255);
        }
    }
}

// 其他层的光透过中间层到达第 i 层的量（逐通道 RGB）
inline void buildEmitFor(const Document& doc, int layerIndex, const Rect& regionIn,
                         std::vector<RGBA8>& out) {
    if (doc.empty()) return;
    Rect r = regionIn;
    r.clipTo(doc.width(), doc.height());
    if (r.empty()) return;
    out.resize((size_t)doc.width() * (size_t)doc.height());

    for (int y = r.y0; y <= r.y1; y++) {
        for (int x = r.x0; x <= r.x1; x++) {
            const size_t k = (size_t)y * (size_t)doc.width() + (size_t)x;
            float sumR = 0.0f, sumG = 0.0f, sumB = 0.0f;

            for (int k2 = 0; k2 < doc.layerCount(); k2++) {
                if (k2 == layerIndex) continue;
                const Layer& S = doc.layer(k2);
                if (!S.visible || S.opacity == 0) continue;
                if ((int)S.pixelCount() != doc.width() * doc.height()) continue;
                const Material& m = S.material[k];
                if (!m.emissive()) continue;

                // 累乘"夹在两者之间"的层的透过率
                const int lo = std::min(layerIndex, k2) + 1;
                const int hi = std::max(layerIndex, k2) - 1;
                float trans = 1.0f;
                for (int t = lo; t <= hi; t++) {
                    const Layer& M = doc.layer(t);
                    if (!M.visible || M.opacity == 0) continue;
                    if ((int)M.pixelCount() != doc.width() * doc.height()) continue;
                    const float cov =
                        (M.albedo.data()[k].a / 255.0f) * (M.opacity / 255.0f);
                    trans *= (1.0f - cov);
                }
                // 光源自身的整层不透明度也要计入
                trans *= (S.opacity / 255.0f);

                sumR += (m.emR / 255.0f) * trans;
                sumG += (m.emG / 255.0f) * trans;
                sumB += (m.emB / 255.0f) * trans;
            }

            auto to255 = [](float v) {
                return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255);
            };
            out[k] = RGBA8{to255(sumR), to255(sumG), to255(sumB), 255};
        }
    }
}

}  // namespace pb
