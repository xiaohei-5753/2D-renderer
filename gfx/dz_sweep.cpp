// gfx/dz_sweep.cpp — d-z 平面光追：只做搬运，射线交给现有引擎
//
// ## 分工
//
//   d-z 平面 = **d 的长度 × 层数** 的窄而宽位图：
//       第 1 层合成的 z 坐标就是 1，第 2 层是 2，依此类推。
//
//   这里**只负责**把世界铺成这张位图（一个 compute pass），
//   然后把它按图层喂给现有的 2D 光线追踪引擎 gfx::LightSweep ——
//   射线推进、遮挡、累积、合成全部由引擎完成，本文件不写任何射线逻辑。
//   最后取「每列最高点加一」那一格的受光，写回画布对应像素。
//
// ## 斜射怎么体现
//
//   引擎在它的网格里走 45° 射线。所以"斜射"由**位图的横向拉伸**表达：
//   位图横向 = len * S 列（S = stepsPerLevel），一层的竖向 1 格对应横向 S 格，
//   于是引擎的 45° 就等价于真实世界的斜率。这样就不需要给引擎加参数。
#include "dz_sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/layer.h"
#include "gl_texture.h"
#include "light_sweep.h"
#include "shader.h"

namespace pb::gfx {
namespace {

// 第 1 条扫描线：把世界沿方向 ω 采样、铺成 d-z 位图。
//
//   g.x = 位图列（0 .. len*S-1），对应世界位置 u = g.x / S
//   g.y = 位图行，第 z 层放在行 (L-z)，即 z=1 在最下、z=L 在最上
//
// 每格输出：rgb = 该处的颜色（表面格带自发光），a = 覆盖率；
// 另出一张 flag：r = 有像素（挡光），g = 表面格。
static const char* kBuildComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_albedo;
uniform sampler2D u_mat;
uniform usampler2D u_height;     // R8UI: 表面层号（0 = 空气）
uniform ivec2 u_canvasWH;
uniform ivec2 u_grid;            // (W, L)：W = len*S
uniform vec2 u_origin;
uniform vec2 u_step;
uniform float u_scale;           // S：横向拉伸
// 目标：d-z 位图（**一张 2D 画布**，宽 W、高 L），第 z 层放在行 (L-z)
layout(rgba8,binding=0)writeonly uniform image2D u_pix;
layout(rgba8,binding=1)writeonly uniform image2D u_flag;
void main(){
  ivec2 g=ivec2(gl_GlobalInvocationID.xy);
  if(g.x>=u_grid.x||g.y>=u_grid.y)return;
  const int z=u_grid.y-g.y;                // 行 -> 层号（z = 1..L）

  const float u=float(g.x)/u_scale;
  vec2 wp=u_origin+u_step*u;
  ivec2 ip=ivec2(floor(wp));
  if(ip.x<0||ip.y<0||ip.x>=u_canvasWH.x||ip.y>=u_canvasWH.y){
    imageStore(u_pix,g,vec4(0.0));
    imageStore(u_flag,g,vec4(0.0));
    return;
  }
  uint h=texelFetch(u_height,ip,0).r;
  if(h==0u||z>int(h)){                     // 第 z 层这里没有像素
    imageStore(u_pix,g,vec4(0.0));
    imageStore(u_flag,g,vec4(0.0));
    return;
  }
  if(z==int(h)){                           // 表面格：真实颜色 + 自发光
    vec4 alb=texelFetch(u_albedo,ip,0);
    vec4 mm=texelFetch(u_mat,ip,0);
    imageStore(u_pix,g,vec4(mm.rgb,alb.a));
    imageStore(u_flag,g,vec4(1.0,1.0,0.0,1.0));
  }else{                                   // 下层：只挡光
    imageStore(u_pix,g,vec4(0.5,0.5,0.5,1.0));
    imageStore(u_flag,g,vec4(1.0,0.0,0.0,1.0));
  }
})";
// 第 3 步：把引擎渲染结果里「每列最高点加一」那一格取出来，写回画布。
// 引擎输出的每一层是 len*S 宽的一条；受力面在列 u*S 处。
static const char* kResolveComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_lit;         // 引擎的渲染结果（一张 2D 图）
uniform usampler2D u_height;
uniform ivec2 u_canvasWH;
uniform ivec2 u_grid;             // (W, L)
uniform vec2 u_origin;
uniform vec2 u_step;
uniform float u_scale;
uniform int u_layers;

layout(std430,binding=0)buffer AccBuf{uint d[];};
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_canvasWH.x||p.y>=u_canvasWH.y)return;
  uint h=texelFetch(u_height,p,0).r;
  if(h==0u)return;

  // 「最高点加一」：表面是第 h 层，取它**上面一层**（第 h+1 层）的受光。
  // 引擎的层索引是 0 基，所以"第 h+1 层"= 索引 h。
  // 受力面是第 h 层 -> 位图行 = L-h；「最高点加一」= 行 (L-h-1)
  int gy=u_layers-1-int(h);
  if(gy<0)gy=0;
  if(gy>=u_grid.y)return;
  float t=dot(vec2(p)+vec2(0.5)-u_origin,u_step)/max(dot(u_step,u_step),1e-6);
  int gx=int(floor(t*u_scale+0.5));
  if(gx<0||gx>=u_grid.x)return;
  vec3 lt=texelFetch(u_lit,ivec2(gx,gy),0).rgb;

  const int bi=(p.y*u_canvasWH.x+p.x)*3;
  atomicAdd(d[bi+0],uint(clamp(lt.r,0.0,1.0)*65535.0));
  atomicAdd(d[bi+1],uint(clamp(lt.g,0.0,1.0)*65535.0));
  atomicAdd(d[bi+2],uint(clamp(lt.b,0.0,1.0)*65535.0));
})";

// 第 4 步：归一化 + 着色
static const char* kAccumComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_albedo;
uniform sampler2D u_mat;
uniform usampler2D u_height;
uniform ivec2 u_canvasWH;
uniform int u_dirs;
uniform float u_emit_scale;
layout(std430,binding=0)readonly buffer AccBuf{uint d[];};
layout(rgba8,binding=0)writeonly uniform image2D u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_canvasWH.x||p.y>=u_canvasWH.y)return;
  uint h=texelFetch(u_height,p,0).r;
  if(h==0u){ imageStore(u_out,p,vec4(0.0,0.0,0.0,1.0)); return; }
  const int bi=(p.y*u_canvasWH.x+p.x)*3;
  vec3 lt=vec3(float(d[bi+0]),float(d[bi+1]),float(d[bi+2]))/65535.0/float(max(u_dirs,1));
  vec4 alb=texelFetch(u_albedo,p,0);
  vec4 m=texelFetch(u_mat,p,0);
  imageStore(u_out,p,vec4(clamp(alb.rgb*lt+m.rgb*u_emit_scale,0.0,1.0),1.0));
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
    resolveProg_ = createComputeProgram("dz/resolve", kResolveComp);
    accumProg_ = createComputeProgram("dz/accum", kAccumComp);
    if (!buildProg_ || !resolveProg_ || !accumProg_) {
        fprintf(stderr, "[dz] 着色器初始化失败\n");
        return false;
    }
    return !checkGLError("dz/init");
}

void DzSweep::shutdown() {
    releaseTextures();
    if (buildProg_) glDeleteProgram(buildProg_);
    if (resolveProg_) glDeleteProgram(resolveProg_);
    if (accumProg_) glDeleteProgram(accumProg_);
    buildProg_ = resolveProg_ = accumProg_ = 0;
    if (engine_) { engine_->shutdown(); delete engine_; engine_ = nullptr; }
    w_ = h_ = layers_ = 0;
}

void DzSweep::releaseTextures() {
    const GLuint t[4] = {albedoTex_, matTex_, heightTex_, outTex_};
    for (GLuint x : t)
        if (x) glDeleteTextures(1, &x);
    albedoTex_ = matTex_ = heightTex_ = outTex_ = 0;
    if (pixArray_) { glDeleteTextures(1, &pixArray_); pixArray_ = 0; }
    if (flagArray_) { glDeleteTextures(1, &flagArray_); flagArray_ = 0; }
    if (accSSBO_) { glDeleteBuffers(1, &accSSBO_); accSSBO_ = 0; }
}

void DzSweep::uploadField(const SurfaceField& f) {
    if (!valid() || f.w != w_ || f.h != h_) return;
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
    if (!valid() || !buildProg_) return false;
    const int dirs = std::max(1, prm.directions);
    const int L = layers_;
    const int len = (int)std::ceil(std::sqrt((double)w_ * w_ + (double)h_ * h_));
    // 横向拉伸 S 倍：引擎在网格里走 45°，拉伸之后等价的真实斜率就是 S
    const int S = std::max(1, (int)std::lround(prm.stepsPerLevel));
    const int W = len * S;

    if (!pixArray_ || bitmapW_ != W || bitmapH_ != L) {
        if (pixArray_) glDeleteTextures(1, &pixArray_);
        if (flagArray_) glDeleteTextures(1, &flagArray_);
        pixArray_ = createTexture(GL_RGBA8, W, L, GL_RGBA, GL_UNSIGNED_BYTE);
        flagArray_ = createTexture(GL_RGBA8, W, L, GL_RGBA, GL_UNSIGNED_BYTE);
        bitmapW_ = W; bitmapH_ = L;
        if (!pixArray_ || !flagArray_) { fprintf(stderr, "[dz] 位图创建失败\n"); return false; }
    }

    // 现有引擎：d-z 位图就是它要渲染的那张画布（单层，宽 W、高 L）
    if (!engine_) engine_ = new LightSweep();
    if (!engine_->init(W, L)) return false;
    engine_->setLayerCount(1);

    const size_t accN = (size_t)w_ * (size_t)h_ * 3u;
    if (!accSSBO_) {
        glGenBuffers(1, &accSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, accSSBO_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, accN * sizeof(GLuint), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    {
        std::vector<GLuint> zero(accN, 0u);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, accSSBO_);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, accN * sizeof(GLuint), zero.data());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO_);
    }

    std::vector<RGBA8> bmp((size_t)W * (size_t)L);
    std::vector<Material> mats((size_t)W * (size_t)L);

    for (int k = 0; k < dirs; k++) {
        const float ang = (float)(2.0 * 3.14159265358979 * (double)k / (double)dirs);
        const float ux = std::cos(ang), uy = std::sin(ang);
        const float cx = (float)w_ * 0.5f, cy = (float)h_ * 0.5f;
        const float r0 = 0.5f * (float)len;         // 世界尺度上的半长
        const float ox = cx - ux * r0, oy = cy - uy * r0;

        // ---- 第 1 条扫描线：铺出 d-z 位图 ----
        glUseProgram(buildProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, albedoTex_);
        glUniform1i(glGetUniformLocation(buildProg_, "u_albedo"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, matTex_);
        glUniform1i(glGetUniformLocation(buildProg_, "u_mat"), 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, heightTex_);
        glUniform1i(glGetUniformLocation(buildProg_, "u_height"), 2);
        glUniform2i(glGetUniformLocation(buildProg_, "u_canvasWH"), w_, h_);
        glUniform2i(glGetUniformLocation(buildProg_, "u_grid"), W, L);
        glUniform2f(glGetUniformLocation(buildProg_, "u_origin"), ox, oy);
        glUniform2f(glGetUniformLocation(buildProg_, "u_step"), ux, uy);
        glUniform1f(glGetUniformLocation(buildProg_, "u_scale"), (float)S);
        glBindImageTexture(0, pixArray_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(1, flagArray_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute((GLuint)((W + 7) / 8), (GLuint)((L + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);

        // ---- 把位图作为**一张画布**交给引擎 ----
        glBindTexture(GL_TEXTURE_2D, pixArray_);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, bmp.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        std::fill(mats.begin(), mats.end(), Material{});
        Image img(W, L);
        for (int y = 0; y < L; y++)
            for (int x = 0; x < W; x++) img.at(x, y) = bmp[(size_t)y * (size_t)W + (size_t)x];
        engine_->uploadAll(img, mats.data());
        // 天光与自发光倍率通过引擎自己的设置传进去
        engine_->settings().ambient[0] = prm.sky[0];
        engine_->settings().ambient[1] = prm.sky[1];
        engine_->settings().ambient[2] = prm.sky[2];
        engine_->settings().emitScale = prm.emitScale;

        // ---- 第 2 条扫描线：引擎自己跑 2D 光追 ----
        engine_->render();

        // ---- 第 3 步：取「最高点加一」，写回画布 ----
        glUseProgram(resolveProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, engine_->resultTexture());
        glUniform1i(glGetUniformLocation(resolveProg_, "u_lit"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, heightTex_);
        glUniform1i(glGetUniformLocation(resolveProg_, "u_height"), 1);
        glUniform2i(glGetUniformLocation(resolveProg_, "u_canvasWH"), w_, h_);
        glUniform2i(glGetUniformLocation(resolveProg_, "u_grid"), W, L);
        glUniform2f(glGetUniformLocation(resolveProg_, "u_origin"), ox, oy);
        glUniform2f(glGetUniformLocation(resolveProg_, "u_step"), ux, uy);
        glUniform1f(glGetUniformLocation(resolveProg_, "u_scale"), (float)S);
        glUniform1i(glGetUniformLocation(resolveProg_, "u_layers"), L);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO_);
        glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    }

    // ---- 第 4 步：归一化 + 着色 ----
    glUseProgram(accumProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, albedoTex_);
    glUniform1i(glGetUniformLocation(accumProg_, "u_albedo"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, matTex_);
    glUniform1i(glGetUniformLocation(accumProg_, "u_mat"), 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, heightTex_);
    glUniform1i(glGetUniformLocation(accumProg_, "u_height"), 2);
    glUniform2i(glGetUniformLocation(accumProg_, "u_canvasWH"), w_, h_);
    glUniform1i(glGetUniformLocation(accumProg_, "u_dirs"), dirs);
    glUniform1f(glGetUniformLocation(accumProg_, "u_emit_scale"), prm.emitScale);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO_);
    glBindImageTexture(0, outTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    return !checkGLError("dz/render");
}
}  // namespace pb::gfx
