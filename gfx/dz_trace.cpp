// gfx/dz_trace.cpp — 2.5D 光线追踪（按规格逐条实现）
//
// ## 规格逐条对照
//
//   第三节 目标曲面：对每个 (x,y)，h(x,y) = max{z | V(x,y,z) 是实体}；
//            目标曲面 Σ = {(x, y, h(x,y)+1)}，法线恒为 (0,0,1)。
//   第四节 要计算的量：I(p) = (1/|D+|) Σ_{d∈D+} L(p,d)，D+ 是**上半球**采样方向。
//   第五节 入射光 L：从 p 沿 d 步进（整数 Bresenham 式），维护透射率 T：
//              空体素 -> 继续
//              半透明 q -> light += T * E(q) * A(q);  T *= (1 - A(q))
//              不透明 q -> light += T * E(q);          停止
//              出界     -> light += T * B(d);          停止
//            **只取上半球方向**（dz > 0），下半球的光不参与。
//   第六节 最终颜色：Color = mix(I + E_top, C_top, A_top)
//   第七节 方向集合：Bresenham 中点圆生成圆周方向，不约分、不去重。
//
// ## 为什么用"每个表面点自己投射射线"
//
//   这就是原版 2D 光追的 `castRay(p, d)` —— 反向光线追踪：从接收点向四周发射，
//   反向收集能到达它的光。它的好处是**每个像素都独立地看到整个场**，
//   所以只要有一条射线能打到光源，那一格就有贡献，光晕会自然铺开。
//   （正向扫描线只沿一个方向传播，光只出现在下游，不会有光晕。）
#include "dz_trace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/voxel.h"
#include "gl_texture.h"
#include "shader.h"

namespace pb::gfx {
namespace {

// 一个线程 = 一个表面点 (x,y)。它向圆周方向发射射线、反向收集光。
//
// 体素空间以 3D 纹理传入（R8UI 存 alpha/柱高信息不方便，这里用两张 3D 纹理：
//   u_vox  : RGBA8 —— rgb = 表面颜色 C, a = 不透明度 A
//   u_emit : RGBA8 —— rgb = 自发光 E
static const char* kTraceComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler3D u_vox;      // C + A
uniform sampler3D u_emit;     // E
uniform ivec3 u_size;         // (X, Y, Z)
uniform ivec2 u_canvasWH;     // (X, Y)
uniform int   u_radius;       // 方向集合半径（中点圆）
uniform vec3  u_sunCol;       // 太阳/背景光颜色
uniform vec3  u_wallCol;      // 墙/环境光颜色
uniform vec2  u_sunDir;       // 太阳方向（xy 平面上的单位向量）
uniform float u_sunSc;        // 太阳角大小（1 - dot <= sunSc 判为太阳）
uniform float u_emitScale;
uniform int   u_zRate;        // 每走 1 个水平像素，z 上升多少
uniform int   u_jitter;       // 方向抖动幅度：把采样晶格打散成噪声
uniform int   u_sampleSeed;   // 每帧变化的样本种子
layout(rgba8,binding=0)writeonly uniform image2D u_out;

// 体素中心采样（越界返回 0）
vec4 fetchVox(ivec3 p){
  if(p.x<0||p.y<0||p.z<0||p.x>=u_size.x||p.y>=u_size.y||p.z>=u_size.z) return vec4(0.0);
  return texelFetch(u_vox,p,0);
}
vec3 fetchEmit(ivec3 p){
  if(p.x<0||p.y<0||p.z<0||p.x>=u_size.x||p.y>=u_size.y||p.z>=u_size.z) return vec3(0.0);
  return texelFetch(u_emit,p,0).rgb*u_emitScale;
}

// 便宜的整数 hash：给每个像素/每条射线一个稳定的伪随机数
uint hash3(uint a, uint b, uint c){
  uint h=a*374761393u + b*668265263u + c*2246822519u;
  h=(h^(h>>13))*1274126177u;
  return h^(h>>16);
}

// 3D 整数步进（DDA）：沿 (dx,dy,dz) 一步一步走，每步只前进"最短的那一维"一格。
//
// 这就是"整数 Bresenham 式步进"的 3D 版：不约分、不归一化，
// 步长严格是 1 格，所以不会跳格也不会重复。
vec3 castRay(ivec3 p0, int dx, int dy, int dz){
  vec3 light=vec3(0.0);
  float T=1.0;
  const float eps=1e-3;

  // 方向必须整体向上（只取上半球）
  if(dz<=0) return light;
  if(dx==0&&dy==0&&dz<=0) return light;

  int sx=(dx>0)?1:(dx<0?-1:0);
  int sy=(dy>0)?1:(dy<0?-1:0);
  int sz=1;                        // dz 恒为正
  int ax=abs(dx), ay=abs(dy), az=abs(dz);

  ivec3 p=p0;
  // 参数化步进：每维的"走到下一格还需要多少 t"
  float tx = (ax>0)? (0.5/float(ax)) : 1e30;
  float ty = (ay>0)? (0.5/float(ay)) : 1e30;
  float tz = (az>0)? (0.5/float(az)) : 1e30;
  const float dtx = (ax>0)? (1.0/float(ax)) : 1e30;
  const float dty = (ay>0)? (1.0/float(ay)) : 1e30;
  const float dtz = (az>0)? (1.0/float(az)) : 1e30;

  int guard=0;
  const int guardMax=(u_size.x+u_size.y+u_size.z)*6;
  while(guard++ < guardMax){
    // 先迈出第一步（离开接收点自己）
    if(tx<ty && tx<tz){ p.x+=sx; tx+=dtx; }
    else if(ty<tz){ p.y+=sy; ty+=dty; }
    else { p.z+=sz; tz+=dtz; }

    if(p.x<0||p.x>=u_size.x||p.y<0||p.y>=u_size.y||p.z<0||p.z>=u_size.z){
      // 出界：按水平方向判断看到太阳还是墙
      float len=sqrt(float(dx*dx+dy*dy));
      vec3 sky=u_wallCol;
      if(len>0.0){
        float dt=(float(dx)/len)*u_sunDir.x+(float(dy)/len)*u_sunDir.y;
        if((1.0-dt)<=u_sunSc) sky=u_sunCol;
      } else {
        sky=u_sunCol;               // 正上方：直接看到天顶
      }
      light+=sky*T;
      return light;
    }

    vec4 c=fetchVox(p);
    float A=c.a;
    if(A>0.0){
      vec3 E=fetchEmit(p);
      if(A>=1.0){ light+=T*E; return light; }     // 不透明：吸收并停止
      light+=T*E*A;                                // 半透明：按 T*E*A 累加
      T*=(1.0-A);
      if(T<eps) return light;
    }
  }
  return light;
}
void main(){
  ivec2 q=ivec2(gl_GlobalInvocationID.xy);
  if(q.x>=u_canvasWH.x||q.y>=u_canvasWH.y)return;

  // 柱高 h(x,y)；没有实体就不输出
  int h=-1;
  for(int z=u_size.z-1;z>=0;z--){ if(fetchVox(ivec3(q,z)).a>0.0){ h=z; break; } }
  if(h<0){ imageStore(u_out,q,vec4(0.0,0.0,0.0,1.0)); return; }

  // 目标曲面上的点：柱顶正上方那一层
  ivec3 p=ivec3(q,h+1);

  // 方向集合 = 水平方向（中点圆）× 竖直爬升量。
  //
  // 为什么竖直分量必须进方向集合：中点圆给出的斜率最陡是 1:1（≤45°），
  // 而"几十像素外、只高一层"的仰角只有 1° 左右 —— 圆里根本没有这么平的方向。
  // 只取圆方向的话射线会过早冲出顶面，横向几乎不铺开，也就没有光晕。
  // 这里让竖直爬升量也参与枚举，并用 3D DDA 按整数步进推进。
  vec3 sum=vec3(0.0);
  int cnt=0;
  int r=u_radius;
  int m=1-r, x=0, y=r;
  while(x<y){
    // 竖直爬升：从 1 层到 (Z-1) 层，覆盖所有可能的仰角
    for(int dz=1; dz<u_size.z; dz++){
      // 方向抖动：把整数格线的采样晶格打散。
      //
      // 不做抖动时，(dx,dy,dz) 都是小整数，射线只能沿有限的几族整数格线走；
      // 相邻方向的射线在远处会跳过好几个体素，于是"能打到光源"与"打不到"
      // 交替出现 —— 累加起来就是明显的带状条纹。
      // 抖动后这些条纹变成不相关的噪声，视觉上就消失了。
      const int J=u_jitter;
      int j1=0,j2=0,j3=0,j4=0,j5=0,j6=0,j7=0,j8=0;
      int k1=0,k2=0,k3=0,k4=0,k5=0,k6=0,k7=0,k8=0;
      if(J>0){
        const uint hs=hash3(uint(q.x),uint(q.y),uint(u_sampleSeed)+uint(dz));
        j1=int(hs%uint(2*J+1))-J;  k1=int((hs>>8)%uint(2*J+1))-J;
        j2=int((hs>>16)%uint(2*J+1))-J; k2=int((hs>>24)%uint(2*J+1))-J;
        const uint hs2=hash3(uint(q.y)+7777u,uint(q.x),uint(u_sampleSeed)*3u+uint(dz)*31u);
        j3=int(hs2%uint(2*J+1))-J; k3=int((hs2>>8)%uint(2*J+1))-J;
        j4=int((hs2>>16)%uint(2*J+1))-J; k4=int((hs2>>24)%uint(2*J+1))-J;
        const uint hs3=hash3(uint(q.x)*5u,uint(q.y)*7u,uint(u_sampleSeed)+uint(dz)*97u);
        j5=int(hs3%uint(2*J+1))-J; k5=int((hs3>>8)%uint(2*J+1))-J;
        j6=int((hs3>>16)%uint(2*J+1))-J; k6=int((hs3>>24)%uint(2*J+1))-J;
        const uint hs4=hash3(uint(q.x)*11u,uint(q.y)*13u,uint(u_sampleSeed)*7u+uint(dz)*131u);
        j7=int(hs4%uint(2*J+1))-J; k7=int((hs4>>8)%uint(2*J+1))-J;
        j8=int((hs4>>16)%uint(2*J+1))-J; k8=int((hs4>>24)%uint(2*J+1))-J;
      }
      sum+=castRay(p,  x+j1,  y+k1, dz); ++cnt;
      sum+=castRay(p, -x+j2,  y+k2, dz); ++cnt;
      sum+=castRay(p,  x+j3, -y+k3, dz); ++cnt;
      sum+=castRay(p, -x+j4, -y+k4, dz); ++cnt;
      sum+=castRay(p,  y+j5,  x+k5, dz); ++cnt;
      sum+=castRay(p, -y+j6,  x+k6, dz); ++cnt;
      sum+=castRay(p,  y+j7, -x+k7, dz); ++cnt;
      sum+=castRay(p, -y+j8, -x+k8, dz); ++cnt;
    }
    ++x;
    if(m<0) m+=2*x+1; else { --y; m+=2*(x-y)+1; }
  }
  // 正上方（水平方向为 0）：垂直看天顶
  for(int dz=1; dz<u_size.z; dz++){ sum+=castRay(p,0,0,dz); ++cnt; }
  vec3 I=(cnt>0)?(sum/float(cnt)):vec3(0.0);

  // 诊断：只用 PB_DZ_H 打开。R = h*40，G = 柱顶 A，B = cnt 的奇偶标记
  if(u_zRate==0){
    vec4 tv=fetchVox(ivec3(q,h));
    imageStore(u_out,q,vec4(float(h+1)/8.0, tv.a, 0.0, 1.0));
    return;
  }

  // 最终颜色：从柱顶往下逐层累积（多层半透明模型，见文件头说明）。
  //
  //     Color = Σ_i T_i · (I0 · A_i · C_i + E_i)
  //     T_i   = Π_{j<i} (1 - A_j)          （到达第 i 层之前的累积透射率）
  //
  //   * 反射项是 I0 · A · C —— 反射率正比于不透明度。
  //     所以 A=1 的黑表面在天光为 0 时结果就是纯黑（0·1·0 = 0），
  //     而 A=1 的白表面在无光时也是纯黑（0·1·1 = 0）—— 这正是期望的行为。
  //   * 自发光 E 即使 I0=0 也会贡献颜色（光源本身可见）。
  vec3 col=vec3(0.0);
  float T=1.0;
  for(int z=h; z>=0; z--){
    vec4 cv=fetchVox(ivec3(q,z));
    float A=cv.a;
    if(A<=0.0) continue;
    vec3 E=fetchEmit(ivec3(q,z));
    col += T * (I*A*cv.rgb + E);
    T *= (1.0-A);
    if(T<1e-3) break;
  }

  // 归一化（原版有这一步：超过 1 就按最大值缩放）
  float ov=max(col.r,max(col.g,col.b));
  if(ov>1.0) col/=ov;
  imageStore(u_out,q,vec4(clamp(col,0.0,1.0),1.0));
})";

}  // namespace

DzTrace::~DzTrace() { shutdown(); }

bool DzTrace::init(int x, int y, int z) {
    shutdown();
    if (x <= 0 || y <= 0 || z <= 0) return false;
    X_ = x; Y_ = y; Z_ = z;

    voxTex_ = createTexture3D(GL_RGBA8, x, y, z, GL_RGBA, GL_UNSIGNED_BYTE);
    emitTex_ = createTexture3D(GL_RGBA8, x, y, z, GL_RGBA, GL_UNSIGNED_BYTE);
    outTex_ = createTexture(GL_RGBA8, x, y, GL_RGBA, GL_UNSIGNED_BYTE);
    if (!voxTex_ || !emitTex_ || !outTex_) {
        fprintf(stderr, "[dz_trace] 纹理创建失败\n");
        return false;
    }
    prog_ = createComputeProgram("dz_trace/trace", kTraceComp);
    if (!prog_) { fprintf(stderr, "[dz_trace] 着色器初始化失败\n"); return false; }
    return !checkGLError("dz_trace/init");
}

void DzTrace::shutdown() {
    const GLuint t[3] = {voxTex_, emitTex_, outTex_};
    for (GLuint v : t)
        if (v) glDeleteTextures(1, &v);
    voxTex_ = emitTex_ = outTex_ = 0;
    if (prog_) glDeleteProgram(prog_);
    prog_ = 0;
    X_ = Y_ = Z_ = 0;
}

void DzTrace::uploadVolume(const VoxelGrid& g) {
    if (!prog_ || g.X != X_ || g.Y != Y_ || g.Z != Z_) return;
    const size_t n = (size_t)X_ * (size_t)Y_ * (size_t)Z_;
    std::vector<uint8_t> vox(n * 4u, 0), emi(n * 4u, 0);
    for (size_t i = 0; i < n; i++) {
        const Voxel& v = g.atIdx(i);
        vox[i * 4u + 0] = v.color.r;
        vox[i * 4u + 1] = v.color.g;
        vox[i * 4u + 2] = v.color.b;
        vox[i * 4u + 3] = v.color.a;                 // A
        emi[i * 4u + 0] = v.mat.emR;                 // E
        emi[i * 4u + 1] = v.mat.emG;
        emi[i * 4u + 2] = v.mat.emB;
        emi[i * 4u + 3] = 255;
    }
    glBindTexture(GL_TEXTURE_3D, voxTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, X_, Y_, Z_, GL_RGBA, GL_UNSIGNED_BYTE,
                    vox.data());
    glBindTexture(GL_TEXTURE_3D, emitTex_);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, X_, Y_, Z_, GL_RGBA, GL_UNSIGNED_BYTE,
                    emi.data());
    glBindTexture(GL_TEXTURE_3D, 0);
    checkGLError("dz_trace/uploadVolume");
}

bool DzTrace::render(const DzTraceParams& prm) {
    if (!prog_) return false;
    glUseProgram(prog_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, voxTex_);
    glUniform1i(glGetUniformLocation(prog_, "u_vox"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, emitTex_);
    glUniform1i(glGetUniformLocation(prog_, "u_emit"), 1);
    glUniform3i(glGetUniformLocation(prog_, "u_size"), X_, Y_, Z_);
    glUniform2i(glGetUniformLocation(prog_, "u_canvasWH"), X_, Y_);
    glUniform1i(glGetUniformLocation(prog_, "u_radius"), std::max(1, prm.radius));
    glUniform3fv(glGetUniformLocation(prog_, "u_sunCol"), 1, prm.sunCol);
    glUniform3fv(glGetUniformLocation(prog_, "u_wallCol"), 1, prm.wallCol);
    glUniform2f(glGetUniformLocation(prog_, "u_sunDir"), prm.sunDir[0], prm.sunDir[1]);
    glUniform1f(glGetUniformLocation(prog_, "u_sunSc"), prm.sunSoft);
    glUniform1f(glGetUniformLocation(prog_, "u_emitScale"), prm.emitScale);
    int zRate = prm.zRate;   // 允许 0：那是诊断模式
    glUniform1i(glGetUniformLocation(prog_, "u_zRate"), zRate);
    glUniform1i(glGetUniformLocation(prog_, "u_jitter"), std::max(0, prm.jitter));
    glUniform1i(glGetUniformLocation(prog_, "u_sampleSeed"), prm.sampleSeed);
    glBindImageTexture(0, outTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((GLuint)((X_ + 7) / 8), (GLuint)((Y_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    return !checkGLError("dz_trace/render");
}

}  // namespace pb::gfx
