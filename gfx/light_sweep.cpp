// gfx/light_sweep.cpp
//
// 管线（与移植前的 renderScanline 一致）：
//   1. 在 GPU 端清零累积 SSBO
//   2. scan  ：一次 dispatch 覆盖所有方向（z 维 = 方向索引），每条射线沿直线步进，
//              lt = lt*(1-a) + em 累积，结果以原子加写入 u32 定点 SSBO
//   3. copy  ：SSBO -> RGBA16F
//   4. blend ：与环境/自发光合成，输出 RGBA8
#include "light_sweep.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "gl_texture.h"
#include "shader.h"

namespace pb::gfx {

// ---------------------------------------------------------------------------
// 着色器
// ---------------------------------------------------------------------------

// 每条射线沿一个方向直线前进，遇到像素就按覆盖率衰减自身亮度并叠加自发光。
// 累积结果用定点 u32 原子加：GL 4.3 不保证浮点原子加，所以走 0..65535 定点。
static const char* kScanComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2D u_cc;uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit;
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
      vec4 c=texelFetch(u_cc,ivec2(x,y),0);
      vec3 em=c.rgb*c.a*u_emit;
      lt=lt*(1.0-c.a)+em;lt+=em*(1.0-c.a)*u_cp;
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

// 合成：覆盖率 >= 1 的实心像素直接显示自身 albedo（不受光影响），
// 其余像素把扫描光照与自发光按覆盖率混合，并做峰值归一防止过曝。
static const char* kBlendComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2D u_cc,u_scan;uniform ivec2 u_cs;
layout(rgba8,binding=0)writeonly uniform image2D u_out;
uniform float u_emit;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  vec4 c=texelFetch(u_cc,p,0);
  if(c.a>=1.0){imageStore(u_out,p,c);return;}
  vec3 s=texelFetch(u_scan,p,0).rgb;
  vec3 e=c.rgb*c.a*u_emit;
  vec3 r=mix(s,e,c.a)+e;
  float pk=max(max(r.r,r.g),r.b);
  if(pk>1.0)r/=pk;
  imageStore(u_out,p,vec4(r,1.0));
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

    canvasTex_ = createTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
    scanTex_ = createTexture(GL_RGBA16F, w, h, GL_RGBA, GL_HALF_FLOAT);
    renderTex_ = createTexture(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);

    // glTexImage2D 传 nullptr 得到的是未定义内容；画布先清成全透明，
    // 这样即使调用方忘了 uploadAll()，渲染出来的也是干净的黑场而不是垃圾数据。
    {
        std::vector<RGBA8> zero((size_t)w * h, RGBA8{0, 0, 0, 0});
        uploadSubImage(canvasTex_, zero.data(), w, h, Rect{0, 0, w - 1, h - 1});
    }

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
    if (!scanProg_ || !copyProg_ || !blendProg_) {
        fprintf(stderr, "[sweep] shader initialization failed\n");
        shutdown();
        return false;
    }

    uScan_.cc = glGetUniformLocation(scanProg_, "u_cc");
    uScan_.cs = glGetUniformLocation(scanProg_, "u_cs");
    uScan_.amb = glGetUniformLocation(scanProg_, "u_ambient");
    uScan_.ndirs = glGetUniformLocation(scanProg_, "u_ndirs");
    uScan_.cp = glGetUniformLocation(scanProg_, "u_cp");
    uScan_.emit = glGetUniformLocation(scanProg_, "u_emit");
    uCopy_.cs = glGetUniformLocation(copyProg_, "u_cs");
    uBlend_.cc = glGetUniformLocation(blendProg_, "u_cc");
    uBlend_.scan = glGetUniformLocation(blendProg_, "u_scan");
    uBlend_.cs = glGetUniformLocation(blendProg_, "u_cs");
    uBlend_.emit = glGetUniformLocation(blendProg_, "u_emit");

    rebuildDirectionTable();
    return !checkGLError("sweep/init");
}

void LightSweep::shutdown() {
    if (canvasTex_) glDeleteTextures(1, &canvasTex_);
    if (scanTex_) glDeleteTextures(1, &scanTex_);
    if (renderTex_) glDeleteTextures(1, &renderTex_);
    if (scanSSBO_) glDeleteBuffers(1, &scanSSBO_);
    if (dirSSBO_) glDeleteBuffers(1, &dirSSBO_);
    if (scanProg_) glDeleteProgram(scanProg_);
    if (copyProg_) glDeleteProgram(copyProg_);
    if (blendProg_) glDeleteProgram(blendProg_);
    canvasTex_ = scanTex_ = renderTex_ = 0;
    scanSSBO_ = dirSSBO_ = 0;
    scanProg_ = copyProg_ = blendProg_ = 0;
    w_ = h_ = 0;
    dirRadius_ = -1;
    dirCount_ = 0;
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
// 上传与渲染
// ---------------------------------------------------------------------------

void LightSweep::upload(const Image& img, const Rect& dirty) {
    if (!canvasTex_) return;
    if (img.width() != w_ || img.height() != h_) {
        fprintf(stderr, "[sweep] upload size mismatch: image %dx%d, target %dx%d\n",
                img.width(), img.height(), w_, h_);
        return;
    }
    uploadSubImage(canvasTex_, img.data(), w_, h_, dirty);
}

void LightSweep::uploadAll(const Image& img) {
    upload(img, Rect{0, 0, w_ - 1, h_ - 1});
}

void LightSweep::render() {
    if (!valid()) return;
    ensureDirectionTable();
    if (dirCount_ <= 0) return;

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
    glBindTexture(GL_TEXTURE_2D, canvasTex_);
    glUniform1i(uScan_.cc, 0);
    glUniform2i(uScan_.cs, w_, h_);
    glUniform3fv(uScan_.amb, 1, st_.ambient);
    glUniform1i(uScan_.ndirs, dirCount_);
    glUniform1f(uScan_.cp, st_.propagate);
    glUniform1f(uScan_.emit, st_.emitScale);
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

    // --- 4. 合成到 renderTex ---
    glUseProgram(blendProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, canvasTex_);
    glUniform1i(uBlend_.cc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, scanTex_);
    glUniform1i(uBlend_.scan, 1);
    glUniform2i(uBlend_.cs, w_, h_);
    glUniform1f(uBlend_.emit, st_.emitScale);
    glBindImageTexture(0, renderTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    glUseProgram(0);
}

}  // namespace pb::gfx
