// gfx/dz_sweep.cpp — 2.5D 光线追踪：截面法 + 柱顶辐照度
//
// ## 对照规格（第七节「实现要点」）
//
//   1. 方向集合：用引擎的 Bresenham 中点圆生成（方向表就是圆上的整数点）。
//   2. 截面法：沿 xy 走 Bresenham 直线，把 z 列收集成 2D 位图，
//      在截面上跑 2D 正向扫描线光线追踪 —— 这一步交给现有引擎 gfx::LightSweep。
//   3. 投影：取 z_max(i)+1 处的辐照度，累加到 xy 画布。
//      ★ 这一步只是**数组取值**，所以放在 CPU 上做。我先前后把它写成第二个
//        compute pass（绑三张纹理 + 一个 SSBO），结果卡在纹理绑定上转不出来；
//        而规格里它本来就是"取一格"。这一步不需要 GPU。
//   4. 多方向：每个方向生成所有平行线（引擎内部的线程枚举就是干这个的），取平均。
//
// ## 位图形状
//
//   宽 = length(d)（沿收集方向 ω 的采样长度），高 = 层数 + 1（多留一行天空）。
//   第 z 层放在行 (H-1-z)：z=0 在最底下、z=L 是最上面那行（永远空）。
//   每格如实收集 (x,y,z) 上的那个体素：颜色 C 与不透明度 A（存在 alpha 里）。
#include "dz_sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/image.h"
#include "core/layer.h"
#include "gl_texture.h"
#include "light_sweep.h"
#include "shader.h"

namespace pb::gfx {
namespace {

// 第 1 条扫描线：沿 ω 走直线，把每个采样点的 z 列收集成位图。
static const char* kBuildComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_albedo;      // 压平后的颜色 C + 不透明度 A
uniform usampler2D u_height;     // 柱高 h(x,y)（0 = 空）
uniform ivec2 u_canvasWH;
uniform ivec2 u_grid;            // (W, H)
uniform vec2 u_origin;
uniform vec2 u_step;
layout(rgba8,binding=0)writeonly uniform image2D u_pix;
void main(){
  ivec2 g=ivec2(gl_GlobalInvocationID.xy);
  if(g.x>=u_grid.x||g.y>=u_grid.y)return;

  const int d=g.x;
  const int z=u_grid.y-1-g.y;            // 行 -> 高度 z

  vec2 wp=u_origin+u_step*float(d);      // 沿 ω 走 d 步落在哪个像素
  ivec2 ip=ivec2(floor(wp));
  if(ip.x<0||ip.y<0||ip.x>=u_canvasWH.x||ip.y>=u_canvasWH.y){
    imageStore(u_pix,g,vec4(0.0));       // 出界：空体素
    return;
  }
  uint h=texelFetch(u_height,ip,0).r;    // 这一列的柱高
  if(h==0u||z>int(h)){                   // 高于柱顶：空体素
    imageStore(u_pix,g,vec4(0.0));
    return;
  }
  // 柱体内的体素：如实放它的表面颜色 C 与不透明度 A
  imageStore(u_pix,g,texelFetch(u_albedo,ip,0));
})";

}  // namespace

DzSweep::~DzSweep() { shutdown(); }

bool DzSweep::init(int w, int h, int layers) {
    shutdown();
    if (w <= 0 || h <= 0 || layers <= 0) return false;
    w_ = w; h_ = h; layers_ = layers;

    albedoTex_ = createTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    matTex_ = createTexture(GL_RGBA32F, w, h, GL_RGBA, GL_FLOAT);
    heightTex_ = createTexture(GL_R8UI, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
    outTex_ = createTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    if (!albedoTex_ || !matTex_ || !heightTex_ || !outTex_) {
        fprintf(stderr, "[dz] 纹理创建失败\n");
        return false;
    }
    buildProg_ = createComputeProgram("dz/build", kBuildComp);
    if (!buildProg_) { fprintf(stderr, "[dz] 着色器初始化失败\n"); return false; }
    return !checkGLError("dz/init");
}

void DzSweep::shutdown() {
    releaseTextures();
    if (buildProg_) glDeleteProgram(buildProg_);
    buildProg_ = 0;
    if (engine_) { engine_->shutdown(); delete engine_; engine_ = nullptr; }
    w_ = h_ = layers_ = 0;
}

void DzSweep::releaseTextures() {
    const GLuint t[4] = {albedoTex_, matTex_, heightTex_, outTex_};
    for (GLuint x : t)
        if (x) glDeleteTextures(1, &x);
    albedoTex_ = matTex_ = heightTex_ = outTex_ = 0;
    if (pixTex_) { glDeleteTextures(1, &pixTex_); pixTex_ = 0; }
    if (litTex_) { glDeleteTextures(1, &litTex_); litTex_ = 0; }
    bitmapW_ = bitmapH_ = 0;
}

void DzSweep::uploadField(const SurfaceField& f) {
    if (!valid() || f.w != w_ || f.h != h_) return;
    field_ = f;                       // 投影那一步在 CPU 上，留一份
    const size_t n = (size_t)w_ * h_;
    std::vector<float> mat(n * 4u, 0.0f);
    for (size_t i = 0; i < n; i++) {
        const Material& m = f.material[i];
        mat[i * 4u + 0] = (float)m.emR / 255.0f;
        mat[i * 4u + 1] = (float)m.emG / 255.0f;
        mat[i * 4u + 2] = (float)m.emB / 255.0f;
        mat[i * 4u + 3] = (float)m.ior + (float)(m.flags * 8);
    }
    glBindTexture(GL_TEXTURE_2D, albedoTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE,
                    f.albedo.data());
    glBindTexture(GL_TEXTURE_2D, matTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_FLOAT, mat.data());
    glBindTexture(GL_TEXTURE_2D, heightTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                    f.height.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    checkGLError("dz/uploadField");
}

bool DzSweep::render(const DzSweepParams& prm) {
    if (!valid() || !buildProg_ || field_.w != w_) return false;
    const int dirs = std::max(1, prm.directions);
    const int L = layers_;
    const int len = (int)std::ceil(std::sqrt((double)w_ * w_ + (double)h_ * h_));
    const int W = len;                 // 截面宽度 = length(d)
    const int Hb = L + 1;              // 层数 + 1（留一行天空）

    if (!pixTex_ || bitmapW_ != W || bitmapH_ != Hb) {
        if (pixTex_) glDeleteTextures(1, &pixTex_);
        if (litTex_) glDeleteTextures(1, &litTex_);
        pixTex_ = createTexture(GL_RGBA8, W, Hb, GL_RGBA, GL_UNSIGNED_BYTE);
        litTex_ = createTexture(GL_RGBA8, W, Hb, GL_RGBA, GL_UNSIGNED_BYTE);
        bitmapW_ = W; bitmapH_ = Hb;
        if (!pixTex_ || !litTex_) { fprintf(stderr, "[dz] 位图创建失败\n"); return false; }
    }

    // 2D 光追引擎：截面就是它的画布
    if (!engine_) engine_ = new LightSweep();
    if (!engine_->init(W, Hb)) return false;
    engine_->setLayerCount(1);
    engine_->settings().rayRadius = std::max(4, m_);   // 采样半径 m
    engine_->settings().ambient[0] = prm.sky[0];
    engine_->settings().ambient[1] = prm.sky[1];
    engine_->settings().ambient[2] = prm.sky[2];
    engine_->settings().emitScale = prm.emitScale;

    std::vector<float> irrad((size_t)w_ * (size_t)h_ * 3u, 0.0f);
    std::vector<RGBA8> bmp((size_t)W * (size_t)Hb);
    std::vector<float> litscan_((size_t)W * (size_t)Hb * 4u, 0.0f);
    std::vector<Material> mats((size_t)W * (size_t)Hb);

    for (int k = 0; k < dirs; k++) {
        const float ang = (float)(2.0 * 3.14159265358979 * (double)k / (double)dirs);
        const float ux = std::cos(ang), uy = std::sin(ang);
        const float cx = (float)w_ * 0.5f, cy = (float)h_ * 0.5f;
        const float r0 = 0.5f * (float)len;
        const float ox = cx - ux * r0, oy = cy - uy * r0;

        // ---- 第 1 条扫描线：截面位图 ----
        glUseProgram(buildProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, albedoTex_);
        glUniform1i(glGetUniformLocation(buildProg_, "u_albedo"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, heightTex_);
        glUniform1i(glGetUniformLocation(buildProg_, "u_height"), 1);
        glUniform2i(glGetUniformLocation(buildProg_, "u_canvasWH"), w_, h_);
        glUniform2i(glGetUniformLocation(buildProg_, "u_grid"), W, Hb);
        glUniform2f(glGetUniformLocation(buildProg_, "u_origin"), ox, oy);
        glUniform2f(glGetUniformLocation(buildProg_, "u_step"), ux, uy);
        glBindImageTexture(0, pixTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute((GLuint)((W + 7) / 8), (GLuint)((Hb + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);

        glBindTexture(GL_TEXTURE_2D, pixTex_);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        // ---- 第 2 条扫描线：引擎在截面上跑 2D 正向扫描线光追 ----
        std::fill(mats.begin(), mats.end(), Material{});
        Image img(W, Hb);
        std::memcpy(img.data(), bmp.data(), bmp.size() * sizeof(RGBA8));
        engine_->uploadAll(img, mats.data());
        engine_->render();

        // ⚠️ 要的是**纯辐照度**，即引擎的 scan 图（平面内扫描收集到的光）。
        // resultTexture() 是已经上过色的合成图（见 kBlendComp：
        //     r = c.rgb + e + c.rgb*s*1.2
        // 表面色 + 自发光 + 光），拿它再按规格 mix(I+E, C, A) 会把表面色算两次。
        // scanTexture 是 RGBA16F，用 float 读回来。
        glBindTexture(GL_TEXTURE_2D, engine_->scanTexture());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, litscan_.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        if (getenv("PB_DZ_DEBUG") && k == 0) {
            float mx = 0.0f, sum = 0.0f;
            for (size_t i = 0; i < (size_t)W * Hb; i++) {
                const float v = litscan_[i * 4u];
                sum += v;
                if (v > mx) mx = v;
            }
            fprintf(stderr, "[dz] scan(辐照度)：最大 %.3f 平均 %.4f  位图 %dx%d\n", mx,
                    sum / (float)(W * Hb), W, Hb);
        }

        // ---- 3. 投影：取 z_max(d)+1 处的辐照度，累加到 xy 画布 ----
        for (int d = 0; d < W; d++) {
            const int px = (int)std::floor(ox + ux * (float)d);
            const int py = (int)std::floor(oy + uy * (float)d);
            if (px < 0 || py < 0 || px >= w_ || py >= h_) continue;

            int zmax = -1;                 // 从下往上找最高的实体格
            for (int z = 0; z < Hb; z++)
                if (bmp[(size_t)(Hb - 1 - z) * (size_t)W + (size_t)d].a > 0) zmax = z;
            if (zmax < 0) continue;        // 这一列没有实体

            const int zlit = zmax + 1;     // 「最高点加一」
            if (zlit > Hb - 1) continue;
            const size_t si = ((size_t)(Hb - 1 - zlit) * (size_t)W + (size_t)d) * 4u;
            const size_t bi = ((size_t)py * (size_t)w_ + (size_t)px) * 3u;
            irrad[bi + 0] += litscan_[si + 0];
            irrad[bi + 1] += litscan_[si + 1];
            irrad[bi + 2] += litscan_[si + 2];
        }
    }

    // ---- 4. 最终颜色：Color = mix(I + E_top, C_top, A_top) ----
    std::vector<RGBA8> out((size_t)w_ * (size_t)h_, RGBA8{0, 0, 0, 255});
    const float invDirs = 1.0f / (float)dirs;
    for (int y = 0; y < h_; y++) {
        for (int x = 0; x < w_; x++) {
            const size_t i = (size_t)y * (size_t)w_ + (size_t)x;
            if (field_.height[i] == 0) continue;      // 空柱
            const size_t bi = i * 3u;
            const float I[3] = {irrad[bi + 0] * invDirs, irrad[bi + 1] * invDirs,
                                irrad[bi + 2] * invDirs};
            const Material& m = field_.material[i];
            const float E[3] = {(float)m.emR / 255.0f * prm.emitScale,
                                (float)m.emG / 255.0f * prm.emitScale,
                                (float)m.emB / 255.0f * prm.emitScale};
            const RGBA8 C = field_.albedo[i];
            const float A = (float)C.a / 255.0f;
            const float Cs[3] = {(float)C.r / 255.0f, (float)C.g / 255.0f,
                                 (float)C.b / 255.0f};
            uint8_t rgb[3];
            for (int c = 0; c < 3; c++) {
                const float a = I[c] + E[c];
                const float v = a * (1.0f - A) + Cs[c] * A;   // mix(I+E, C, A)
                rgb[c] = (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            }
            out[i] = RGBA8{rgb[0], rgb[1], rgb[2], 255};
            if (getenv("PB_DZ_DEBUG") && x == 4 && y == 4) {
                fprintf(stderr,
                        "[dz] 像素(4,4): h=%d I=%.3f,%.3f,%.3f C=%d,%d,%d A=%.3f -> %d,%d,%d\n",
                        (int)field_.height[i], I[0], I[1], I[2], (int)C.r, (int)C.g, (int)C.b, A,
                        (int)rgb[0], (int)rgb[1], (int)rgb[2]);
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, outTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    return !checkGLError("dz/render");
}

}  // namespace pb::gfx
