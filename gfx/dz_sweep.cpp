// gfx/dz_sweep.cpp — d-z 平面 2D 光追（套用现有扫描线）
//
// 「两个扫描线 + 合成」：
//
//   ① build  ：生成 d-z 位图（第 1 条扫描线）
//   ② trace  ：在 d-z 位图上跑完整的 2D 光线追踪（逐格 DDA，原样照搬现有累积式）
//   ③ resolve：取每列「最高点加一」那一格的受光，写回它在 (x,y) 上的像素并合成
//
// ## 位图坐标
//
//   u（横轴 0..len-1） = 沿收集方向 ω 的位置
//   z（纵轴 0..L-1）   = 图层高度；位图行 row = L-1-z（图片 y 向下、z 向上）
//
// 每格放「第 z 层在该采样点上的像素」：表面那一格带真实颜色与自发光，
// 其下方各层只负责遮挡。**每一格都是像素**，所以遮挡/穿透/自发光天然正确。
#include "dz_sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "gl_texture.h"
#include "shader.h"

namespace pb::gfx {
namespace {

// ---------------------------------------------------------------- ① 生成 d-z 位图
//
// 一个线程 = 一个 d-z 格子。直接按格子反查世界坐标，不用散射（更简单也更稳）。
static const char* kBuildComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_albedo;
uniform sampler2D u_mat;
uniform usampler2D u_height;      // R8UI: 表面高度（层号，0 = 空气）
uniform ivec2 u_canvasWH;
uniform ivec2 u_grid;             // (len, L)
uniform vec2 u_origin;
uniform vec2 u_step;
uniform int u_dir;
layout(rgba8,binding=0)writeonly uniform image2DArray u_pix;   // rgb = 颜色, a = 覆盖率
layout(rgba8,binding=1)writeonly uniform image2DArray u_flag;  // r = 有像素, g = 表面格
layout(rgba8,binding=2)writeonly uniform image2DArray u_light; // 累积光（先清零）
void main(){
  ivec2 g=ivec2(gl_GlobalInvocationID.xy);
  if(g.x>=u_grid.x||g.y>=u_grid.y)return;
  int u=g.x, L=u_grid.y, z=L-1-g.y;
  ivec3 cell=ivec3(g,u_dir);

  // 先把这一格清空（trace 只写命中的格，没命中的必须保持 0）
  imageStore(u_light,cell,vec4(0.0));

  vec2 wp=u_origin+u_step*float(u);
  ivec2 ip=ivec2(floor(wp));
  if(ip.x<0||ip.y<0||ip.x>=u_canvasWH.x||ip.y>=u_canvasWH.y){
    imageStore(u_pix,cell,vec4(0.0));
    imageStore(u_flag,cell,vec4(0.0));
    return;
  }
  uint h=texelFetch(u_height,ip,0).r;
  if(h==0u||z>int(h)-1){
    imageStore(u_pix,cell,vec4(0.0));
    imageStore(u_flag,cell,vec4(0.0));
    return;
  }
  float solid=1.0, isSurf=0.0;
  vec4 col;
  if(z==int(h)-1){
    // 表面格：真实颜色 + 自发光（自发光作为"这一格发出的光"交给 trace）
    vec4 alb=texelFetch(u_albedo,ip,0);
    vec4 m=texelFetch(u_mat,ip,0);
    col=vec4(m.rgb,alb.a);
    isSurf=1.0;
  }else{
    // 下层：只负责遮挡
    col=vec4(0.5,0.5,0.5,1.0);
  }
  imageStore(u_pix,cell,col);
  imageStore(u_flag,cell,vec4(solid,isSurf,0.0,1.0));
})";
// ---------------------------------------------------------------- ② 在 d-z 位图上跑 2D 光追
//
// **逐格 DDA**（不是参数化射线）：每命中一格就累积一次
//     lt = lt * mix(1, albedo, cov) * (1-cov) + em
// 与现有 2D 光追完全同一套推进与累积方式，只是网格换成了 d-z。
//
// z 方向的推进按 u_zStep 缩放（= 1/stepsPerLevel）：这是有限阴影的来源。
// 每条射线从网格外起步，落在 (u, z) 这一格时就记下它的受光 ——
// 于是每个格子都能拿到"从哪个方向来的光照亮了我"。
static const char* kTraceComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_pix;
uniform sampler2DArray u_flag;
uniform ivec2 u_grid;        // (len, L)
uniform vec3 u_ambient;
uniform float u_emit_scale;
uniform float u_steps;       // = stepsPerLevel，决定射线斜率
uniform int u_dir;
layout(rgba8,binding=0)writeonly uniform image2DArray u_light;
void main(){
  int ri=int(gl_GlobalInvocationID.x);
  int len=u_grid.x, L=u_grid.y;

  // ## 每个像素投射**自己的**射线
  //
  // 一个线程 = 位图里的一个格子 (u0, z0)，它就代表"该 d 处、该层的那个像素"。
  // 射线从像素**自己**出发朝天空走：层号一路上升，横向按斜率偏移。
  // 沿途命中谁就累积谁：
  //     lt = lt * mix(1, albedo, cov) * (1-cov) + em
  // 这正是现有 2D 光追的累积式，逐格推进，一格不漏。
  //
  // 注意起点那一格是**受力面自己**，必须跳过（否则自己挡自己，全黑）。
  if(ri>=len*L)return;
  const int u0=ri%len;
  const int z0=ri/len;                 // 起点层号

  vec3 lt=u_ambient;
  // 横向偏移方向：由 u_dir 的奇偶决定，让不同方向的光从两侧来
  const int side=((u_dir&1)==0)?1:-1;
  // 斜率：每上升 1 层，横向偏移 1/S 格（S = stepsPerLevel）。
  // 用**绝对坐标**重算 u（而不是增量累加），避免浮点漂移改变格子的对齐。
  const float invS=(u_steps>0.5f)?(1.0f/u_steps):1.0f;
  int u=u0, z=z0;
  int guard=0;
  while(guard < 4*(len+L)){
    guard++;
    if(u<0||u>=len)break;
    if(z<0||z>=L)break;
    if(u!=u0||z!=z0){                  // 跳过起点（受力面自己）
      ivec3 g=ivec3(u,L-1-z,u_dir);
      vec4 fl=texelFetch(u_flag,g,0);
      if(fl.r>0.5){
        vec4 c=texelFetch(u_pix,g,0);
        float cov=(fl.g>0.5)?c.a:1.0;  // 表面格用它的覆盖率，下层完全遮挡
        cov=clamp(cov,0.0,1.0);
        vec3 em=(fl.g>0.5)?c.rgb*u_emit_scale:vec3(0.0);
        lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)+em;
      }
    }
    // 朝天空走：层号 +1；横向偏移按斜率由**绝对高度差**算出
    z+=1;
    if(z>=L)break;
    u=u0+side*int(floor(float(z-z0)*invS+0.5f));
  }

  // 写出：这一格收到的光
  {
    ivec3 g0=ivec3(u0,L-1-z0,u_dir);
    imageStore(u_light,g0,vec4(lt,1.0));
  }
})";
// ---------------------------------------------------------------- ③ 把每个方向的受光累积到画布
//
// ⚠️ 不能直接往输出纹理上写（每个方向会把上一个方向的结果覆盖掉），
// 所以按现有的做法用定点原子累加，最后再统一归一化。
static const char* kResolveComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_light;
uniform usampler2D u_height;
uniform ivec2 u_canvasWH;
uniform ivec2 u_grid;
uniform vec2 u_origin;
uniform vec2 u_step;
uniform int u_dir;
// 定点累积：0..65535 映射 0..1，与原管线一致
layout(std430,binding=0)buffer AccBuf{uint d[];};
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_canvasWH.x||p.y>=u_canvasWH.y)return;
  uint h=texelFetch(u_height,p,0).r;
  if(h==0u)return;

  // 反查这个像素在该方向的位图里落在哪一列
  const vec2 pf=vec2(p)+vec2(0.5);
  float un=dot(pf-u_origin,u_step)/max(dot(u_step,u_step),1e-6);
  int u=int(floor(un+0.5));
  if(u<0||u>=u_grid.x)return;

  // 「最高点加一」：表面格是 z=h-1，取它上面一格 z=h
  int z=int(h);                       // = (h-1)+1，即表面格**上面**那一格
  if(z<0||z>=u_grid.y)return;
  vec3 lt=texelFetch(u_light,ivec3(u,u_grid.y-1-z,u_dir),0).rgb;

  const int bi=(p.y*u_canvasWH.x+p.x)*3;
  atomicAdd(d[bi+0],uint(clamp(lt.r,0.0,1.0)*65535.0));
  atomicAdd(d[bi+1],uint(clamp(lt.g,0.0,1.0)*65535.0));
  atomicAdd(d[bi+2],uint(clamp(lt.b,0.0,1.0)*65535.0));
})";

// ④ 归一化 + 着色：颜色 = albedo * 平均受光 + 自发光
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
  vec3 c=alb.rgb*lt+m.rgb*u_emit_scale;
  imageStore(u_out,p,vec4(clamp(c,0.0,1.0),1.0));
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
    traceProg_ = createComputeProgram("dz/trace", kTraceComp);
    resolveProg_ = createComputeProgram("dz/resolve", kResolveComp);
    accumProg_ = createComputeProgram("dz/accum", kAccumComp);
    if (!buildProg_ || !traceProg_ || !resolveProg_ || !accumProg_) {
        fprintf(stderr, "[dz] 着色器初始化失败\n");
        return false;
    }
    return !checkGLError("dz/init");
}

void DzSweep::shutdown() {
    releaseTextures();
    if (buildProg_) glDeleteProgram(buildProg_);
    if (traceProg_) glDeleteProgram(traceProg_);
    if (resolveProg_) glDeleteProgram(resolveProg_);
    if (accumProg_) glDeleteProgram(accumProg_);
    buildProg_ = traceProg_ = resolveProg_ = accumProg_ = 0;
    w_ = h_ = layers_ = 0;
}

void DzSweep::releaseTextures() {
    const GLuint t[4] = {albedoTex_, matTex_, heightTex_, outTex_};
    for (GLuint x : t)
        if (x) glDeleteTextures(1, &x);
    albedoTex_ = matTex_ = heightTex_ = outTex_ = 0;
    if (lightTex_) { glDeleteTextures(1, &lightTex_); lightTex_ = 0; }
    const GLuint bm[2] = {pixArray_, flagArray_};
    for (GLuint x : bm)
        if (x) glDeleteTextures(1, &x);
    pixArray_ = flagArray_ = 0;
    bitmapW_ = bitmapH_ = bitmapD_ = 0;
    if (accSSBO_) { glDeleteBuffers(1, &accSSBO_); accSSBO_ = 0; }
}

void DzSweep::uploadField(const SurfaceField& f) {
    if (!valid() || f.w != w_ || f.h != h_) return;

    std::vector<float> mat((size_t)w_ * h_ * 4u, 0.0f);
    const size_t n = (size_t)w_ * h_;
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
    if (!valid() || !buildProg_ || !traceProg_ || !resolveProg_ || !accumProg_) return false;
    const int dirs = std::max(1, prm.directions);
    const int L = std::max(1, layers_);
    const int len = (int)std::ceil(std::sqrt((double)w_ * w_ + (double)h_ * h_));
    ensureBitmap(len, L, dirs);
    if (!pixArray_ || !flagArray_ || !lightTex_) return false;

    // 累积缓冲：每个画布像素 3 个 u32（定点累加）
    const size_t accFloats = (size_t)w_ * (size_t)h_ * 3u;
    if (!accSSBO_) {
        glGenBuffers(1, &accSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, accSSBO_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, accFloats * sizeof(GLuint), nullptr,
                     GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    {
        std::vector<GLuint> zero(accFloats, 0u);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, accSSBO_);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, accFloats * sizeof(GLuint), zero.data());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO_);
    }

    const float emitScale = prm.emitScale;

    for (int k = 0; k < dirs; k++) {
        const float ang = (float)(2.0 * 3.14159265358979 * (double)k / (double)dirs);
        const float ux = std::cos(ang), uy = std::sin(ang);
        const float cx = (float)w_ * 0.5f, cy = (float)h_ * 0.5f;
        const float r0 = 0.5f * (float)len;
        const float ox = cx - ux * r0, oy = cy - uy * r0;

        // ---------- 第 1 条扫描线：生成 d-z 位图 ----------
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
        glUniform2i(glGetUniformLocation(buildProg_, "u_grid"), len, L);
        glUniform2f(glGetUniformLocation(buildProg_, "u_origin"), ox, oy);
        glUniform2f(glGetUniformLocation(buildProg_, "u_step"), ux, uy);
        glUniform1i(glGetUniformLocation(buildProg_, "u_dir"), k);
        glBindImageTexture(0, pixArray_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(1, flagArray_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glBindImageTexture(2, lightTex_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute((GLuint)((len + 7) / 8), (GLuint)((L + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);

        // ---------- 第 2 条扫描线：在 d-z 位图上跑 2D 光追 ----------
        glUseProgram(traceProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, pixArray_);
        glUniform1i(glGetUniformLocation(traceProg_, "u_pix"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, flagArray_);
        glUniform1i(glGetUniformLocation(traceProg_, "u_flag"), 1);
        glUniform2i(glGetUniformLocation(traceProg_, "u_grid"), len, L);
        glUniform3fv(glGetUniformLocation(traceProg_, "u_ambient"), 1, prm.sky);
        glUniform1f(glGetUniformLocation(traceProg_, "u_emit_scale"), emitScale);
        glUniform1f(glGetUniformLocation(traceProg_, "u_steps"),
                    prm.stepsPerLevel > 0.0f ? prm.stepsPerLevel : 1.0f);
        glUniform1i(glGetUniformLocation(traceProg_, "u_dir"), k);
        glBindImageTexture(0, lightTex_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        const int rays = len * L;
        glDispatchCompute((GLuint)((rays + 255) / 256), 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);

        // ---------- 合成：把该方向每个像素的受光累加到画布 ----------
        glUseProgram(resolveProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, lightTex_);
        glUniform1i(glGetUniformLocation(resolveProg_, "u_light"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, heightTex_);
        glUniform1i(glGetUniformLocation(resolveProg_, "u_height"), 1);
        glUniform2i(glGetUniformLocation(resolveProg_, "u_canvasWH"), w_, h_);
        glUniform2i(glGetUniformLocation(resolveProg_, "u_grid"), len, L);
        glUniform2f(glGetUniformLocation(resolveProg_, "u_origin"), ox, oy);
        glUniform2f(glGetUniformLocation(resolveProg_, "u_step"), ux, uy);
        glUniform1i(glGetUniformLocation(resolveProg_, "u_dir"), k);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO_);
        glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    }

    // ---------- ④ 归一化 + 着色 ----------
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
    glUniform1f(glGetUniformLocation(accumProg_, "u_emit_scale"), emitScale);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO_);
    glBindImageTexture(0, outTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    return !checkGLError("dz/render");
}

void DzSweep::ensureBitmap(int len, int L, int dirs) {
    if (bitmapW_ == len && bitmapH_ == L && bitmapD_ == dirs && pixArray_) return;
    const GLuint old[3] = {pixArray_, flagArray_, lightTex_};
    for (GLuint x : old)
        if (x) glDeleteTextures(1, &x);
    pixArray_ = createTextureArray(GL_RGBA8, len, L, dirs, GL_RGBA, GL_UNSIGNED_BYTE);
    flagArray_ = createTextureArray(GL_RGBA8, len, L, dirs, GL_RGBA, GL_UNSIGNED_BYTE);
    lightTex_ = createTextureArray(GL_RGBA16F, len, L, dirs, GL_RGBA, GL_FLOAT);
    bitmapW_ = len;
    bitmapH_ = L;
    bitmapD_ = dirs;
    if (!pixArray_ || !flagArray_ || !lightTex_)
        fprintf(stderr, "[dz] d-z 位图创建失败 (%d x %d x %d)\n", len, L, dirs);
}
}  // namespace pb::gfx
