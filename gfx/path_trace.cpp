// gfx/path_trace.cpp
//
// 每个像素每帧只投 1 条射线，靠时间累积收敛（见头文件里"怎么做到实时"的说明）。
// 表面交互按材质权重在三者之间**显式选一个**（而不是全部叠加），
// 这样每次弹射只走一条路径，每帧成本固定且与材质复杂度无关：
//   镜面反射 / 折射 / 漫反射
#include "path_trace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "gl_texture.h"
#include "material_pack.h"
#include "shader.h"

namespace pb::gfx {

// ---------------------------------------------------------------------------
// 追踪通道
// ---------------------------------------------------------------------------
static const char* kTraceComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_alb,u_mat;
uniform ivec2 u_cs;
uniform float u_emit;
uniform vec3 u_ambient;
uniform int u_steps,u_bounces;
uniform float u_base;                 // 表面基础可见度（风格化项，见头文件说明）
uniform uint u_frame;                 // 同时用作样本序号与随机种子
layout(rgba16f,binding=0)uniform image2D u_accum;

// PCG 风格整数哈希：便宜且分布够好
uint hashU(uint x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}

// 二维哈希：把 x / y / 帧号分别乘上互质的大常数再混。
//
// 一开始我用的是 hashU(px.x*1973 + px.y*9277 + frame*26699)——**这是个错误**：
// 对"线性组合"这种输入序列，单变量哈希的输出在相邻 x 之间仍有相关性，
// 表现为整片**规则竖条纹**（相邻列抽到的射线方向相似）。分开乘再异或就没事了。
uint hash2(uint x,uint y,uint s){
  uint h=x*0x8DA6B343u^y*0xD8163841u^s*0xCB1AB31Fu;
  h^=h>>15;h*=0x2C1B3C6Du;h^=h>>12;h*=0x297A2D39u;h^=h>>15;
  return h;
}
float rnd(inout uint s){s=hashU(s);return float(s&0xFFFFFFu)*(1.0/16777216.0);}

float coverageOf(ivec2 p){
  p=clamp(p,ivec2(0),u_cs-1);
  vec4 a=texelFetch(u_alb,p,0);
  vec4 m=texelFetch(u_mat,p,0);
  int fl=int(floor(m.a/8.0+0.5));
  return ((fl&2)!=0)?1.0:a.a;
}

// 用覆盖率的中心差分估计表面法线，结果指向**空气**一侧
vec2 normalOf(ivec2 p){
  float l=coverageOf(p+ivec2(-1,0));
  float r=coverageOf(p+ivec2( 1,0));
  float u=coverageOf(p+ivec2(0,-1));
  float d=coverageOf(p+ivec2(0, 1));
  vec2 g=vec2(r-l,d-u);
  float len=length(g);
  if(len<1e-5)return vec2(0.0,1.0);
  return -g/len;
}

void main(){
  ivec2 px=ivec2(gl_GlobalInvocationID.xy);
  if(px.x>=u_cs.x||px.y>=u_cs.y)return;

  uint seed=hash2(uint(px.x),uint(px.y),u_frame);

  // ---- 初级射线 ----
  //
  // 关键：**实体像素的射线不能从实体内部出发**。否则射线每一步都会撞回自己所在的格子，
  // 每撞一次就乘一次 albedo，几次弹射后能量趋近于 0 —— 实测表现为实心块全黑。
  // 正确做法是从该像素沿外法线站到空气一侧，余弦加权地朝空气那半边发射，
  // 收集到的入射辐照度再乘自己的 albedo（见循环之后的处理）。
  float cov0=coverageOf(px);
  bool primarySolid=(cov0>0.5);
  int fl0=int(floor(texelFetch(u_mat,px,0).a/8.0+0.5));
  bool primaryMirror=primarySolid&&((fl0&1)!=0);
  vec2 pos,dir;
  if(primarySolid){
    vec2 n0=normalOf(px);
    pos=vec2(px)+n0*1.1;
    if(primaryMirror){
      // 镜面像素：沿法线**锐利地**看出去，方向完全不抖动。
      //
      // 为什么必须这样：镜面属性如果只影响"别的射线弹到它"的时候，那么镜面像素自己
      // 看起来会和漫反射像素一模一样（当初就是这么写的，所以反射完全看不出来）。
      // 让初级射线沿法线单向看出去、并保持全强度（不乘 albedo），
      // 镜面像素才会明确显示它正对的场景 —— 这才是用户期待的"镜子"。
      dir=n0;
    }else{
      float t=asin(clamp(2.0*rnd(seed)-1.0,-1.0,1.0));
      float ct=cos(t),st=sin(t);
      dir=vec2(n0.x*ct-n0.y*st,n0.x*st+n0.y*ct);
    }
  }else{
    pos=vec2(px)+vec2(rnd(seed),rnd(seed));   // 亚像素抖动
    float ang=rnd(seed)*6.2831853;
    dir=vec2(cos(ang),sin(ang));
  }

  vec3 tp=vec3(1.0);      // throughput
  vec3 rad=vec3(0.0);

  for(int b=0;b<u_bounces;b++){
    bool hit=false;ivec2 hp=ivec2(0);

    // ---- DDA 网格遍历：沿途累积自发光、穿过半透明介质 ----
    //
    // 这里必须用 DDA（逐格跨越边界），**不能**用 pos += dir 那种固定步长前进。
    // 固定步长 1 格时，方向接近对角线的射线会从 (x,y) 直接跳到 (x+1,y+1)，
    // 把 (x+1,y) 与 (x,y+1) 两个格子整个跳过 —— 表现为：
    //   * 细的发光线条经常被整条漏掉 -> 光"很粗糙"、缺斤少两
    //   * 每条射线只采到一半信息 -> 方差大 -> 收敛极慢
    //   * 不同方向漏的格子不同 -> 残留方向性结构
    // DDA 保证射线经过的每个格子恰好被采样一次，且 dt 就是射线在该格内的长度，
    // 于是体积自发光的积分权重不再需要拍一个magic系数。
    vec2 cellF=floor(pos);
    ivec2 cell=ivec2(cellF);
    ivec2 stp=ivec2(dir.x>=0.0?1:-1,dir.y>=0.0?1:-1);
    vec2 tDelta=vec2(1.0/max(abs(dir.x),1e-8),1.0/max(abs(dir.y),1e-8));
    vec2 tMax;
    tMax.x=(dir.x>=0.0?(cellF.x+1.0-pos.x):(pos.x-cellF.x))*tDelta.x;
    tMax.y=(dir.y>=0.0?(cellF.y+1.0-pos.y):(pos.y-cellF.y))*tDelta.y;
    float tPrev=0.0;
    bool escaped=false;

    for(int s=0;s<u_steps;s++){
      if(cell.x<0||cell.y<0||cell.x>=u_cs.x||cell.y>=u_cs.y){escaped=true;break;}
      float tNext=min(tMax.x,tMax.y);
      float dt=max(tNext-tPrev,1e-4);
      tPrev=tNext;

      vec4 a=texelFetch(u_alb,cell,0);
      vec4 m=texelFetch(u_mat,cell,0);
      int fl=int(floor(m.a/8.0+0.5));
      float cov=((fl&2)!=0)?1.0:a.a;

      // 体积自发光：**独立的 RGB**，按射线在该格内的长度加权
      rad+=tp*m.rgb*u_emit*dt;

      if(cov>0.5){hit=true;hp=cell;break;}
      // 半透明介质：衰减 + 按 albedo 染色（彩色玻璃）
      tp*=mix(vec3(1.0),a.rgb,cov)*(1.0-cov);
      if(max(max(tp.r,tp.g),tp.b)<0.004)break;

      if(tMax.x<tMax.y){tMax.x+=tDelta.x;cell.x+=stp.x;}
      else{tMax.y+=tDelta.y;cell.y+=stp.y;}
    }
    // 逃出画布、能量耗尽、或步数用尽：都按"看到了环境"处理。
    // 步数用尽也算逃逸，否则远处会因为截断而出现黑斑。
    if(!hit||escaped||max(max(tp.r,tp.g),tp.b)<0.004){
      rad+=tp*u_ambient;
      break;
    }

    // ---- 表面交互 ----
    vec4 a=texelFetch(u_alb,hp,0);
    vec4 m=texelFetch(u_mat,hp,0);
    float aFl=floor(m.a/8.0+0.5);
    int fl=int(aFl);
    float iorv=m.a-aFl*8.0;          // 折射率就存在材质纹理的第 4 个通道里
    float wSpec=((fl&1)!=0)?1.0:0.0;
    float wRefr=(iorv>1.01)?0.85:0.0;
    float wDiff=1.0;
    float sum=wSpec+wRefr+wDiff;

    vec2 n=normalOf(hp);
    if(dot(n,dir)>0.0)n=-n;       // 法线必须迎着入射线

    float pick=rnd(seed)*sum;
    vec2 nd;
    if(pick<wSpec){
      nd=reflect(dir,n);          // 镜面反射
      tp*=a.rgb;
      pos=vec2(hp)+n*1.0+nd*0.5;
    }else if(pick<wSpec+wRefr){
      // 折射（Snell）；全内反射时退回镜面反射
      float cosi=clamp(-dot(dir,n),0.0,1.0);
      float eta=1.0/iorv;
      float k=1.0-eta*eta*(1.0-cosi*cosi);
      nd=(k<0.0)?reflect(dir,n):normalize(eta*dir+(eta*cosi-sqrt(k))*n);
      tp*=a.rgb;
      pos=vec2(hp)+nd*1.0;
    }else{
      // 漫反射：余弦加权采样（2D 半平面内 theta = asin(2u-1)）。
      // 这样采样的 Lambert 估计量恰好就是 albedo，不必再除 pdf。
      float t=asin(clamp(2.0*rnd(seed)-1.0,-1.0,1.0));
      float ct=cos(t),st=sin(t);
      nd=vec2(n.x*ct-n.y*st,n.x*st+n.y*ct);
      tp*=a.rgb;
      pos=vec2(hp)+n*1.0+nd*0.5;
    }
    dir=nd;
  }

  // ---- 实体像素：所见 = 自发光 + albedo × (基础可见度 + 收集到的入射光) ----
  //
  // rad 此时是"从空气半球收集到的入射辐射亮度"（逐通道的颜色），直接乘 albedo 即可。
  // u_base 是刻意加的风格化基础项，保证"画什么颜色就看得见什么颜色"（见头文件说明）。
  if(primarySolid){
    vec4 ap=texelFetch(u_alb,px,0);
    vec4 mp=texelFetch(u_mat,px,0);
    vec3 selfEm=mp.rgb*u_emit;       // 自发光是独立的 RGB，与 albedo 无关
    if(primaryMirror){
      // 镜面保持全强度：乘 albedo 会让它和普通表面难以区分（当初就是这个原因
      // 让"反射看不见"）。这样镜面明显更亮、且显示的是正对的场景。
      rad=selfEm+rad;
    }else{
      rad=selfEm+ap.rgb*(u_base+rad);
    }
  }

  // ---- 时间累积：滑动平均。第 n+1 个样本的权重是 1/(n+1) ----
  vec3 old=imageLoad(u_accum,px).rgb;
  imageStore(u_accum,px,vec4(mix(old,rad,1.0/(float(u_frame)+1.0)),1.0));
})";

// ---------------------------------------------------------------------------
// 清零通道
// ---------------------------------------------------------------------------
//
// 累积缓冲是 RGBA16F，重置时必须真的清零，否则上一轮的样本会残留成拖影。
// 注意：glClearTexImage 是 GL 4.4 才有的，本项目上下文是 4.3，所以自己写一个
// compute 清零通道。（这个 shader 与我在 P0 从原实现里当作死代码删掉的那个
// clearComp 是同一件事——当时它确实没被用到，现在路径追踪真的需要它了。）
static const char* kClearComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
layout(rgba16f,binding=0)uniform image2D u_img;
uniform ivec2 u_cs;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x<u_cs.x&&p.y<u_cs.y)imageStore(u_img,p,vec4(0));
})";

// ---------------------------------------------------------------------------
// 解析通道（曝光 + 色调映射）
// ---------------------------------------------------------------------------
//
// 累积值可能远超 1（多次弹射 + 强自发光），直接截断会丢掉高光，所以默认走 Reinhard。
static const char* kResolveComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_src;
uniform ivec2 u_cs;
uniform float u_exposure;
uniform int u_tonemap;
layout(rgba8,binding=0)writeonly uniform image2D u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  vec3 c=texelFetch(u_src,p,0).rgb*u_exposure;
  if(u_tonemap!=0)c=c/(1.0+c);
  imageStore(u_out,p,vec4(clamp(c,0.0,1.0),1.0));
})";

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

PathTrace::~PathTrace() { shutdown(); }

bool PathTrace::init(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    shutdown();
    w_ = w;
    h_ = h;

    albedoTex_ = createTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    // 材质用 RGBA32F：解耦后需要 5 个量（自发光 RGB + 折射率 + 标志位）
    matTex_ = createTexture(GL_RGBA32F, w, h, GL_RGBA, GL_FLOAT);
    accumTex_ = createTexture(GL_RGBA16F, w, h, GL_RGBA, GL_HALF_FLOAT);
    displayTex_ = createTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);

    // 清零：调用方忘了 uploadAll 时也应是干净黑场而不是未定义内容
    {
        std::vector<RGBA8> zero((size_t)w * h, RGBA8{0, 0, 0, 0});
        uploadSubImage(albedoTex_, zero.data(), w, h, Rect{0, 0, w - 1, h - 1});
        std::vector<float> zf((size_t)w * h * 4, 0.0f);
        uploadSubImagePackedF(matTex_, 0, 0, w, h, zf.data());
    }

    traceProg_ = createComputeProgram("pt/trace", kTraceComp);
    resolveProg_ = createComputeProgram("pt/resolve", kResolveComp);
    clearProg_ = createComputeProgram("pt/clear", kClearComp);
    if (!traceProg_ || !resolveProg_ || !clearProg_) {
        fprintf(stderr, "[pt] shader initialization failed\n");
        shutdown();
        return false;
    }

    uTrace_.alb = glGetUniformLocation(traceProg_, "u_alb");
    uTrace_.mat = glGetUniformLocation(traceProg_, "u_mat");
    uTrace_.cs = glGetUniformLocation(traceProg_, "u_cs");
    uTrace_.emit = glGetUniformLocation(traceProg_, "u_emit");
    uTrace_.amb = glGetUniformLocation(traceProg_, "u_ambient");
    uTrace_.steps = glGetUniformLocation(traceProg_, "u_steps");
    uTrace_.bounces = glGetUniformLocation(traceProg_, "u_bounces");
    uTrace_.frame = glGetUniformLocation(traceProg_, "u_frame");
    uTrace_.base = glGetUniformLocation(traceProg_, "u_base");
    uResolve_.src = glGetUniformLocation(resolveProg_, "u_src");
    uResolve_.cs = glGetUniformLocation(resolveProg_, "u_cs");
    uResolve_.exposure = glGetUniformLocation(resolveProg_, "u_exposure");
    uResolve_.tonemap = glGetUniformLocation(resolveProg_, "u_tonemap");
    uClear_.cs = glGetUniformLocation(clearProg_, "u_cs");

    resetAccumulation();
    return !checkGLError("pt/init");
}

void PathTrace::shutdown() {
    if (albedoTex_) glDeleteTextures(1, &albedoTex_);
    if (matTex_) glDeleteTextures(1, &matTex_);
    if (accumTex_) glDeleteTextures(1, &accumTex_);
    if (displayTex_) glDeleteTextures(1, &displayTex_);
    if (traceProg_) glDeleteProgram(traceProg_);
    if (resolveProg_) glDeleteProgram(resolveProg_);
    if (clearProg_) glDeleteProgram(clearProg_);
    albedoTex_ = matTex_ = accumTex_ = displayTex_ = 0;
    traceProg_ = resolveProg_ = clearProg_ = 0;
    w_ = h_ = 0;
    sampleIndex_ = 0;
    needsClear_ = true;
}

void PathTrace::resetAccumulation() {
    sampleIndex_ = 0;
    needsClear_ = true;   // GPU 操作留到 render() 里做
}

// ---------------------------------------------------------------------------
// 上传
// ---------------------------------------------------------------------------

void PathTrace::upload(const Image& albedo, const Material* mats, const Rect& dirty) {
    if (!albedoTex_ || !matTex_) return;
    if (albedo.width() != w_ || albedo.height() != h_) return;

    Rect r = dirty;
    r.clipTo(w_, h_);
    if (r.empty()) return;

    uploadSubImage(albedoTex_, albedo.data(), w_, h_, r);

    if (mats) {
        static std::vector<float> pack;
        packMaterials(mats, w_, r, pack);
        if (!pack.empty())
            uploadSubImagePackedF(matTex_, r.x0, r.y0, r.width(), r.height(), pack.data());
    }
}

void PathTrace::uploadAll(const Image& albedo, const Material* mats) {
    upload(albedo, mats, Rect{0, 0, w_ - 1, h_ - 1});
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

void PathTrace::render() {
    if (!valid()) return;
    if (sampleIndex_ >= kConvergedSamples) return;   // 收敛后不再浪费 GPU

    // --- 0. 累积缓冲清零（只在画布变了/刚切换模式时需要）---
    if (needsClear_) {
        glUseProgram(clearProg_);
        glUniform2i(uClear_.cs, w_, h_);
        glBindImageTexture(0, accumTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        needsClear_ = false;
    }

    // --- 1. 追踪 + 累积（每帧可跑多次，用相互独立的种子） ---
    glUseProgram(traceProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, albedoTex_);
    glUniform1i(uTrace_.alb, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, matTex_);
    glUniform1i(uTrace_.mat, 1);
    glUniform2i(uTrace_.cs, w_, h_);
    glUniform1f(uTrace_.emit, st_.emitScale);
    glUniform3fv(uTrace_.amb, 1, st_.ambient);
    glUniform1i(uTrace_.steps, std::max(1, st_.maxSteps));
    glUniform1i(uTrace_.bounces, std::max(1, st_.maxBounces));
    glUniform1f(uTrace_.base, st_.albedoBase);
    glBindImageTexture(0, accumTex_, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);

    // 每帧跑 spp 次，每次用不同的样本序号（= 不同的随机种子）。
    // 噪声按 1/sqrt(N) 下降，而单帧成本只有 2-3 ms，余量很大，用采样换画质是划算的。
    const int spp = std::clamp(st_.samplesPerFrame, 1, 32);
    for (int i = 0; i < spp && sampleIndex_ < kConvergedSamples; i++) {
        glUniform1ui(uTrace_.frame, (GLuint)sampleIndex_);
        glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        sampleIndex_++;
    }

    // --- 2. 解析到显示纹理 ---
    glUseProgram(resolveProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, accumTex_);
    glUniform1i(uResolve_.src, 0);
    glUniform2i(uResolve_.cs, w_, h_);
    glUniform1f(uResolve_.exposure, st_.exposure);
    glUniform1i(uResolve_.tonemap, st_.tonemap ? 1 : 0);
    glBindImageTexture(0, displayTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    glUseProgram(0);
}

}  // namespace pb::gfx
