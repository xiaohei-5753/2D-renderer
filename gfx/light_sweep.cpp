// gfx/light_sweep.cpp
//
// 管线（逐层版）：
//   对每个要算的图层 i：
//     1. 在 GPU 端清零累积 SSBO
//     2. scan  ：一次 dispatch 覆盖所有方向（z 维 = 方向索引），每条射线沿直线步进，
//                每一步把 lt 按第 i 层的覆盖率衰减、并额外乘上 (1 - occl_i) 得到
//                "上层投下来的影"，再把第 i 层自己的自发光与 emit_i（其他层透过
//                中间层照过来的光）叠上去；结果以原子加写入 u32 定点 SSBO
//     3. copy  ：SSBO -> RGBA16F（单层，逐层复用）
//     4. blend ：与自发光合成，写进结果数组纹理的第 i 层
//   最后 composite() 把各层按不透明度叠成最终画面（见 layer_composite.cpp）
//
// 单层文档时 occl / emit 恒为 0，整套公式退化成原来的平面内扫描 —— 这也正是
// tools/render_cli 走的路径。
#include "light_sweep.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "gl_texture.h"
#include "layer_composite.h"
#include "material_pack.h"
#include "shader.h"

namespace pb::gfx {

// ---------------------------------------------------------------------------
// 着色器
// ---------------------------------------------------------------------------

// 每条射线沿一个方向直线前进，遇到像素就按覆盖率衰减自身亮度并叠加自发光。
// 累积结果用定点 u32 原子加：GL 4.3 不保证浮点原子加，所以走 0..65535 定点。
//
// u_mat / u_occl / u_emit 都是**数组纹理**，用 u_layer 取当前正在算的那一层。
//
// 三个量在这里是互相独立的（这是本项目光照模型的核心）：
//   cov   该像素自身的遮挡（alpha 或"强制实心"标志）
//   occl  第 i 层**上方**各层的累计遮盖 —— 模型 B 里"上层给下层投影"的载体
//   em    自发光（独立的 RGB）；emit 是其他层透过中间层照过来的光
static const char* kScanComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat,u_occl,u_emit;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit_scale;
uniform int u_layer;
uniform int u_cross;      // 1 = 启用层间耦合（occl 投影 + emit 互照），0 = 纯平面内
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
void main(){
  int ri=int(gl_GlobalInvocationID.x);
  ivec2 ud=dirs[gl_GlobalInvocationID.z];
  int dx=abs(ud.x),dy=abs(ud.y);
  int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1,W=u_cs.x,H=u_cs.y;
  int rc=(dx>=dy)?(2*H+1):(2*W+1);
  // 保持与旧版逐方向 dispatch 完全相同的线程集合 ceil(rc/256)*256：
  // 这些补齐出来的射线并非无用——它们中有些从网格外起步并走进网格，会贡献真实光照。
  // 丢掉它们会改变渲染结果，所以这里显式保留。
  if(ri>=((rc+255)/256)*256)return;
  int x,y;if(dx>=dy){x=(sx>0)?0:W-1;y=-H+ri;}else{x=-W+ri;y=(sy>0)?0:H-1;}
  vec3 lt=u_ambient;float invN=1.0/float(u_ndirs);int er=dx-dy;
  while(true){
    if(x>=0&&x<W&&y>=0&&y<H){
      ivec3 sp=ivec3(x,y,u_layer);
      vec4 c=texelFetch(u_cc,sp,0);
      vec4 mt=texelFetch(u_mat,sp,0);
      int fl=int(floor(mt.a/8.0+0.5));
      // kSolid(bit1) 强制完全遮挡，忽略 alpha
      float cov=((fl&2)!=0)?1.0:c.a;
      // 自发光是**独立的 RGB**（mt.rgb），不再乘 albedo。
      // albedo 只负责"显示成什么颜色 / 透光时过滤成什么颜色"。
      vec3 em=mt.rgb*u_emit_scale;
      // 层间耦合：上层给这一层投下的影，以及其他层透过中间层照过来的光。
      vec3 emX=vec3(0.0);float occX=0.0;
      if(u_cross!=0){
        occX=texelFetch(u_occl,sp,0).r;
        emX=texelFetch(u_emit,sp,0).rgb*u_emit_scale;
      }      // 透射部分按 albedo 染色：cov 越大染色越强，cov=1 时完全阻断。
      // 再额外乘 (1-occX)：上方图层盖得越多，这一层在平面内收到的光越少。
      lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)*(1.0-occX)+em+emX;
      lt+=em*(1.0-cov)*u_cp;
      int bi=(y*W+x)*3;
      vec3 add=lt*invN;
      atomicAdd(d[bi],uint(clamp(add.r,0.0,1.0)*65535.0));
      atomicAdd(d[bi+1],uint(clamp(add.g,0.0,1.0)*65535.0));
      atomicAdd(d[bi+2],uint(clamp(add.b,0.0,1.0)*65535.0));
    }
    int e2=2*er;if(e2>-dy){er-=dy;x+=sx;}if(e2<dx){er+=dx;y+=sy;}
    if((sx>0&&x>=W)||(sx<0&&x<0)||(sy>0&&y>=H)||(sy<0&&y<0))break;
  }
})";

static const char* kCopyComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(rgba16f,binding=0)writeonly uniform image2D u_img;
uniform ivec2 u_cs;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  int bi=(p.y*u_cs.x+p.x)*3;
  imageStore(u_img,p,vec4(vec3(float(d[bi])/65535.0,float(d[bi+1])/65535.0,float(d[bi+2])/65535.0),0));
})";

// 合成**单层**的受光结果，写进结果数组纹理的第 u_layer 层。
//
// ⚠️ 输出 alpha 存的是**覆盖率**，但它的语义是"这一层的颜色要盖住下面多少"，
// 而**不是**"这个像素有多不透明" —— 所以空气像素（alpha=0）的颜色（这一层贡献的
// **光晕**）仍然要留着。这一点搞错就会让泛光整片消失（实测平均亮度 73 -> 0.47）。
// 详见 layer_composite.cpp 顶部的推导。
//
// 另外这里顺手修掉了老实现的一个已知问题：老版对半透明像素做了
// `mix(scan, em, cov) + em`，而 scan 在算这个像素时**已经**用该像素自己的 cov
// 衰减并叠加过 em 了 —— 等于二次衰减 + 二次叠加自发光（视觉上是霓虹偏亮、
// 彩色玻璃偏暗）。逐层化之后每一层都要参与合成，这种双重计算会被放大，
// 所以直接改成"平面内的光 + 自发光"，单调、可预期。
static const char* kBlendComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_cc,u_mat,u_emit;
uniform sampler2D u_scan;
uniform ivec2 u_cs;
uniform float u_emit_scale;
uniform int u_layer;
uniform int u_cross;
layout(rgba8,binding=0)writeonly uniform image2DArray u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  ivec3 sp=ivec3(p,u_layer);
  vec4 c=texelFetch(u_cc,sp,0);
  vec4 mt=texelFetch(u_mat,sp,0);
  int fl=int(floor(mt.a/8.0+0.5));
  float cov=((fl&2)!=0)?1.0:c.a;
  vec3 e=mt.rgb*u_emit_scale;                 // 自发光：独立的 RGB
  if(u_cross!=0)e+=texelFetch(u_emit,sp,0).rgb*u_emit_scale;  // 其他层照过来的光
  vec3 s=texelFetch(u_scan,p,0).rgb;          // 平面内扫描收集到的光

  // 实心像素：显示自身 albedo，并**加上**独立的彩色自发光与收集到的光。
  // 两者相加而不是互相混合，所以"暗表面发绿光"这类组合才表达得出来。
  // 半透明像素：自身颜色按覆盖率淡化后叠上光 —— 透出去的部分留给下面的图层。
  vec3 r=(cov>=1.0)?(c.rgb+e+c.rgb*s*1.2):(c.rgb*cov+e+s*(1.0-cov));
  float pk=max(max(r.r,r.g),r.b);
  if(pk>1.0)r/=pk;
  imageStore(u_out,ivec3(p,u_layer),vec4(clamp(r,0.0,1.0),cov));
})";

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

LightSweep::~LightSweep() { shutdown(); }

bool LightSweep::init(int w, int h) {
    if (w <= 0 || h <= 0) {
        fprintf(stderr, "[sweep] invalid size %dx%d\n", w, h);
        return false;
    }
    shutdown();
    w_ = w;
    h_ = h;

    // 合成结果与方向表与层数无关，先建好
    renderTex_ = createTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    scanTex_ = createTexture(GL_RGBA16F, w, h, GL_RGBA, GL_HALF_FLOAT);

    // 3 个 u32 通道（定点，范围 0..65535）的累积缓冲
    glGenBuffers(1, &scanSSBO_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, scanSSBO_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)((size_t)w * h * 3 * sizeof(GLuint)),
                 nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &dirSSBO_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirSSBO_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLint) * 2, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    scanProg_ = createComputeProgram("sweep/scan", kScanComp);
    copyProg_ = createComputeProgram("sweep/copy", kCopyComp);
    blendProg_ = createComputeProgram("sweep/blend", kBlendComp);
    if (!scanProg_ || !copyProg_ || !blendProg_ || !comp_.init()) {
        fprintf(stderr, "[sweep] shader initialization failed\n");
        shutdown();
        return false;
    }

    uScan_.cc = glGetUniformLocation(scanProg_, "u_cc");
    uScan_.mat = glGetUniformLocation(scanProg_, "u_mat");
    uScan_.occl = glGetUniformLocation(scanProg_, "u_occl");
    uScan_.emit = glGetUniformLocation(scanProg_, "u_emit");
    uScan_.cs = glGetUniformLocation(scanProg_, "u_cs");
    uScan_.ambient = glGetUniformLocation(scanProg_, "u_ambient");
    uScan_.ndirs = glGetUniformLocation(scanProg_, "u_ndirs");
    uScan_.cp = glGetUniformLocation(scanProg_, "u_cp");
    uScan_.emitScale = glGetUniformLocation(scanProg_, "u_emit_scale");
    uScan_.layer = glGetUniformLocation(scanProg_, "u_layer");
    uScan_.crossLayers = glGetUniformLocation(scanProg_, "u_cross");
    uCopy_.cs = glGetUniformLocation(copyProg_, "u_cs");
    uBlend_.cc = glGetUniformLocation(blendProg_, "u_cc");
    uBlend_.mat = glGetUniformLocation(blendProg_, "u_mat");
    uBlend_.emit = glGetUniformLocation(blendProg_, "u_emit");
    uBlend_.scan = glGetUniformLocation(blendProg_, "u_scan");
    uBlend_.cs = glGetUniformLocation(blendProg_, "u_cs");
    uBlend_.emitScale = glGetUniformLocation(blendProg_, "u_emit_scale");
    uBlend_.layer = glGetUniformLocation(blendProg_, "u_layer");
    uBlend_.crossLayers = glGetUniformLocation(blendProg_, "u_cross");

    setLayerCount(1);
    rebuildDirectionTable();
    return !checkGLError("sweep/init");
}

void LightSweep::shutdown() {
    releaseLayerTextures();
    comp_.shutdown();
    if (renderTex_) glDeleteTextures(1, &renderTex_);
    if (scanTex_) glDeleteTextures(1, &scanTex_);
    if (scanSSBO_) glDeleteBuffers(1, &scanSSBO_);
    if (dirSSBO_) glDeleteBuffers(1, &dirSSBO_);
    if (scanProg_) glDeleteProgram(scanProg_);
    if (copyProg_) glDeleteProgram(copyProg_);
    if (blendProg_) glDeleteProgram(blendProg_);
    renderTex_ = scanTex_ = 0;
    scanSSBO_ = dirSSBO_ = 0;
    scanProg_ = copyProg_ = blendProg_ = 0;
    w_ = h_ = 0;
    layers_ = 0;
    dirRadius_ = -1;
    dirCount_ = 0;
}

void LightSweep::releaseLayerTextures() {
    if (canvasArray_) glDeleteTextures(1, &canvasArray_);
    if (matArray_) glDeleteTextures(1, &matArray_);
    if (occlArray_) glDeleteTextures(1, &occlArray_);
    if (emitArray_) glDeleteTextures(1, &emitArray_);
    if (litArray_) glDeleteTextures(1, &litArray_);
    if (opacityArray_) glDeleteTextures(1, &opacityArray_);
    canvasArray_ = matArray_ = occlArray_ = emitArray_ = 0;
    litArray_ = opacityArray_ = 0;
    if (fieldArray_) glDeleteTextures(1, &fieldArray_);
    fieldArray_ = 0;
}

void LightSweep::setLayerCount(int n) {
    n = std::clamp(n, 1, kMaxLayers);
    if (n == layers_ && canvasArray_ != 0) return;
    releaseLayerTextures();
    layers_ = n;

    canvasArray_ = createTextureArray(GL_RGBA8, w_, h_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    // 材质用 RGBA32F：完全解耦后一个像素需要 5 个量（自发光 RGB + 折射率 + 标志位），
    // RGBA8 装不下，硬塞就得量化折射率
    matArray_ = createTextureArray(GL_RGBA32F, w_, h_, layers_, GL_RGBA, GL_FLOAT);
    occlArray_ = createTextureArray(GL_RGBA8, w_, h_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    emitArray_ = createTextureArray(GL_RGBA8, w_, h_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    litArray_ = createTextureArray(GL_RGBA8, w_, h_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    opacityArray_ =
        createTextureArray(GL_R32F, w_, h_, layers_, GL_RED, GL_FLOAT);
    // 每层的光场：逐层一张，合成时照亮下面的层
    fieldArray_ = createTextureArray(GL_RGBA16F, w_, h_, kMaxLayers, GL_RGBA, GL_HALF_FLOAT);

    clearAllLayers();
}

void LightSweep::clearAllLayers() {
    if (!canvasArray_) return;
    const Rect all{0, 0, w_ - 1, h_ - 1};
    std::vector<RGBA8> zero((size_t)w_ * h_, RGBA8{0, 0, 0, 0});
    std::vector<float> zf((size_t)w_ * h_ * 4, 0.0f);
    for (int i = 0; i < layers_; i++) {
        uploadArrayLayer(canvasArray_, i, w_, h_, zero.data());
        // 材质数组是 RGBA32F，按整层上传 float 数据
        glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, w_, h_, 1, GL_RGBA, GL_FLOAT,
                        zf.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        uploadArrayLayer(occlArray_, i, w_, h_, zero.data());
        uploadArrayLayer(emitArray_, i, w_, h_, zero.data());
        uploadArrayLayer(litArray_, i, w_, h_, zero.data());
        uploadFloatArrayLayer(opacityArray_, i, w_, h_, 1.0f);
    }
    (void)all;
}

// ---------------------------------------------------------------------------
// 方向表（中点圆算法的一个八分圆 + 8 重对称）
// ---------------------------------------------------------------------------

void LightSweep::rebuildDirectionTable() {
    const int R = std::max(1, st_.rayRadius);
    std::vector<GLint> dirs;
    dirs.reserve((size_t)R * 8);

    int m = 1 - R, x = 0, y = R;
    while (x < y) {
        const int px[8] = {x, -x, x, -x, y, -y, y, -y};
        const int py[8] = {y, y, -y, -y, x, x, -x, -x};
        for (int i = 0; i < 8; i++) {
            dirs.push_back(px[i]);
            dirs.push_back(py[i]);
        }
        x++;
        if (m < 0) {
            m += 2 * x + 1;
        } else {
            y--;
            m += 2 * (x - y) + 1;
        }
    }

    // compute shader 的 z 维上限是 65535
    if (dirs.size() / 2 > 65535) dirs.resize((size_t)65535 * 2);

    dirCount_ = (int)(dirs.size() / 2);
    dirRadius_ = st_.rayRadius;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirSSBO_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(dirs.size() * sizeof(GLint)),
                 dirs.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void LightSweep::ensureDirectionTable() {
    if (dirRadius_ == st_.rayRadius && dirCount_ > 0) return;
    rebuildDirectionTable();
    fprintf(stderr, "[sweep] rayRadius=%d -> %d directions\n", st_.rayRadius, dirCount_);
}

// ---------------------------------------------------------------------------
// 上传
// ---------------------------------------------------------------------------

void LightSweep::uploadLayer(int layer, const Image& albedo, const Material* mats,
                             const Rect& region) {
    if (!canvasArray_ || layer < 0 || layer >= layers_) return;
    if (albedo.width() != w_ || albedo.height() != h_) {
        fprintf(stderr, "[sweep] upload size mismatch: image %dx%d, target %dx%d\n",
                albedo.width(), albedo.height(), w_, h_);
        return;
    }

    Rect r = region;
    r.clipTo(w_, h_);
    if (r.empty()) return;

    // 只上传脏矩形那一块：数组纹理的分层子矩形上传（紧凑数据）
    static std::vector<RGBA8> tight;
    tight.resize((size_t)r.width() * (size_t)r.height());
    for (int y = 0; y < r.height(); y++) {
        const RGBA8* src = albedo.data() + (size_t)(r.y0 + y) * (size_t)w_ + (size_t)r.x0;
        std::copy(src, src + r.width(), tight.data() + (size_t)y * (size_t)r.width());
    }
    uploadArrayLayerSub(canvasArray_, layer, r.x0, r.y0, r.width(), r.height(),
                        tight.data());

    if (!mats) return;

    // 材质打包成 4 个 float/像素（见 gfx/material_pack.h），只打包脏矩形那一块
    static std::vector<float> pack;
    packMaterials(mats, w_, r, pack);
    if (!pack.empty()) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, r.x0, r.y0, layer, r.width(), r.height(), 1,
                        GL_RGBA, GL_FLOAT, pack.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
}

void LightSweep::uploadLayerChannels(int layer, const uint8_t* occl, const RGBA8* emit,
                                     const Rect& region) {
    if (!occlArray_ || !emitArray_ || layer < 0 || layer >= layers_) return;
    Rect r = region;
    r.clipTo(w_, h_);
    if (r.empty()) return;

    static std::vector<RGBA8> tmp;
    tmp.resize((size_t)r.width() * (size_t)r.height());

    if (occl) {
        for (int y = 0; y < r.height(); y++) {
            const uint8_t* src = occl + (size_t)(r.y0 + y) * (size_t)w_ + (size_t)r.x0;
            RGBA8* dst = tmp.data() + (size_t)y * (size_t)r.width();
            for (int x = 0; x < r.width(); x++)
                dst[x] = RGBA8{src[x], src[x], src[x], 255};
        }
        uploadArrayLayerSub(occlArray_, layer, r.x0, r.y0, r.width(), r.height(),
                            tmp.data());
    }
    if (emit) {
        for (int y = 0; y < r.height(); y++) {
            const RGBA8* src = emit + (size_t)(r.y0 + y) * (size_t)w_ + (size_t)r.x0;
            RGBA8* dst = tmp.data() + (size_t)y * (size_t)r.width();
            for (int x = 0; x < r.width(); x++) dst[x] = RGBA8{src[x].r, src[x].g,
                                                               src[x].b, 255};
        }
        uploadArrayLayerSub(emitArray_, layer, r.x0, r.y0, r.width(), r.height(),
                            tmp.data());
    }
}

void LightSweep::uploadLayerOpacity(int layer, float opacity) {
    if (!opacityArray_ || layer < 0 || layer >= layers_) return;
    uploadFloatArrayLayer(opacityArray_, layer, w_, h_, std::clamp(opacity, 0.0f, 1.0f));
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

void LightSweep::renderLayer(int layer) {
    if (!valid() || layer < 0 || layer >= layers_) return;
    ensureDirectionTable();
    if (dirCount_ <= 0) return;

    // 单层文档没有"其他层"，关掉层间耦合让公式退化成原来的平面内扫描。
    // 这不只是省一点算力：它也让 render_cli 的单层路径与老结果逐像素一致。
    const int cross = (layers_ > 1) ? 1 : 0;

    // --- 1. 在 GPU 端清零累积缓冲（省掉每帧 CPU->GPU 上传） ---
    const GLsizeiptr accBytes = (GLsizeiptr)((size_t)w_ * h_ * 3 * sizeof(GLuint));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, scanSSBO_);
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, accBytes, GL_RED_INTEGER,
                         GL_UNSIGNED_INT, nullptr);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, scanSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirSSBO_);

    // --- 2. 一次 dispatch 覆盖所有方向（z 维 = 方向索引） ---
    glUseProgram(scanProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
    glUniform1i(uScan_.cc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
    glUniform1i(uScan_.mat, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, occlArray_);
    glUniform1i(uScan_.occl, 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, emitArray_);
    glUniform1i(uScan_.emit, 3);
    glUniform2i(uScan_.cs, w_, h_);
    glUniform3fv(uScan_.ambient, 1, st_.ambient);
    glUniform1i(uScan_.ndirs, dirCount_);
    glUniform1f(uScan_.cp, st_.propagate);
    glUniform1f(uScan_.emitScale, st_.emitScale);
    glUniform1i(uScan_.layer, layer);
    glUniform1i(uScan_.crossLayers, cross);
    {
        const int maxrc = 2 * std::max(w_, h_) + 1;
        glDispatchCompute((GLuint)((maxrc + 255) / 256), 1, (GLuint)dirCount_);
    }
    // scan 写 SSBO，后面的 copy 要读它
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // --- 3. SSBO -> RGBA16F ---
    glUseProgram(copyProg_);
    glUniform2i(uCopy_.cs, w_, h_);
    glBindImageTexture(0, scanTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // --- 3b. 把这一层的光场存下来（后面照亮下层用）---
    if (fieldArray_ && layer >= 0 && layer < layers_)
        glCopyImageSubData(scanTex_, GL_TEXTURE_2D, 0, 0, 0, 0, fieldArray_,
                           GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, w_, h_, 1);

    // --- 4. 合成这一层，写进结果数组纹理的第 layer 层 ---
    glUseProgram(blendProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
    glUniform1i(uBlend_.cc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scanTex_);
    glUniform1i(uBlend_.scan, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
    glUniform1i(uBlend_.mat, 2);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, emitArray_);
    glUniform1i(uBlend_.emit, 3);
    glUniform2i(uBlend_.cs, w_, h_);
    glUniform1f(uBlend_.emitScale, st_.emitScale);
    glUniform1i(uBlend_.layer, layer);
    glUniform1i(uBlend_.crossLayers, cross);
    // 注意：glBindImageTexture 的 layered=GL_TRUE 才能把 image2DArray 的
    // 所有层都暴露给着色器，否则只有 layer 0 可写。
    glBindImageTexture(0, litArray_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    glUseProgram(0);
}

void LightSweep::composite() {
    if (!valid() || !litArray_ || !comp_.valid()) return;
    comp_.composite(litArray_, opacityArray_, layers_, w_, h_, renderTex_, false, nullptr,
                    fieldArray_, canvasArray_, fieldStrength_);
}

}  // namespace pb::gfx
