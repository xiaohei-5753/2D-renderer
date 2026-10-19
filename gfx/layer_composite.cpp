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
// 而那是光场（u_field）负责的事，与这里无关。
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

// ---------------------------------------------------------------------------
// 主合成
// ---------------------------------------------------------------------------

static const char* kCompositeComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_tex;      // 每层的颜色（受光结果或原始 albedo）
uniform sampler2DArray u_opacity;  // 每层：一个 float 的图层不透明度
uniform ivec2 u_cs;
uniform int u_n;                   // 参与合成的层数
uniform int u_albedoAsOpaque;      // 1 = 原始 albedo（alpha 是"有多不透明"）
uniform vec3 u_backdrop;           // 所有层都没盖住时显示什么
uniform sampler2DArray u_albedo;   // 每层的原始颜色（受光就是它乘上光）
uniform float u_fieldStrength;     // 0 = 关掉光场模型（哨兵值，见下）
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
  // 图层不透明度：**光场也要按它加权**。
  //
  // 隐藏图层在 app 层被映射成 opacity=0（见 app/lighting.cpp 的 opacityOf）。
  // 原来这里只把不透明度用在"这一层的颜色盖住多少"上，光场求和却没乘 op ——
  // 于是隐藏一盏灯所在的图层，灯本身不显示了，它洒出来的光**还在**照亮下层。
  // 用户的预期显然是"隐藏 = 这一层完全不参与"，所以两处都要乘。
  vec3 gtotal=vec3(0.0);
  if(u_fieldStrength>0.0){
    for(int i=0;i<u_n;i++){
      vec4 L=texelFetch(u_tex,ivec3(p,i),0);
      float opi=clamp(texelFetch(u_opacity,ivec3(p,i),0).r,0.0,1.0);
      gtotal+=max(vec3(0.0),L.rgb-u_backdrop-vec3(0.006))*(1.0-clamp(L.a,0.0,1.0))*opi;
    }
  }
  vec3 Ftot=gtotal;
  const vec3 kDead=vec3(0.006);

  for(int i=0;i<u_n;i++){
    vec4 c=texelFetch(u_tex,ivec3(p,i),0);
    float op=clamp(texelFetch(u_opacity,ivec3(p,i),0).r,0.0,1.0);
    if(u_fieldStrength>0.0){
      // 这一层的颜色 = 原始颜色 A_i × 累计光场 F_i，再加上垂直透过来的自发光
      vec3 A=texelFetch(u_albedo,ivec3(p,i),0).rgb;
      float cov=clamp(c.a,0.0,1.0);      // 受光结果里 alpha 存的是覆盖率
      vec3 C=A*(Ftot*u_fieldStrength+u_backdrop);
      acc=mix(acc,C,cov*op);             // 覆盖 / 半覆盖
      // 本层光场继续往下传（"Ln 与 Ln-1 相加"）。这里的权重必须与上面
      // 求和时**完全一致**（含 op），否则减法减掉的比加进去的多/少，
      // 下层拿到的光会和"直接自上而下累加"的结果对不上。
      Ftot-=max(vec3(0.0),c.rgb-u_backdrop-kDead)*(1.0-cov)*op;
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

// ---------------------------------------------------------------------------
// 自发光的垂直传输
// ---------------------------------------------------------------------------
//
// 这是"发光像素把亮度垂直透过上方各层照到相机上"的那一条光路。
//
// 两个关键决定：
//
// 1. **单独一个 pass**：主合成是**自下而上**的，而"上方各层挡了多少"是一个
//    **自上而下**的累积量。方向相反的两件事塞进同一个循环，主合成就会从
//    "一眼能看懂"变成需要推导。分开之后各自都只有几行。
//
// 2. **自发光是"光"，不是"表面"**，所以它**加法**叠加，不参与覆盖混合。
//    第一版我把它并进主合成的 `mix(acc, C, cov)` 里，结果被覆盖率乘了两次
//    （一次是这里，一次是主合成的 `cov*op`），半透明层下的发光直接少一半。
//    光就是光：它穿过上方各层该衰减多少，在下面这个循环里已经算完了，
//    主合成只要把它加到最后画面上即可。
//
//   transAbove[i] = Π(k>i) (1 - cov_k × op_k)     第 i 层**上方**的累计透过率
//   emit[i]       = 该层自发光 × 不透明度 × transAbove[i]
//   总自发光       = Σ_i emit[i]                   所有层到达相机的那部分之和
//
// 用**不透明度加权**的透过率，而不是纯覆盖率：主合成是按 `cov*op` 覆盖的，
// 两者必须一致，否则 50% 不透明的实心层会挡住光但只盖掉一半颜色。
//
// 注意 `transAbove` 是**累积**的：第 0 层的发光会穿过它上方**所有**层到达相机。
//
// 这正是老实现漏掉的东西：那时发光只是"这一层自己的颜色"，所以下方图层的发光
// 永远照不到上方图层；而 `C = A×(F+S)` 在实心像素上 S=0，还让光晕只剩 1 像素宽。
static const char* kEmitComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_albedo;    // 每层的覆盖率（alpha）
uniform sampler2DArray u_opacity;   // 每层的不透明度
uniform sampler2DArray u_material;  // 每层材质：RGB = 自发光
uniform ivec2 u_cs;
uniform int u_n;
uniform float u_emitScale;
layout(rgba16f,binding=0)writeonly uniform image2D u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;

  // 自上而下累乘：每往下走一层，就把这一层的遮挡并进透过率；
  // 同时把这一层的发光按当前透过率累加进总和。
  vec3 total=vec3(0.0);
  float trans=1.0;
  for(int i=u_n-1;i>=0;i--){
    vec3 em=texelFetch(u_material,ivec3(p,i),0).rgb*u_emitScale;
    float op=texelFetch(u_opacity,ivec3(p,i),0).r;
    float cov=clamp(texelFetch(u_albedo,ivec3(p,i),0).a,0.0,1.0);
    total+=em*op*trans;          // 本层发出的光穿过上方各层后到达相机的那部分
    trans*=(1.0-cov*op);         // 本层再把下方来的光挡掉一部分
  }
  imageStore(u_out,p,vec4(total,1.0));
})";

// 把"垂直传输后的自发光总和"加到已经合成好的画面上。
//
// 自发光是**光**：它不从属于任何一个表面，所以是加法，而不是又一次覆盖混合。
// （见上面 kEmitComp 的说明：并进 mix 会被覆盖率乘两次。）
static const char* kEmitAddComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_emit;
uniform ivec2 u_cs;
// 注意这里**不能**写 writeonly：它要读回自己刚写的内容再累加
layout(rgba8,binding=0)uniform image2D u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  vec4 c=imageLoad(u_out,p);
  imageStore(u_out,p,vec4(clamp(c.rgb+texelFetch(u_emit,p,0).rgb,0.0,1.0),1.0));
})";

LayerComposite::~LayerComposite() { shutdown(); }

bool LayerComposite::init() {
    shutdown();
    prog_ = createComputeProgram("layers/composite", kCompositeComp);
    emitProg_ = createComputeProgram("layers/emit", kEmitComp);
    emitAddProg_ = createComputeProgram("layers/emit-add", kEmitAddComp);
    if (!prog_ || !emitProg_ || !emitAddProg_) {
        fprintf(stderr, "[layers] composite shader initialization failed\n");
        return false;
    }
    u_.tex = glGetUniformLocation(prog_, "u_tex");
    u_.opacity = glGetUniformLocation(prog_, "u_opacity");
    u_.cs = glGetUniformLocation(prog_, "u_cs");
    u_.n = glGetUniformLocation(prog_, "u_n");

    u_.opaque = glGetUniformLocation(prog_, "u_albedoAsOpaque");
    u_.backdrop = glGetUniformLocation(prog_, "u_backdrop");
    u_.albedo2 = glGetUniformLocation(prog_, "u_albedo");
    u_.fieldStrength = glGetUniformLocation(prog_, "u_fieldStrength");
    uEmit_.albedo = glGetUniformLocation(emitProg_, "u_albedo");
    uEmit_.opacity = glGetUniformLocation(emitProg_, "u_opacity");
    uEmit_.material = glGetUniformLocation(emitProg_, "u_material");
    uEmit_.cs = glGetUniformLocation(emitProg_, "u_cs");
    uEmit_.n = glGetUniformLocation(emitProg_, "u_n");
    uEmit_.emitScale = glGetUniformLocation(emitProg_, "u_emitScale");
    uEmitAdd_.emit = glGetUniformLocation(emitAddProg_, "u_emit");
    uEmitAdd_.cs = glGetUniformLocation(emitAddProg_, "u_cs");
    return !checkGLError("layers/composite/init");
}

void LayerComposite::shutdown() {
    if (prog_) glDeleteProgram(prog_);
    if (emitProg_) glDeleteProgram(emitProg_);
    if (emitAddProg_) glDeleteProgram(emitAddProg_);
    prog_ = 0;
    emitProg_ = 0;
    emitAddProg_ = 0;
    if (emitTex_) glDeleteTextures(1, &emitTex_);
    emitTex_ = 0;
    emitW_ = emitH_ = 0;
}

void LayerComposite::composite(GLuint layersTex, GLuint opacityTex, int layerCount, int w,
                               int h, GLuint outTex, bool albedoAsOpaque,
                               const float backdrop[3], GLuint albedoTex,
                               float fieldStrength, GLuint materialTex, float emitScale) {
    if (!valid() || !layersTex || !outTex || w <= 0 || h <= 0) return;
    layerCount = layerCount < 0 ? 0 : layerCount;

    const GLuint groups = (GLuint)((w + 7) / 8), groupsY = (GLuint)((h + 7) / 8);
    const bool wantEmit = (materialTex != 0) && (emitScale > 0.0f) && layerCount > 0;
    // 光场模型（上层的光场照亮下层）只要求"有原始颜色 + 强度 > 0"。
    // 以前这里还要一个 fieldTex 非空，于是每层每帧得额外拷一整张光场纹理 ——
    // 而着色器从未采样它（光场就是受光结果里空气像素的颜色）。那份拷贝已删除。
    const bool wantField = (albedoTex != 0) && (fieldStrength > 0.0f);

    // --- 主合成 ---
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
    glUniform1f(u_.fieldStrength, wantField ? fieldStrength : 0.0f);
    if (wantField) {
        // 注意用 unit 4：unit 0 是各层结果、unit 1 是**不透明度数组**，
        // 占用 1/2 会把不透明度覆盖掉（实测光场整个消失）。
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
    glDispatchCompute(groups, groupsY, 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // --- 自发光的垂直传输 + 加法合成 ---
    if (wantEmit) {
        GLuint emitTex = ensureEmitTexture(w, h);
        if (emitTex) {
            // 1) 逐层算"这一层的发光有多少能到达相机"
            glUseProgram(emitProg_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, albedoTex ? albedoTex : layersTex);
            glUniform1i(uEmit_.albedo, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_ARRAY, opacityTex);
            glUniform1i(uEmit_.opacity, 1);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D_ARRAY, materialTex);
            glUniform1i(uEmit_.material, 2);
            glUniform2i(uEmit_.cs, w, h);
            glUniform1i(uEmit_.n, layerCount);
            glUniform1f(uEmit_.emitScale, emitScale);
            glBindImageTexture(0, emitTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(groups, groupsY, 1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);

            // 2) 把结果加到最后画面上（自发光是光，不是表面）
            glUseProgram(emitAddProg_);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, emitTex);
            glUniform1i(uEmitAdd_.emit, 0);
            glUniform2i(uEmitAdd_.cs, w, h);
            glBindImageTexture(0, outTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8);
            glDispatchCompute(groups, groupsY, 1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
        }
    }
    glUseProgram(0);
}

// 自发光用的纹理，按需重建（尺寸变了就重来）。它已经是"所有层到达相机的
// 自发光之**和**"，所以是一张普通的 2D 纹理。
GLuint LayerComposite::ensureEmitTexture(int w, int h) {
    if (emitTex_ && emitW_ == w && emitH_ == h) return emitTex_;
    if (emitTex_) glDeleteTextures(1, &emitTex_);
    emitTex_ = createTexture(GL_RGBA16F, w, h, GL_RGBA, GL_HALF_FLOAT);
    emitW_ = w;
    emitH_ = h;
    return emitTex_;
}

}  // namespace pb::gfx
