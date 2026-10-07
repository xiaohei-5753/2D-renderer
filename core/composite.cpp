// core/composite.cpp — 图层合成与层间光照通道的实现
//
// 设计说明与"为什么需要 occlAbove / emitFor"写在 core/composite.h 里。
#include "composite.h"

#include <algorithm>
#include <cmath>

namespace pb {

void flattenDocument(const Document& doc, Image& outAlbedo,
                     std::vector<Material>& outMaterial, const Rect& regionIn) {
    if (doc.empty() || outAlbedo.width() != doc.width() ||
        outAlbedo.height() != doc.height() ||
        outMaterial.size() != (size_t)doc.width() * (size_t)doc.height()) {
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

void buildOcclAbove(const Document& doc, int layerIndex, const Rect& regionIn,
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

void buildEmitFor(const Document& doc, int layerIndex, const Rect& regionIn,
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
