// render/voxel_gpu.cpp — Radiance Cascades 的 GPU 实现（OpenGL 4.3 compute）
//
// ## 为什么 RC 特别适合 GPU
//
//   每个 cascade 的 探测点 × 射线 互相独立，没有依赖；
//   合并阶段只是逐层查表插值。所以整条管线就是几次 dispatch。
//
// ## 管线
//
//   1. 体素 → 3D 纹理：rgb = 自发光 E，a = 不透明度 A
//   2. 每层 cascade 一次 dispatch：
//        线程数 = 探测点数 × 该层射线数（160x96 时每层约 1.8M）
//        每个线程走一条体素 DDA，原子累加进这一层的 SSBO
//        （定点 u32，值域 0..65535 —— 和现有 light_sweep 同款做法）
//   3. 每层再一次小 dispatch：把 SSBO 落成一张 float 图集（cascadeTex）的一个横带
//   4. 合并：每像素逐层做**手写双线性**（不用硬件采样，避免跨层渗色）
//
// 结果留在显存里，GUI 直接用 resultTexture()，省掉读回+上传。
#include "render/voxel_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gfx/gl_texture.h"
#include "gfx/shader.h"

namespace pb::render {
namespace {

constexpr int kMaxCascades = 12;

// ---- 1. 走射线，把这一层负责区间内的辐射原子累加 ----
// 线程 = (探测点, 射线)
static const char* kTraceComp = R"(#version 430 core
layout(local_size_x=64)in;
uniform sampler3D u_vol;          // rgb = 自发光, a = 不透明度
uniform usampler2D u_top;         // R8UI: 每列柱高（0 = 空柱）
uniform ivec2 u_canvas;
uniform ivec3 u_volSize;
uniform int u_probeSpacing;
uniform ivec2 u_probeGrid;        // (gw, gh)
uniform int u_dirs;
uniform float u_r0, u_r1;
uniform int u_limit;
layout(std430,binding=0)buffer Acc{ uint accbuf[]; };   // 名字不能叫 acc：
                                                                  // 下面有个局部 vec3 acc，
                                                                  // 同名会把数组遮住 -> acc[bi] 变成给 vec3 下标
void main(){
  uint gid = gl_GlobalInvocationID.x;
  int probe = int(gid) / u_dirs;
  int ray   = int(gid) % u_dirs;
  int gw = u_probeGrid.x, gh = u_probeGrid.y;
  if (probe >= gw * gh) return;
  int gx = probe % gw, gy = probe / gw;
  int px = gx * u_probeSpacing, py = gy * u_probeSpacing;
  if (px >= u_canvas.x || py >= u_canvas.y) return;
  uint h = texelFetch(u_top, ivec2(px, py), 0).r;
  if (h == 0u) return;
  vec3 p = vec3(float(px) + 0.5, float(py) + 0.5, float(h));   // 柱顶正上方

  // 上半球斐波那契方向
  float cz = 1.0 - (float(ray) + 0.5) / float(u_dirs);
  float sz = sqrt(max(0.0, 1.0 - cz * cz));
  float phi = float(ray) * 2.39996323;
  vec3 d = vec3(sz * cos(phi), sz * sin(phi), cz);

  // 体素 DDA
  ivec3 c = ivec3(floor(p));
  ivec3 st = ivec3(d.x > 0.0 ? 1 : (d.x < 0.0 ? -1 : 0),
                   d.y > 0.0 ? 1 : (d.y < 0.0 ? -1 : 0),
                   d.z > 0.0 ? 1 : (d.z < 0.0 ? -1 : 0));
  const float INF = 1e30;
  vec3 td = vec3(d.x != 0.0 ? abs(1.0 / d.x) : INF,
                 d.y != 0.0 ? abs(1.0 / d.y) : INF,
                 d.z != 0.0 ? abs(1.0 / d.z) : INF);
  vec3 tm;
  for (int i = 0; i < 3; i++) {
    float dd = d[i];
    int s = st[i];
    tm[i] = (dd == 0.0 || s == 0) ? INF
            : (((s > 0) ? float(c[i] + 1) : float(c[i])) - p[i]) / dd;
  }
  vec3 acc = vec3(0.0);
  float T = 1.0;
  for (int it = 0; it < u_limit; it++) {
    float tHit;
    if (tm.x < tm.y && tm.x < tm.z) { tHit = tm.x; c.x += st.x; tm.x += td.x; }
    else if (tm.y < tm.z)          { tHit = tm.y; c.y += st.y; tm.y += td.y; }
    else                           { tHit = tm.z; c.z += st.z; tm.z += td.z; }
    if (tHit > u_r1) break;
    if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= u_canvas.x || c.y >= u_canvas.y ||
        c.z >= u_volSize.z)
      break;
    vec4 v = texelFetch(u_vol, c, 0);
    float A = v.a;
    if (A > 0.0) {
      if (tHit >= u_r0) acc += T * v.rgb * A;      // 只累加本层负责的区间
      T *= (1.0 - A);
      if (T < 1e-3) break;
    }
  }
  // 定点原子累加。
  //
  // 注意精度：大 cascade 的**每条**射线贡献只有 ~1e-6，直接 ×65535 取整会变成 0
  // （整个远场就没了）。所以这里只存 cos 加权的辐射，把常数 2π/dirs
  // 挪到 finalize 再乘 —— 值域一下大了四个量级，定点够用。
  int bi = probe * 3;
  vec3 v = clamp(acc * d.z, 0.0, 1.0) * 65535.0;
  atomicAdd(accbuf[bi + 0], uint(v.x));
  atomicAdd(accbuf[bi + 1], uint(v.y));
  atomicAdd(accbuf[bi + 2], uint(v.z));
})";

// ---- 2. 把这一层的 SSBO 落成图集里的一条横带 ----
static const char* kFinalComp = R"(#version 430 core
layout(local_size_x=16,local_size_y=16)in;
uniform ivec2 u_probeGrid;
uniform int u_bandY;              // 本层在 cascadeTex 里的起始行
uniform float u_dOmega;           // 2*pi/dirs（trace 里没乘，挪到这里）
layout(std430,binding=0)readonly buffer Acc{ uint accbuf[]; };
layout(rgba32f,binding=0)writeonly uniform image2D u_out;
void main(){
  ivec2 g = ivec2(gl_GlobalInvocationID.xy);
  if (g.x >= u_probeGrid.x || g.y >= u_probeGrid.y) return;
  int i = g.y * u_probeGrid.x + g.x;
  vec3 v = vec3(float(accbuf[i*3+0]), float(accbuf[i*3+1]), float(accbuf[i*3+2])) /
           65535.0 * u_dOmega;
  imageStore(u_out, ivec2(g.x, u_bandY + g.y), vec4(v, 1.0));
})";

// ---- 3. 合并：每像素逐层手写双线性 ----
static const char* kMergeComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler3D u_vol;         // rgb = 自发光, a = 不透明度
uniform sampler3D u_color;       // rgb = 表面色,   a = 不透明度
uniform usampler2D u_top;
uniform sampler2D u_casc;         // 图集：每层一条横带
uniform ivec2 u_canvas;
uniform int u_count;
uniform int u_spacing[12];
uniform int u_band[12];
uniform ivec2 u_grid[12];
uniform vec3 u_sunCol;
uniform vec3 u_skyCol;
uniform float u_sunI;
uniform vec3 u_sunDir;
uniform int u_shadows;
uniform float u_giIntensity;
uniform float u_emitScale;
uniform int u_limit;
layout(rgba8,binding=0)writeonly uniform image2D u_out;

// 阴影射线（太阳）
float sunVis(vec3 p, vec3 d){
  if (u_shadows == 0) return 1.0;
  ivec3 c = ivec3(floor(p));
  ivec3 st = ivec3(d.x > 0.0 ? 1 : (d.x < 0.0 ? -1 : 0),
                   d.y > 0.0 ? 1 : (d.y < 0.0 ? -1 : 0),
                   d.z > 0.0 ? 1 : (d.z < 0.0 ? -1 : 0));
  const float INF = 1e30;
  vec3 td = vec3(d.x != 0.0 ? abs(1.0 / d.x) : INF,
                 d.y != 0.0 ? abs(1.0 / d.y) : INF,
                 d.z != 0.0 ? abs(1.0 / d.z) : INF);
  vec3 tm;
  for (int i = 0; i < 3; i++) {
    float dd = d[i]; int s = st[i];
    tm[i] = (dd == 0.0 || s == 0) ? INF
            : (((s > 0) ? float(c[i] + 1) : float(c[i])) - p[i]) / dd;
  }
  float T = 1.0;
  for (int it = 0; it < u_limit; it++) {
    if (tm.x < tm.y && tm.x < tm.z) { c.x += st.x; tm.x += td.x; }
    else if (tm.y < tm.z)          { c.y += st.y; tm.y += td.y; }
    else                           { c.z += st.z; tm.z += td.z; }
    if (c.x < 0 || c.y < 0 || c.z < 0 || c.x >= u_canvas.x || c.y >= u_canvas.y)
      return T;
    float A = texelFetch(u_vol, c, 0).a;
    if (A >= 1.0) return 0.0;
    if (A > 0.0) { T *= (1.0 - A); if (T < 1e-3) return 0.0; }
  }
  return T;
}

void main(){
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (p.x >= u_canvas.x || p.y >= u_canvas.y) return;
  uint h = texelFetch(u_top, p, 0).r;
  if (h == 0u) { imageStore(u_out, p, vec4(0.0, 0.0, 0.0, 1.0)); return; }
  // 表面色必须来自**颜色**纹理。我第一版拿了自发光纹理的 rgb，
  // 而地面的自发光是 0 -> col = 0 * lit = 0，非发光处全黑。
  vec3 topC = texelFetch(u_color, ivec3(p, int(h) - 1), 0).rgb;

  // 逐层手写双线性（不用硬件采样：各层横带宽度不同，硬件采样会跨层渗色）
  vec3 gi = vec3(0.0);
  for (int c = 0; c < u_count; c++) {
    float sp = float(u_spacing[c]);
    vec2 uv = (vec2(p) + 0.5) / sp - 0.5;     // 探测点坐标
    ivec2 i0 = ivec2(floor(uv));
    vec2 f = uv - vec2(i0);
    ivec2 gsz = u_grid[c];
    vec3 s = vec3(0.0);
    for (int dy = 0; dy <= 1; dy++)
      for (int dx = 0; dx <= 1; dx++) {
        int gx = clamp(i0.x + dx, 0, gsz.x - 1);
        int gy = clamp(i0.y + dy, 0, gsz.y - 1);
        float w = (dx == 1 ? f.x : 1.0 - f.x) * (dy == 1 ? f.y : 1.0 - f.y);
        s += texelFetch(u_casc, ivec2(gx, u_band[c] + gy), 0).rgb * w;
      }
    gi += s;
  }
  gi *= u_giIntensity;

  // 太阳 + 天空（解析项）
  vec3 p3 = vec3(float(p.x) + 0.5, float(p.y) + 0.5, float(h));
  float ndl = max(0.0, u_sunDir.z);
  float vis = (ndl > 0.0) ? sunVis(p3, u_sunDir) : 1.0;
  vec3 lit = u_sunCol * u_sunI * ndl * vis + u_skyCol + gi;

  vec3 col = clamp(topC * lit, 0.0, 1.0);
  imageStore(u_out, p, vec4(col, 1.0));
})";

inline float u8f(uint8_t v) { return (float)v / 255.0f; }

}  // namespace

class VoxelGpuTracer final : public VoxelRenderer {
public:
    ~VoxelGpuTracer() override { release(); }

    bool setVolume(const VoxelVolume& v) override {
        vol_ = v;
        const int X = v.X, Y = v.Y, Z = v.Z;
        if (X <= 0 || Y <= 0 || Z <= 0) return false;

        // 两张 3D 纹理：自发光（走射线用）与表面色（合并用），都带不透明度
        std::vector<uint8_t> tex((size_t)X * (size_t)Y * (size_t)Z * 4u, 0);
        std::vector<uint8_t> ctex((size_t)X * (size_t)Y * (size_t)Z * 4u, 0);
        for (int z = 0; z < Z; z++)
            for (int y = 0; y < Y; y++)
                for (int x = 0; x < X; x++) {
                    const size_t si = v.idx(x, y, z);
                    const size_t di = (((size_t)z * (size_t)Y + (size_t)y) * (size_t)X +
                                       (size_t)x) * 4u;
                    const RGBA8 e = v.emission[si];
                    tex[di + 0] = e.r;
                    tex[di + 1] = e.g;
                    tex[di + 2] = e.b;
                    tex[di + 3] = v.color[si].a;      // 不透明度
                    const RGBA8 c = v.color[si];
                    ctex[di + 0] = c.r;
                    ctex[di + 1] = c.g;
                    ctex[di + 2] = c.b;
                    ctex[di + 3] = c.a;
                }
        if (!volTex_) volTex_ = gfx::createTexture3D(GL_RGBA8, X, Y, Z, GL_RGBA,
                                                     GL_UNSIGNED_BYTE);
        if (!volTex_) return false;
        glBindTexture(GL_TEXTURE_3D, volTex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, X, Y, Z, GL_RGBA, GL_UNSIGNED_BYTE,
                        tex.data());
        if (!colorTex_) colorTex_ = gfx::createTexture3D(GL_RGBA8, X, Y, Z, GL_RGBA,
                                                         GL_UNSIGNED_BYTE);
        glBindTexture(GL_TEXTURE_3D, colorTex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, X, Y, Z, GL_RGBA, GL_UNSIGNED_BYTE,
                        ctex.data());
        glBindTexture(GL_TEXTURE_3D, 0);

        // 柱高（+1）作为 R8UI 2D 纹理给 shader
        std::vector<uint8_t> top((size_t)X * (size_t)Y, 0);
        for (int y = 0; y < Y; y++)
            for (int x = 0; x < X; x++) {
                const int h = v.columnTop(x, y);
                top[(size_t)y * (size_t)X + (size_t)x] =
                    (uint8_t)(h < 0 ? 0 : std::min(255, h + 1));
            }
        if (!topTex_) topTex_ = gfx::createTexture(GL_R8UI, X, Y, GL_RED_INTEGER,
                                                   GL_UNSIGNED_BYTE);
        glBindTexture(GL_TEXTURE_2D, topTex_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, X, Y, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                        top.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        return !gfx::checkGLError("voxel_gpu/setVolume");
    }

    void setSettings(const RenderSettings& s) override { set_ = s; }
    void resetAccumulation() override {}
    int accumFrames() const override { return 1; }
    bool converged() const override { return true; }
    GLuint resultTexture() const override { return outTex_; }

    void render(std::vector<RGBA8>& out) override {
        // GPU 版把结果留在显存；CPU 侧只做一次读回（GUI 会用 resultTexture，
        // 这里的读回只是为了兼容接口与离线出图）。
        const auto t0 = std::chrono::steady_clock::now();
        const int X = vol_.X, Y = vol_.Y;
        if (!ensurePrograms() || X <= 0 || Y <= 0) return;

        const int want = std::clamp(set_.cascades, 1, kMaxCascades);
        const float L0 = std::max(0.5f, set_.cascadeBase);
        const int K0 = std::max(1, set_.cascadeRays);

        // ---- 各层探测点网格 + 图集布局 ----
        int bandY[kMaxCascades] = {0}, spacing[kMaxCascades] = {0};
        int gw[kMaxCascades] = {0}, gh[kMaxCascades] = {0};
        int totalH = 0, maxW = 1;
        for (int c = 0; c < want; c++) {
            spacing[c] = 1 << c;
            gw[c] = (X + spacing[c] - 1) / spacing[c];
            gh[c] = (Y + spacing[c] - 1) / spacing[c];
            bandY[c] = totalH;
            totalH += gh[c];
            maxW = std::max(maxW, gw[c]);
        }

        if (!cascTex_ || cascW_ != maxW || cascH_ != totalH) {
            if (cascTex_) glDeleteTextures(1, &cascTex_);
            cascTex_ = gfx::createTexture(GL_RGBA32F, maxW, totalH, GL_RGBA, GL_FLOAT);
            cascW_ = maxW;
            cascH_ = totalH;
        }
        if (!outTex_ || outW_ != X || outH_ != Y) {
            if (outTex_) glDeleteTextures(1, &outTex_);
            outTex_ = gfx::createTexture(GL_RGBA8, X, Y, GL_RGBA, GL_UNSIGNED_BYTE);
            outW_ = X;
            outH_ = Y;
        }

        const int limit = (X + Y + vol_.Z) * 2 + 8;

        // ---- 每层：走射线 -> 落带 ----
        for (int c = 0; c < want; c++) {
            const int dirs = K0 * (1 << (2 * c));
            const int probes = gw[c] * gh[c];
            const size_t accN = (size_t)probes * 3u;
            if (accBuf_.size() < accN) {
                accBuf_.assign(accN, 0u);
                if (!accSSBO_) glGenBuffers(1, &accSSBO_);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, accSSBO_);
                glBufferData(GL_SHADER_STORAGE_BUFFER, accN * sizeof(GLuint), nullptr,
                             GL_DYNAMIC_DRAW);
            }
            std::vector<GLuint> zero(accN, 0u);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, accSSBO_);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, accN * sizeof(GLuint), zero.data());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO_);

            glUseProgram(traceProg_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_3D, volTex_);
            glUniform1i(glGetUniformLocation(traceProg_, "u_vol"), 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, topTex_);
            glUniform1i(glGetUniformLocation(traceProg_, "u_top"), 1);
            glUniform2i(glGetUniformLocation(traceProg_, "u_canvas"), X, Y);
            glUniform1i(glGetUniformLocation(traceProg_, "u_probeSpacing"), spacing[c]);
            glUniform2i(glGetUniformLocation(traceProg_, "u_probeGrid"), gw[c], gh[c]);
            glUniform1i(glGetUniformLocation(traceProg_, "u_dirs"), dirs);
            const float r0 = (c == 0) ? 0.0f : L0 * (float)(1 << (c - 1));
            const float r1 = L0 * (float)(1 << c);
            glUniform1f(glGetUniformLocation(traceProg_, "u_r0"), r0);
            glUniform1f(glGetUniformLocation(traceProg_, "u_r1"), r1);
            glUniform1i(glGetUniformLocation(traceProg_, "u_limit"), limit);
            glUniform3i(glGetUniformLocation(traceProg_, "u_volSize"), X, Y, vol_.Z);
            const GLuint threads = (GLuint)(probes * dirs);
            glDispatchCompute((threads + 63u) / 64u, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

            glUseProgram(finalProg_);
            glUniform2i(glGetUniformLocation(finalProg_, "u_probeGrid"), gw[c], gh[c]);
            glUniform1i(glGetUniformLocation(finalProg_, "u_bandY"), bandY[c]);
            glUniform1f(glGetUniformLocation(finalProg_, "u_dOmega"),
                        6.2831853f / (float)dirs);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO_);
            glBindImageTexture(0, cascTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
            glDispatchCompute((GLuint)((gw[c] + 15) / 16), (GLuint)((gh[c] + 15) / 16), 1);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        }

        if (getenv("PB_RCDBG")) {
            std::vector<float> back((size_t)cascW_ * (size_t)cascH_ * 4u, 0.0f);
            glBindTexture(GL_TEXTURE_2D, cascTex_);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, back.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            for (int c = 0; c < want; c++) {
                double s = 0;
                float mx = 0;
                for (int y = 0; y < gh[c]; y++)
                    for (int x = 0; x < gw[c]; x++) {
                        const float v = back[((size_t)(bandY[c] + y) * (size_t)cascW_ +
                                              (size_t)x) * 4u];
                        s += v;
                        if (v > mx) mx = v;
                    }
                fprintf(stderr, "[rc] c=%d spacing=%d grid=%dx%d dirs=%d band=%d 和=%.4f 最大=%.4f\\n",
                        c, spacing[c], gw[c], gh[c], K0 * (1 << (2 * c)), bandY[c], s, mx);
            }
        }

        // ---- 合并 ----
        glUseProgram(mergeProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_3D, volTex_);
        glUniform1i(glGetUniformLocation(mergeProg_, "u_vol"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, topTex_);
        glUniform1i(glGetUniformLocation(mergeProg_, "u_top"), 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_3D, colorTex_);
        glUniform1i(glGetUniformLocation(mergeProg_, "u_color"), 2);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, cascTex_);
        glUniform1i(glGetUniformLocation(mergeProg_, "u_casc"), 3);
        glUniform2i(glGetUniformLocation(mergeProg_, "u_canvas"), X, Y);
        glUniform1i(glGetUniformLocation(mergeProg_, "u_count"), want);
        glUniform1iv(glGetUniformLocation(mergeProg_, "u_spacing"), want, spacing);
        glUniform1iv(glGetUniformLocation(mergeProg_, "u_band"), want, bandY);
        for (int c = 0; c < want; c++) {
            char nm[32];
            std::snprintf(nm, sizeof(nm), "u_grid[%d]", c);
            glUniform2i(glGetUniformLocation(mergeProg_, nm), gw[c], gh[c]);
        }
        glUniform3fv(glGetUniformLocation(mergeProg_, "u_sunCol"), 1, set_.sunColor);
        glUniform3fv(glGetUniformLocation(mergeProg_, "u_skyCol"), 1, set_.skyColor);
        glUniform1f(glGetUniformLocation(mergeProg_, "u_sunI"), set_.sunIntensity);
        glUniform3fv(glGetUniformLocation(mergeProg_, "u_sunDir"), 1, set_.sunDir);
        glUniform1i(glGetUniformLocation(mergeProg_, "u_shadows"), set_.shadows ? 1 : 0);
        glUniform1f(glGetUniformLocation(mergeProg_, "u_giIntensity"), set_.giIntensity);
        glUniform1f(glGetUniformLocation(mergeProg_, "u_emitScale"), set_.emitScale);
        glUniform1i(glGetUniformLocation(mergeProg_, "u_limit"), limit);
        glBindImageTexture(0, outTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute((GLuint)((X + 7) / 8), (GLuint)((Y + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        glUseProgram(0);
        glActiveTexture(GL_TEXTURE0);

        // 读回（兼容接口 / 离线出图）
        out.assign((size_t)X * (size_t)Y, RGBA8{0, 0, 0, 255});
        glBindTexture(GL_TEXTURE_2D, outTex_);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        lastMs_ = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
    }

    double lastMs() const override { return lastMs_; }

private:
    bool ensurePrograms() {
        if (traceProg_) return true;
        traceProg_ = gfx::createComputeProgram("rc/trace", kTraceComp);
        finalProg_ = gfx::createComputeProgram("rc/final", kFinalComp);
        mergeProg_ = gfx::createComputeProgram("rc/merge", kMergeComp);
        return traceProg_ && finalProg_ && mergeProg_;
    }

    void release() {
        const GLuint t[4] = {volTex_, colorTex_, topTex_, outTex_};
        for (GLuint x : t)
            if (x) glDeleteTextures(1, &x);
        if (cascTex_) glDeleteTextures(1, &cascTex_);
        if (accSSBO_) glDeleteBuffers(1, &accSSBO_);
        if (traceProg_) glDeleteProgram(traceProg_);
        if (finalProg_) glDeleteProgram(finalProg_);
        if (mergeProg_) glDeleteProgram(mergeProg_);
        volTex_ = colorTex_ = topTex_ = outTex_ = cascTex_ = 0;
        traceProg_ = finalProg_ = mergeProg_ = 0;
    }

    VoxelVolume vol_;
    RenderSettings set_;
    GLuint volTex_ = 0, colorTex_ = 0, topTex_ = 0, outTex_ = 0, cascTex_ = 0,
           accSSBO_ = 0;
    int cascW_ = 0, cascH_ = 0, outW_ = 0, outH_ = 0;
    std::vector<GLuint> accBuf_;
    GLuint traceProg_ = 0, finalProg_ = 0, mergeProg_ = 0;
    double lastMs_ = 0.0;
};

std::unique_ptr<VoxelRenderer> createVoxelGpuRenderer() {
    return std::make_unique<VoxelGpuTracer>();
}

}  // namespace pb::render
