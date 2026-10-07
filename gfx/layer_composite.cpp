// gfx/layer_composite.cpp
//
// 一个 compute pass，从最底层往上叠。
//
// ## 关键：合成要按"可见度"而不是"覆盖率"来叠
//
// 这是我这次踩得最久的一个坑，值得写清楚：
//
// "先逐层照亮，再合成"意味着每一层的受光结果里**空气像素也是有内容的** ——
// 那正是这一层贡献的光晕（辉光/泛光）。但空气像素的覆盖率 alpha = 0。
// 如果合成直接拿 alpha 当透明度，空气就会被当成"什么都没画"而**整片丢掉**，
// 于是所有光晕消失、画面只剩几条线（实测：平均亮度从 73.8 掉到 0.73）。
//
// 所以每一层的受光结果里，alpha 存的数虽然也是"覆盖率"，语义却与 albedo 的 alpha 不同：
//   空气   覆盖率 0   -> 颜色（这一层贡献的**光晕**）必须照单全收
//   半透明 覆盖率 cov -> 颜色按 source-over 叠上去
//   实心   覆盖率 1   -> 这一层在这里不透明，把下面的挡住
//
// 也就是说：它回答的是"这一层的颜色要盖住下面多少"，而不是"这个像素有多不透明"。
// 空气像素的 alpha 是 0，却仍然要把颜色画上去 —— 这正是泛光的来源。
// （我一开始按 albedo 的语义去写，结果整片光晕被丢掉：平均亮度 73 -> 0.47。）
//
// 该层在这个像素的有效覆盖率 = 覆盖率 * 图层不透明度：
//     sa = coverage * layerOpacity
// 这样"图层不透明度"与"像素自己的遮挡 alpha"就自然相乘了 —— 不需要额外的约定。
//
// ## 另一个分支：原始 albedo（关灯看原图）
//
// 那条路径送进来的是**未受光的 albedo**，它的 alpha 就是覆盖率（不是可见度），
// 而且空气像素确实是"空的"。所以用 alphaIsVisibility=false 走另一套算法。
#include "layer_composite.h"

#include <cstdio>

#include "gl_texture.h"
#include "shader.h"

namespace pb::gfx {

static const char* kCompositeComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_tex;      // 每层：已经算好的颜色
uniform sampler2DArray u_opacity;  // 每层：一个 float 的图层不透明度
uniform ivec2 u_cs;
uniform int u_n;                   // 参与合成的层数
uniform int u_alphaIsCov;         // 1 = tex.a 是覆盖率；0 = tex.a 是可见度 (1-覆盖率)
uniform vec3 u_backdrop;           // 所有层都没盖住时显示什么
layout(rgba8,binding=0)writeonly uniform image2D u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;

  vec3 acc=u_backdrop;
  for(int i=0;i<u_n;i++){
    vec4 c=texelFetch(u_tex,ivec3(p,i),0);
    float op=texelFetch(u_opacity,ivec3(p,i),0).r;
    float cov=(u_alphaIsCov!=0)?c.a:(1.0-c.a);   // 这一层在这个像素的覆盖率
    float sa=clamp(cov*op,0.0,1.0);
    if(sa<=0.0)continue;                     // 这层这个像素是空的，看下面那层
    acc=mix(acc,c.rgb,sa);
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
    u_.alphaIsCov = glGetUniformLocation(prog_, "u_alphaIsCov");
    u_.backdrop = glGetUniformLocation(prog_, "u_backdrop");
    return !checkGLError("layers/composite/init");
}

void LayerComposite::shutdown() {
    if (prog_) glDeleteProgram(prog_);
    prog_ = 0;
}

void LayerComposite::composite(GLuint layersTex, GLuint opacityTex, int layerCount, int w,
                               int h, GLuint outTex, bool alphaIsCoverage,
                               const float backdrop[3]) {
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
    glUniform1i(u_.alphaIsCov, alphaIsCoverage ? 1 : 0);
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
