// app/panels_sections_lighting.cpp — 右栏「光照」分区
//
// 只剩一种照明模式：**扫描 GI**（方向表扫描，实时、无噪声）。
// 参数分三块：
//   采样方式 —— 渐进哈希采样 / 去噪
//   公共     —— 环境光 / 自发光倍率
//   扫描 GI  —— 方向表半径 / 画质档位 / 方向扩散 / 沿程增益 / 光场照亮下层
//
// 路径追踪（spp / 弹射 / 曝光 / 色调映射 / 收敛进度）已于 2026-10-19 整体移除。
//
// 参数改动必须走 actLightSettingsChanged()：它会标记光照需要重算
// （渐进模式下也就此重开累积，否则新旧参数会混叠）。
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/panels_internal.h"
#include "gfx/light_sweep.h"
#include "ui/widgets.h"

namespace pb::app {

void drawLightingSection(App& a) {
    // ------------------------------------------------------------------
    // 1. 照明模式（只剩扫描 GI，不再有可点的模式按钮）
    // ------------------------------------------------------------------
    ImGui::TextDisabled("照明模式：扫描 GI（路径追踪已移除）");

    ImGui::Spacing();
    ImGui::Checkbox("开启光照", &a.lightOn);
    if (ImGui::IsItemEdited()) a.lightDirty = true;
    ImGui::SameLine();
    if (ImGui::Button("重置累积")) actLightSettingsChanged(a);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("清空时间累积，重新从第 1 个样本开始收敛");

    // ------------------------------------------------------------------
    // 1b. 渐进哈希采样（扫描 GI 的可选路径）
    // ------------------------------------------------------------------
    ImGui::SeparatorText("采样方式");
    if (ImGui::Checkbox("渐进哈希采样（每帧每像素一个方向）", &a.progressive))
        actLightSettingsChanged(a);
    ImGui::TextDisabled("零原子、单帧只要 ~0.1–0.35×；第 D 帧与确定性扫描精确相等");
    if (a.progressive) {
        if (ImGui::Checkbox("去噪（à-trous 联合双边）", &a.denoise))
            actLightSettingsChanged(a);
        if (a.denoise) {
            ImGui::SetNextItemWidth(-1);
            if (ui::labeledSliderFloat("##dnstr", &a.denoiseStrength, 0.5f, 8.0f,
                                       "去噪强度")) {
                actLightSettingsChanged(a);
            }
            int it = a.denoiseIters;
            if (ui::labeledSliderInt("##dnit", &it, 1, 5, "迭代层数")) {
                a.denoiseIters = it;
                actLightSettingsChanged(a);
            }
        }
        if (a.sweep) {
            const int n = a.sweep->progressiveSamples();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d 帧样本", n);
            ImGui::ProgressBar((float)n / (float)std::max(1, a.sweep->directionCount()),
                               ImVec2(-1.0f, a.ui(12.0f)), buf);
            ImGui::TextDisabled("到第 D 帧（= %d）时与确定性扫描完全一致",
                                a.sweep->directionCount());
        }
    }

    // ------------------------------------------------------------------
    // 2. 公共参数
    // ------------------------------------------------------------------
    ImGui::SeparatorText("公共");
    {
        float amb[3] = {0.0f, 0.0f, 0.0f};
        if (a.sweep) {
            amb[0] = a.sweep->settings().ambient[0];
            amb[1] = a.sweep->settings().ambient[1];
            amb[2] = a.sweep->settings().ambient[2];
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::ColorEdit3("##ambient", amb, ImGuiColorEditFlags_InputRGB)) {
            if (a.sweep) {
                for (int i = 0; i < 3; i++) a.sweep->settings().ambient[i] = amb[i];
            }
            actLightSettingsChanged(a);
        }
        ImGui::TextDisabled("环境光（逃出画布的射线得到的亮度）");

        float em = a.sweep ? a.sweep->settings().emitScale : 1.0f;
        ImGui::SetNextItemWidth(-1);
        if (ui::labeledSliderFloat("##emit", &em, 0.0f, 4.0f, "自发光倍率")) {
            if (a.sweep) a.sweep->settings().emitScale = em;
            actLightSettingsChanged(a);
        }
    }

    // ------------------------------------------------------------------
    // 3. 扫描 GI 专属参数
    // ------------------------------------------------------------------
    if (a.sweep) {
        ImGui::SeparatorText("扫描 GI");
        gfx::LightSettings& s = a.sweep->settings();
        int r = s.rayRadius;
        if (ui::labeledSliderInt("##radius", &r, 4, 512, "方向表半径")) {
            s.rayRadius = r;
            actLightSettingsChanged(a);
        }
        // 光场降采样：光照低频 -> 只在 1/s 分辨率上算光场，合成时双线性放大。
        // albedo / 自发光保持全分辨率，所以 1px 霓虹不会被糊掉（实测最大差 7 级、
        // 平均差 <0.1 级；5 层 R=64 的耗时 4.08 ms -> 0.53 ms）。
        {
            static const char* kFieldLabels[4] = {"光场降采样：关（1×）", "光场降采样：2×（约 2.5×）",
                                                  "光场降采样：3×", "光场降采样：4×（约 7.7×）"};
            int fs = std::clamp(a.fieldScale, 1, 4);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##fieldscale", &fs, kFieldLabels, 4)) {
                a.fieldScale = fs;
                actLightSettingsChanged(a);
            }
        }
        if (ImGui::Checkbox("分块合并扫描（实验：x 主方向走 shared 合并）", &a.tiledScan))
            actLightSettingsChanged(a);
        ImGui::TextDisabled("当前 %d 个方向（半径越大越平滑、越慢）",
                            a.sweep->directionCount());

        // 每像素原子加次数：占整帧耗时的 83%（见 BENCHMARK.md），这里给个开关。
        // 2 次与 3 次的结果差 ≤2 级（不可见）；1 次是 10/10/10，亮部会有色带。
        {
            static const char* kPackLabels[3] = {"3 次（默认，最精确）", "2 次（R|G 打包，快 1.5×）",
                                                 "1 次（10/10/10，快 2.6×，有色带）"};
            int pk = a.sweep->atomicPack();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##pack", &pk, kPackLabels, 3)) {
                a.sweep->setAtomicPack(pk);
                actLightSettingsChanged(a);
            }
            ImGui::TextDisabled("写入打包：占整帧 83%%，2 次档肉眼无差");
        }

        // ------------------------------------------------------------------
        // 画质档位：一次把"半径 + 扩散"配对设好
        // ------------------------------------------------------------------
        //
        // 为什么做成档位而不是两个独立滑块：半径和扩散是**互相补偿**的 ——
        // 半径小（快、有辐条）就该用大一点的扩散来补，半径大（慢、本来就干净）
        // 就不需要扩散。分开调的话用户很难知道"这对组合"该取什么，
        // 而且很容易调到"半径也小、扩散也关"这种又慢又脏的角落。
        //
        // 每个档位的数都来自实测（tools/dir_probe 对 R=128 的误差）。
        ImGui::SeparatorText("画质档位");
        {
            gfx::LightSettings& ls = a.sweep->settings();
            struct Preset { const char* label; const char* tip; int radius; bool diff; float w; int k; };
            static const Preset kPresets[] = {
                {"关", "只调半径，不做方向扩散。细光源周围会有放射状辐条。", 32, false, 0.0f, 8},
                {"轻", "w=0.02 / K=4：温和地抹平角度空隙，对细线几乎无损。", 32, true, 0.02f, 4},
                {"均衡", "w=0.1 / K=8：实测甜点。光点/网格误差降 24~52%，细线只退化 3%。", 32, true, 0.10f, 8},
                {"强", "w=0.25 / K=2：辐条基本消失，代价是细线会变糊。",
                 32, true, 0.25f, 2},
            };
            const int nP = (int)(sizeof(kPresets) / sizeof(kPresets[0]));
            int cur = -1;
            for (int i = 0; i < nP; i++) {
                const Preset& p = kPresets[i];
                if (ls.diffusionOn != p.diff) continue;
                if (!p.diff) { cur = i; break; }
                if (std::fabs(ls.diffusion - p.w) < 1e-4f && ls.diffusionStep == p.k) { cur = i; break; }
            }
            ImGui::BeginGroup();
            for (int i = 0; i < nP; i++) {
                if (i) ImGui::SameLine();
                const bool sel = (cur == i);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.78f, 1.0f));
                if (ImGui::Button(kPresets[i].label, ImVec2(a.ui(52.0f), a.ui(24.0f)))) {
                    ls.diffusionOn = kPresets[i].diff;
                    ls.diffusion = kPresets[i].w;
                    ls.diffusionStep = kPresets[i].k;
                    actLightSettingsChanged(a);
                }
                if (sel) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPresets[i].tip);
            }
            ImGui::EndGroup();
            ImGui::TextDisabled("档位只改扩散；半径在下面单独调");
        }

        // ------------------------------------------------------------------
        // 方向扩散的细调（= 上面"均衡/强"档背后的两个参数）
        // ------------------------------------------------------------------
        //
        // 做成折叠的：折叠时只占一行，不会把"沿程增益 / 光场照亮下层"挤出
        // 可视区 —— 这个面板已经很长了。
        if (ImGui::TreeNode("方向扩散 细调")) {
            gfx::LightSettings& ls = a.sweep->settings();
            bool on = ls.diffusionOn;
            if (ImGui::Checkbox("启用方向扩散", &on)) {
                ls.diffusionOn = on;
                // 刚打开时给一组实测好用的默认值，免得用户面对 0 不知所措
                if (on && ls.diffusion <= 0.0f) {
                    ls.diffusion = 0.1f;
                    ls.diffusionStep = 8;
                }
                actLightSettingsChanged(a);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("相邻射线互相交换一部分携带的光。\n"
                                  "等价于在角度维做扩散，能显著削弱\n"
                                  "离散方向造成的放射状辐条。\n"
                                  "权重和恒为 1，所以总亮度不变。");

            float dw = ls.diffusion;
            if (ui::labeledSliderFloat("##diffw", &dw, 0.005f, 0.5f, "交换比例 w", "%.3f")) {
                ls.diffusion = dw;
                actLightSettingsChanged(a);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("每次交换把邻居的光混进来多少。\n"
                                  "越大辐条消得越干净，但**细线光源会变糊**：\n"
                                  "实测 w>0.05 且 K 很小时线的锐利度明显下降。\n"
                                  "建议 0.05~0.15。");

            int dk = ls.diffusionStep;
            if (ui::labeledSliderInt("##diffk", &dk, 1, 64, "每 K 步交换一次")) {
                ls.diffusionStep = dk;
                actLightSettingsChanged(a);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("交换的频率。K 越小交换越频繁、辐条越干净，\n"
                                  "但耗时越高（K=1 约为基线的 1.3~2 倍）。\n"
                                  "实测 K=8 是质量与性能的甜点。");
            ImGui::TextDisabled("实测：点 −36%% / 网格 −52%% / 方块 −24%% / 细线 +3%%");
            ImGui::TreePop();
        }

        ImGui::SetNextItemWidth(-1);
        if (ui::labeledSliderFloat("##cp", &s.propagate, 0.5f, 2.0f, "沿程增益")) {
            actLightSettingsChanged(a);
        }
        // 上层光场照亮下层：每层独立扫出一个"光场"，再按"彩色光打在彩色表面"
        // 的方式乘到下面各层上。纯后期，不做阴影。
        //
        // 下界刻意**不取 0**：0 是合成着色器里"关掉光场模型"的哨兵值（那条分支是
        // 旧的加法混合），而光场模型现在就是默认且唯一的路径。真要极暗的效果，
        // 0.05 已经足够；把它做成 0 只会让面板能一键退回一条已废弃的路径。
        float fs = a.fieldStrength;
        if (ui::labeledSliderFloat("##fs", &fs, 0.05f, 3.0f, "光场照亮下层", "%.2f")) {
            a.fieldStrength = fs;
            if (a.sweep) a.sweep->setFieldStrength(fs);
            actLightSettingsChanged(a);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("每层先独立扫出自己的光场，\n"
                              "再乘到下面各层的颜色上照亮它们。\n"
                              "所以只有环境光时白纸是暗的 —— 这是期望行为。");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("光照 %.1f ms | 光照图层 %s", a.lightMs, a.lightOn ? "开" : "关");
}

}  // namespace pb::app
