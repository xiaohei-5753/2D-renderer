// core/composite.cpp — 图层压平的实现
//
// 设计说明见 core/composite.h（含"为什么层间光照通道已被删除"）。
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

}  // namespace pb
