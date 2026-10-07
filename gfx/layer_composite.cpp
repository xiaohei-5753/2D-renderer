// gfx/layer_composite.cpp
//
// 一个 compute pass，从最底层往上叠。
//
// ## 只有两件事要做
//
//   对每一层：  acc = mix(acc, layer.rgb, layerOpacity)
//
// 就这么简单 —— 但我的第一版把它写复杂了，然后连着栽了三次，所以这里解释清楚
// **为什么**它可以这么简单。
//
// 合成一层看似需要两个量：
//   1. 这一层在这里有没有内容（没有就看下面那层）
//   2. 半透明像素要透出下面多少
//
// ### 「受光结果」这两个量都不需要
//
// 逐层照亮之后，每一层的结果已经是**这一层最终的画面**：
//   * 空气像素带着这一层贡献的**光晕**（纯黑的层也要显示它的黑）
//   * 半透明像素的透光与自发光已经在光照那一步算完了
//   * 实心像素就是要盖住下面
// 也就是说它已经"所见即所得"了，合成唯一要做的就是按**图层不透明度**叠上去。
// 再拿它的 alpha 混一次，就是把光照结果又衰减一遍（实测：一条 alpha=110 的霓虹线
// 显示成一半亮度；纯黑的光晕层干脆整片消失）。
//
// 我先前把 alpha 存成"可见度 1-cov"并试图让它同时承担"参与度"，这是错的：
// 一个纯黑图层（cov=0，可见度 1）与一个实心图层（cov=1，可见度 0）**都应当
// 100% 显示自己的颜色**。两者的区别不在"显示多少"，而在**下面那层收到多少光** ——
// 而那是 core/composite.h 的 occlAbove 通道负责的事，与这里无关。
//
// ### 「原始 albedo」（关灯看原图）需要的是另一套
//
// 它没有经过光照，alpha 是"这个像素有多不透明"：
//   * 空气(alpha=0) 是空的，露出下面那层（不是"以完整颜色显示"）
//   * 半透明像素要**以完整颜色显示**（画上去什么颜色就是什么颜色），
//     只透出下面一部分 —— 而不是把自己的颜色乘暗
// 所以对原始 albedo：acc = mix(c.rgb, acc, c.a * layerOpacity)，
// 且只在 alpha > 0 时才参与（alpha=0 表示这一层这里没内容）。
#include "layer_composite.h"

#include <cstdio>

#include "gl_texture.h"
#include "shader.h"

namespace pb::gfx {

static const char* kCompositeComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_tex;      // 每层的颜色（受光结果或原始 albedo）
uniform sampler2DArray u_opacity;  // 每层：一个 float 的图层不透明度
uniform ivec2 u_cs;
uniform int u_n;                   // 参与合成的层数
uniform int u_albedoAsOpaque;      // 1 = 原始 albedo（alpha 是"有多不透明"）
uniform vec3 u_backdrop;
uniform sampler2DArray u_field;       // 每层的"光场"：这一层平面内有多少光
uniform sampler2DArray u_albedo;      // 每层的原始颜色（受光就是它乘上光）
uniform float u_fieldStrength;        // 0 = 关掉这个模型           // 所有层都没盖住时显示什么
layout(rgba8,binding=0)writeonly uniform image2D u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;

  vec3 acc=u_backdrop;
  // ---- 光场模型（用户给的规格）----
  //
  //   L_i = 第 i 层的光场：只含光、**不含原始像素**（就是 2D 光追图里空气像素
  //         的颜色），并把环境光减掉
  //   自下而上 F_i = Σ_{k>=i} L_k     "Ln 与 Ln-1 相加得到更下层的光场"
  //   C_i = A_i × F_i                  "A_{n-1} × Ln"
  //   result = C_i 按透明度覆盖 result  "通过透明度覆盖或半覆盖"
  //
  // 环境光：每层的光场里都含了一份环境光，n 层相加就会过曝 n 倍。
  // 所以这里从每层光场里减掉环境光，最后只在 C_i 里加一次
  // 死区 0.006：没有发光内容的层，其空气色就是环境光本身，减去后只剩浮点残渣；
  // 层数一多，这些残渣累加起来就表现为"每新建一层环境光更亮一点"。
  // （等价于你说的"每层环境光 1/n"，但不会过曝）。
  vec3 gtotal=vec3(0.0);
  if(u_fieldStrength>0.0){
    for(int i=0;i<u_n;i++){
      vec4 L=texelFetch(u_tex,ivec3(p,i),0);
      gtotal+=max(vec3(0.0),L.rgb-u_backdrop-vec3(0.006))*(1.0-clamp(L.a,0.0,1.0));
    }
  }
  vec3 Ftot=gtotal;
  for(int i=0;i<u_n;i++){
    vec4 c=texelFetch(u_tex,ivec3(p,i),0);
    float op=texelFetch(u_opacity,ivec3(p,i),0).r;
    if(u_fieldStrength>0.0){
      // 这一层的颜色 = 原始颜色 A_i × 累计光场 F_i
      vec3 A=texelFetch(u_albedo,ivec3(p,i),0).rgb;
      vec3 C=A*(Ftot*u_fieldStrength+u_backdrop);
      float a=clamp(c.a,0.0,1.0)*op;
      acc=mix(acc,C,a);                       // 覆盖 / 半覆盖
      // 本层光场继续往下传（"Ln 与 Ln-1 相加"）
      Ftot-=max(vec3(0.0),c.rgb-u_backdrop-vec3(0.006))*(1.0-clamp(c.a,0.0,1.0));
      continue;
    }
    if(op<=0.0)continue;
    if(u_albedoAsOpaque!=0){
      // 原始 albedo：**以完整颜色显示**（画上去什么颜色就是什么颜色），
      // alpha 只削弱下面那层，而不是把自己的颜色往黑色上混。
      // 写成 mix(c.rgb,acc,c.a*op) 是错的（那是"半透明就画得暗"）；
      // 正确形式是 c.rgb + (acc-c.rgb)*(1-a*op)：
      //   a=0 -> 颜色原样、下面全透出（空气也是这一支）
      //   a=1 -> 完全覆盖下面
      acc=c.rgb+(acc-c.rgb)*(1.0-c.a*op);
    }else{
      // 受光结果：
      //   有覆盖的地方 -> 按**逐像素覆盖率**覆盖（原来只按"整层不透明度"，
      //                    于是空的上层会把下层整片擦掉：图层2的绿块看不见）
      //   空气处       -> 这一层的光晕**加法**叠上去，而不是把下面的层擦掉
      float cov=clamp(c.a,0.0,1.0);
      acc=mix(acc,c.rgb,cov*op);
      acc+=c.rgb*(1.0-cov)*op;
    }
  }
  imageStore(u_out,p,vec4(acc,1.0));
})";

LayerComposite::~LayerComposite() { shutdown(); }

bool LayerComposite::init() {
    shutdown();
    prog_ = createComputeProgram("layers/composite", kCompositeComp);
    if (!prog_) {
        fprintf(stderr, "[layers] composite shader initialization failed\n");
        return false;
    }
    u_.tex = glGetUniformLocation(prog_, "u_tex");
    u_.opacity = glGetUniformLocation(prog_, "u_opacity");
    u_.cs = glGetUniformLocation(prog_, "u_cs");
    u_.n = glGetUniformLocation(prog_, "u_n");

    u_.opaque = glGetUniformLocation(prog_, "u_albedoAsOpaque");
    u_.backdrop = glGetUniformLocation(prog_, "u_backdrop");
    u_.field = glGetUniformLocation(prog_, "u_field");
    u_.albedo2 = glGetUniformLocation(prog_, "u_albedo");
    u_.fieldStrength = glGetUniformLocation(prog_, "u_fieldStrength");
    return !checkGLError("layers/composite/init");
}

void LayerComposite::shutdown() {
    if (prog_) glDeleteProgram(prog_);
    prog_ = 0;
}

void LayerComposite::composite(GLuint layersTex, GLuint opacityTex, int layerCount, int w,
                               int h, GLuint outTex, bool albedoAsOpaque,
                               const float backdrop[3], GLuint fieldTex, GLuint albedoTex,
                               float fieldStrength) {
    if (!valid() || !layersTex || !outTex || w <= 0 || h <= 0) return;
    layerCount = layerCount < 0 ? 0 : layerCount;

    glUseProgram(prog_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, layersTex);
    glUniform1i(u_.tex, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, opacityTex);
    glUniform1i(u_.opacity, 1);
    glUniform2i(u_.cs, w, h);
    glUniform1i(u_.n, layerCount);
    // 光场模型：上层的光场照亮下层
    glUniform1f(u_.fieldStrength, fieldTex ? fieldStrength : 0.0f);
    if (fieldTex && albedoTex) {
        // 注意用 3/4：unit 0 是各层结果、unit 1 是**不透明度数组**，
        // 占用 1/2 会把不透明度覆盖掉（实测光场整个消失）。
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_ARRAY, fieldTex);
        glUniform1i(u_.field, 3);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, albedoTex);
        glUniform1i(u_.albedo2, 4);
        glActiveTexture(GL_TEXTURE0);
    }
    glUniform1i(u_.opaque, albedoAsOpaque ? 1 : 0);
    if (backdrop) {
        glUniform3fv(u_.backdrop, 1, backdrop);
    } else {
        const float kBlack[3] = {0.0f, 0.0f, 0.0f};
        glUniform3fv(u_.backdrop, 1, kBlack);
    }
    glBindImageTexture(0, outTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((GLuint)((w + 7) / 8), (GLuint)((h + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glUseProgram(0);
}

}  // namespace pb::gfx
