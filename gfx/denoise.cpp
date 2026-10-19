// gfx/denoise.cpp — à-trous 联合双边（3~5 层，5×5 抽头，空洞步长 1/2/4/8/16）
//
// 全部按**数组纹理的某一层**处理：这样输入可以是 fp32 累积缓冲（渐进哈希采样）、
// 输出是 fp16 的显示用纹理，中间乒乓用 fp16 数组纹理，最后一步
// glCopyImageSubData 拷进 2D 的 scanTex_（格式一致才能拷）。
#include "denoise.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "gl_texture.h"
#include "shader.h"

namespace pb::gfx {

static const char* kDenoiseComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_src;
uniform sampler2DArray u_guide;
uniform ivec2 u_cs;
uniform int u_layer;
uniform int u_step;
uniform float u_sigmaG;
uniform float u_cRange;    // σr = c·σ_est
uniform float u_invN;      // 1/已累积样本数
uniform float u_floor;     // σr 下限（样本极少时也给一点平滑）
layout(rgba16f,binding=0)writeonly uniform image2DArray u_dst;
const float kTap[5]=float[](1.0/16.0,4.0/16.0,6.0/16.0,4.0/16.0,1.0/16.0);
const vec3 kLum=vec3(0.2126,0.7152,0.0722);
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  vec4 s0=texelFetch(u_src,ivec3(p,u_layer),0);
  float l0=dot(s0.rgb,kLum);
  float var0=max(0.0,s0.a-l0*l0);            // E[l²] − E[l]²
  float sr=max(u_floor,u_cRange*sqrt(var0*u_invN));
  float inv2sr2=1.0/(2.0*sr*sr);
  float inv2sg2=1.0/(2.0*u_sigmaG*u_sigmaG);
  float g0=texelFetch(u_guide,ivec3(p,u_layer),0).a;
  vec3 acc=vec3(0.0);float accA=0.0;float wsum=0.0;
  for(int ty=-2;ty<=2;ty++){
    int qy=p.y+ty*u_step;if(qy<0||qy>=u_cs.y)continue;
    for(int tx=-2;tx<=2;tx++){
      int qx=p.x+tx*u_step;if(qx<0||qx>=u_cs.x)continue;
      ivec3 q=ivec3(qx,qy,u_layer);
      vec4 sq=texelFetch(u_src,q,0);
      float lq=dot(sq.rgb,kLum);
      float gq=texelFetch(u_guide,q,0).a;
      float dg=gq-g0,dl=lq-l0;
      float w=kTap[ty+2]*kTap[tx+2]*exp(-dg*dg*inv2sg2-dl*dl*inv2sr2);
      acc+=w*sq.rgb;accA+=w*sq.a;wsum+=w;
    }
  }
  // ⚠️ alpha（E[luma²]）必须**跟着一起滤波**：下一层的 σr 还要用它算方差。
  // 直接写 1.0 会让第 2 次迭代起 Var = 1 − luma² 变成垃圾 ⇒ σr 过大 ⇒ 糊成一片、
  // 彩色光源互相串色（这就是"颜色偏移"的另一个来源）。
  float a=(wsum>1e-8)?(accA/wsum):s0.a;
  imageStore(u_dst,ivec3(p,u_layer),vec4(wsum>1e-8?acc/wsum:s0.rgb,a));
})";

Denoise::~Denoise() { shutdown(); }

bool Denoise::init(int layers) {
    shutdown();
    layers_ = std::max(1, layers);
    prog_ = createComputeProgram("denoise/atrous", kDenoiseComp);
    if (!prog_) {
        fprintf(stderr, "[denoise] shader init failed\n");
        return false;
    }
    u_.src = glGetUniformLocation(prog_, "u_src");
    u_.guide = glGetUniformLocation(prog_, "u_guide");
    u_.cs = glGetUniformLocation(prog_, "u_cs");
    u_.layer = glGetUniformLocation(prog_, "u_layer");
    u_.step = glGetUniformLocation(prog_, "u_step");
    u_.sigmaG = glGetUniformLocation(prog_, "u_sigmaG");
    u_.cRange = glGetUniformLocation(prog_, "u_cRange");
    u_.invN = glGetUniformLocation(prog_, "u_invN");
    u_.floorSr = glGetUniformLocation(prog_, "u_floor");
    return !checkGLError("denoise/init");
}

void Denoise::shutdown() {
    if (prog_) glDeleteProgram(prog_);
    if (tmp_[0]) glDeleteTextures(1, &tmp_[0]);
    if (tmp_[1]) glDeleteTextures(1, &tmp_[1]);
    prog_ = 0;
    tmp_[0] = tmp_[1] = 0;
    w_ = h_ = 0;
}

void Denoise::ensureTemps(int w, int h) {
    if (tmp_[0] && w_ == w && h_ == h) return;
    if (tmp_[0]) glDeleteTextures(1, &tmp_[0]);
    if (tmp_[1]) glDeleteTextures(1, &tmp_[1]);
    tmp_[0] = createTextureArray(GL_RGBA16F, w, h, layers_, GL_RGBA, GL_HALF_FLOAT);
    tmp_[1] = createTextureArray(GL_RGBA16F, w, h, layers_, GL_RGBA, GL_HALF_FLOAT);
    w_ = w;
    h_ = h;
}

void Denoise::apply(GLuint src, GLuint guide, GLuint dst, int layer, int w, int h, int iters,
                    float sigmaG, float cRange, float invN) {
    if (!valid() || !src || !dst || !guide) return;
    iters = std::clamp(iters, 1, 5);
    ensureTemps(w, h);
    if (!tmp_[0] || !tmp_[1]) return;

    glUseProgram(prog_);
    glUniform2i(u_.cs, w, h);
    glUniform1i(u_.layer, layer);
    glUniform1f(u_.sigmaG, std::max(1e-3f, sigmaG));
    glUniform1f(u_.cRange, std::clamp(cRange, 0.5f, 8.0f));
    glUniform1f(u_.invN, std::max(1e-6f, invN));
    glUniform1f(u_.floorSr, 0.02f);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, guide);
    glUniform1i(u_.guide, 1);

    const GLuint gx = (GLuint)((w + 7) / 8), gy = (GLuint)((h + 7) / 8);
    GLuint cur = src;
    for (int it = 0; it < iters; it++) {
        const GLuint out = tmp_[it & 1];
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, cur);
        glUniform1i(u_.src, 0);
        glUniform1i(u_.step, 1 << it);
        glBindImageTexture(0, out, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute(gx, gy, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        cur = out;
    }

    // 最后一次写入 tmp_[(iters-1)&1]，拷进 dst 的同一层
    glCopyImageSubData(cur, GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, dst, GL_TEXTURE_2D_ARRAY, 0,
                       0, 0, layer, w, h, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glUseProgram(0);
}

}  // namespace pb::gfx
