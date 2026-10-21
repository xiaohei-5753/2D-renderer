// tools/tile_probe.cpp — 「分块 + shared-memory 合并」到底能不能替代全局原子？
//
// ## 为什么要这个探针
//
// 实测（见 BENCHMARK.md「写的负担」）：320×180 / R=128 / 单层，扫描耗时 1.93 ms，
// 其中 **83% 是每像素的 3 次全局原子加**（1.26 亿次，约 13 ns 一次）。
// 一个自然的想法是：把画布切成小块，**同一块内多个方向的贡献先在 shared memory
// 里合并**，最后每块只往全局写一次 —— 全局原子数从 1.26 亿降到 0。
//
// 但这条路值不值得重写扫描内核，取决于一件事：**shared 原子加到底比全局原子加快多少**，
// 以及分块引入的额外开销（每块每方向都要重新进块、写块）有多大。
// 这个探针就是把这两种写入方式放在**同一份计算**上对比：
//
//   * `--mode global`：现行做法。DDA 走全画布，每个像素 3 次全局 atomicAdd。
//   * `--mode tile`  ：把画布切成 T×T，每个 workgroup 负责一块，块内所有方向
//                      累加到 shared memory（3 个平面），最后**一次**写完这块。
//
// 两个内核的 DDA 推进、取样次数、沉积次数都**完全一致**，唯一差异是写的去处。
// 所以两者的差就是"分块 + shared 合并"的净收益（或净损失）。
//
// ## 这个探针**不**做什么
//
//   * 不做波前的正确性（块的入口光用 ambient 顶替）—— 它只回答"写快多少"；
//     真做起来还要加 block 间的传播与同步，那部分开销另算。
//   * 不写图片，只报时间。
//
// 用法：
//   .\build\tile_probe.exe --size 320x180 --radius 128 --tile 16 --repeat 5
//   .\build\tile_probe.exe --mode tile --tile 8/16/32/64      # 扫块大小
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gfx/gl_common.h"
#include "gfx/gl_context.h"
#include "gfx/gl_texture.h"
#include "gfx/shader.h"
#include "tools/common.h"

using namespace pb;
using namespace pb::tools;

namespace {

int g_w = 320, g_h = 180;
int g_radius = 128;
int g_tile = 16;
int g_repeat = 5;
int g_group = 16;   // tileg：每个 workgroup 处理几个方向
std::string g_mode = "both";   // global | tile | tilew | tileg

// ---- 方向表：与 LightSweep 同构（圆盘内互素的整数方向，8 向对称）----
// 与 LightSweep::rebuildDirectionTable 完全相同（中点圆算法，8 向对称）。
// 必须一致，否则时间不能直接和引擎里的 1.93 ms 对比。
std::vector<int> buildDirs(int R) {
    std::vector<int> out;   // 扁平 [dx,dy] 对
    int m = 1 - R, x = 0, y = R;
    while (x < y) {
        const int px[8] = {x, -x, x, -x, y, -y, y, -y};
        const int py[8] = {y, y, -y, -y, x, x, -x, -x};
        for (int i = 0; i < 8; i++) {
            out.push_back(px[i]);
            out.push_back(py[i]);
        }
        x++;
        if (m < 0) {
            m += 2 * x + 1;
        } else {
            y--;
            m += 2 * (x - y) + 1;
        }
    }
    return out;
}

// ---- 内核 A：全局原子（现行做法）----
static const char* kGlobalComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit_scale;
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
void main(){
  int ri=int(gl_GlobalInvocationID.x);
  ivec2 ud=dirs[gl_GlobalInvocationID.z];
  int dx=abs(ud.x),dy=abs(ud.y);
  int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1,W=u_cs.x,H=u_cs.y;
  int rc=(dx>=dy)?(2*H+1):(2*W+1);
  if(ri>=((rc+255)/256)*256)return;
  int x,y;if(dx>=dy){x=(sx>0)?0:W-1;y=-H+ri;}else{x=-W+ri;y=(sy>0)?0:H-1;}
  vec3 lt=u_ambient;float invN=1.0/float(u_ndirs);int er=dx-dy;
  while(true){
    if(x>=0&&x<W&&y>=0&&y<H){
      ivec3 sp=ivec3(x,y,0);
      vec4 c=texelFetch(u_cc,sp,0);vec4 mt=texelFetch(u_mat,sp,0);
      float cov=c.a;vec3 em=mt.rgb*u_emit_scale;
      lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)+em;
      lt+=em*(1.0-cov)*u_cp;
      int pi=y*W+x;int wh=W*H;vec3 add=lt*invN;
      atomicAdd(d[pi],uint(clamp(add.r,0.0,1.0)*65535.0));
      atomicAdd(d[pi+wh],uint(clamp(add.g,0.0,1.0)*65535.0));
      atomicAdd(d[pi+2*wh],uint(clamp(add.b,0.0,1.0)*65535.0));
    }
    int e2=2*er;if(e2>-dy){er-=dy;x+=sx;}if(e2<dx){er+=dx;y+=sy;}
    if((sx>0&&x>=W)||(sx<0&&x<0)||(sy>0&&y>=H)||(sy<0&&y<0))break;
  }
})";

// ---- 内核 B：分块 + shared 合并 ----
//
// 每个 workgroup 负责一块 T×T。块内维护 3 个 shared 平面（u32 定点），
// 依次把**所有方向**穿过这块的线段累加进来，最后整块一次写到全局。
//
// 线程映射：256 线程 = (256/T) 个方向 × T 条射线；一个线程沿射线在块内走 T 步。
// 沉积次数 = 256 × T 步 / 每轮 × 轮数 = D × T²，与内核 A 的 D × W×H 同量级。
static const char* kTileComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit_scale;
uniform int u_T;
uniform int u_colBase;   // 波前模拟：本次 dispatch 从哪一列开始
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
shared uint acc[256*3];   // 上限 T=16（16*16*3=768），这里按 16 线程组的 256 个线程 * 3 预留

void main(){
  int T=u_T;
  int tx=u_colBase+int(gl_WorkGroupID.x), ty=int(gl_WorkGroupID.y);
  int x0=tx*T, y0=ty*T;
  int W=u_cs.x,H=u_cs.y;
  int tid=int(gl_LocalInvocationID.x);

  // 清零（T*T*3 个 u32）
  for(int i=tid;i<T*T*3;i+=256)acc[i]=0u;
  barrier();

  int dirsPerPass=256/(2*T);      // 线程映射：(方向, 射线)
  int rayIdx=tid%(2*T), dirOff=tid/(2*T);
  float invN=1.0/float(u_ndirs);

  for(int dirBase=0;dirBase<u_ndirs;dirBase+=dirsPerPass){
    int di=dirBase+dirOff;
    if(di<u_ndirs){
      ivec2 ud=dirs[di];
      int dx=abs(ud.x),dy=abs(ud.y);
      int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1;
      // **真实的射线密度**：斜率越大，穿过一块的射线越多。
      // 16×16 的块，斜率 1 时左边缘要进 32 条射线（16 行 + 16 行错位），
      // 而不是探针第一版假设的 16 条。少了这个，就低估了斜向的成本。
      int span=(dx>=dy)?((T*dy)/max(1,dx)+1):((T*dx)/max(1,dy)+1);
      int nr=T+span;
      if(rayIdx<nr){
        int x,y;
        if(dx>=dy){ x=(sx>0)?x0:(x0+T-1); y=y0-span+rayIdx; }
        else      { y=(sy>0)?y0:(y0+T-1); x=x0-span+rayIdx; }
        vec3 lt=u_ambient;int er=dx-dy;
        for(int s=0;s<T;s++){
          if(x>=x0&&x<x0+T&&y>=y0&&y<y0+T&&x<W&&y<H){
            ivec3 sp=ivec3(x,y,0);
            vec4 c=texelFetch(u_cc,sp,0);vec4 mt=texelFetch(u_mat,sp,0);
            float cov=c.a;vec3 em=mt.rgb*u_emit_scale;
            lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)+em;
            lt+=em*(1.0-cov)*u_cp;
            int pi=(y-y0)*T+(x-x0);vec3 add=lt*invN;
            atomicAdd(acc[pi],uint(clamp(add.r,0.0,1.0)*65535.0));
            atomicAdd(acc[pi+T*T],uint(clamp(add.g,0.0,1.0)*65535.0));
            atomicAdd(acc[pi+2*T*T],uint(clamp(add.b,0.0,1.0)*65535.0));
          }
          int e2=2*er;if(e2>-dy){er-=dy;x+=sx;}if(e2<dx){er+=dx;y+=sy;}
          if(x<x0-2||x>x0+T+1||y<y0-2||y>y0+T+1)break;
        }
      }
    }
    barrier();   // 等所有线程把这一批方向写完，再进入下一批（shared 要复用）
  }

  // 整块写一次全局（普通 store，无原子）
  int wh=W*H;
  for(int i=tid;i<T*T;i+=256){
    int px=x0+(i%T), py=y0+(i/T);
    if(px>=W||py>=H)continue;
    int gpi=py*W+px;
    d[gpi]+=acc[i];
    d[gpi+wh]+=acc[i+T*T];
    d[gpi+2*wh]+=acc[i+2*T*T];
  }
})";

// ---- 内核 C：分块 + 方向分组 + 列波前（修正版设计）----
//
// 严格的"每列一次 dispatch"会饿死 GPU：一列只有 tilesY 个 workgroup（T=8 时 23 个），
// 实测 4.3–7.4 ms，比现在的全局原子还慢。修法是**把方向也切开**：
// 每个 workgroup 负责（一块 × G 个方向），块内 shared 累加后整块写一次全局。
// 这样每列的 workgroup 数 = tilesY × ceil(D/G)，并行度回来了；
// 全局原子数降到 WH × ceil(D/G)，可以忽略。
//
// 波前仍然按列（入口光来自上一列的出口），但每列的活变多、串行开销被摊薄。
static const char* kTileGComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit_scale;
uniform int u_T;
uniform int u_colBase;   // 本次 dispatch 的列
uniform int u_group;     // 每个 workgroup 处理几个方向
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
shared uint acc[3*16*16];

void main(){
  int T=u_T;
  int tx=u_colBase, ty=int(gl_WorkGroupID.x), gIdx=int(gl_WorkGroupID.y);
  int x0=tx*T, y0=ty*T;
  int W=u_cs.x,H=u_cs.y;
  int tid=int(gl_LocalInvocationID.x);

  for(int i=tid;i<T*T*3;i+=256)acc[i]=0u;
  barrier();

  int raysPerDir=2*T;
  int rayIdx=tid%raysPerDir, dirOff=tid/raysPerDir;
  int dirsPerPass=256/raysPerDir;
  float invN=1.0/float(u_ndirs);

  for(int k=0;k<u_group;k+=dirsPerPass){
    int di=gIdx*u_group+k+dirOff;
    if(di<u_ndirs){
      ivec2 ud=dirs[di];
      int dx=abs(ud.x),dy=abs(ud.y);
      int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1;
      int span=(dx>=dy)?((T*dy)/max(1,dx)+1):((T*dx)/max(1,dy)+1);
      int nr=T+span;
      if(rayIdx<nr){
        int x,y;
        if(dx>=dy){ x=(sx>0)?x0:(x0+T-1); y=y0-span+rayIdx; }
        else      { y=(sy>0)?y0:(y0+T-1); x=x0-span+rayIdx; }
        vec3 lt=u_ambient;int er=dx-dy;
        for(int s=0;s<T;s++){
          if(x>=x0&&x<x0+T&&y>=y0&&y<y0+T&&x<W&&y<H){
            ivec3 sp=ivec3(x,y,0);
            vec4 c=texelFetch(u_cc,sp,0);vec4 mt=texelFetch(u_mat,sp,0);
            float cov=c.a;vec3 em=mt.rgb*u_emit_scale;
            lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)+em;
            lt+=em*(1.0-cov)*u_cp;
            int pi=(y-y0)*T+(x-x0);vec3 add=lt*invN;
            atomicAdd(acc[pi],uint(clamp(add.r,0.0,1.0)*65535.0));
            atomicAdd(acc[pi+T*T],uint(clamp(add.g,0.0,1.0)*65535.0));
            atomicAdd(acc[pi+2*T*T],uint(clamp(add.b,0.0,1.0)*65535.0));
          }
          int e2=2*er;if(e2>-dy){er-=dy;x+=sx;}if(e2<dx){er+=dx;y+=sy;}
          if(x<x0-2||x>x0+T+1||y<y0-2||y>y0+T+1)break;
        }
      }
    }
  }
  barrier();
  // 整块写一次全局（不同方向组会撞同一块 -> 这里仍需原子，但次数是 WH×组数）
  int wh=W*H;
  for(int i=tid;i<T*T;i+=256){
    int px=x0+(i%T), py=y0+(i/T);
    if(px>=W||py>=H)continue;
    int gpi=py*W+px;
    atomicAdd(d[gpi],acc[i]);
    atomicAdd(d[gpi+wh],acc[i+T*T]);
    atomicAdd(d[gpi+2*wh],acc[i+2*T*T]);
  }
})";

struct Stats { double minMs, meanMs; };

Stats timeKernel(GLuint prog, int dispatchX, int dispatchY, int dispatchZ, int frames) {
    std::vector<double> v;
    for (int f = 0; f < frames; f++) {
        glFinish();
        const auto t0 = std::chrono::steady_clock::now();
        glDispatchCompute((GLuint)dispatchX, (GLuint)dispatchY, (GLuint)dispatchZ);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        v.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    return {v.front(), sum / (double)v.size()};
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (a == "--size") { const char* v = next(); if (v) std::sscanf(v, "%dx%d", &g_w, &g_h); }
        else if (a == "--radius") { const char* v = next(); if (v) g_radius = std::atoi(v); }
        else if (a == "--tile") { const char* v = next(); if (v) g_tile = std::atoi(v); }
        else if (a == "--repeat") { const char* v = next(); if (v) g_repeat = std::atoi(v); }
        else if (a == "--mode") { const char* v = next(); if (v) g_mode = v; }
        else if (a == "--group") { const char* v = next(); if (v) g_group = std::atoi(v); }
        else if (a == "--help" || a == "-h") {
            printf("tile_probe — 全局原子 vs 分块 shared-memory 合并（同计算、只改写的去处）\n"
                   "  --size WxH      画布（默认 320x180）\n"
                   "  --radius N      方向表半径（默认 128）\n"
                   "  --tile T        块边长（默认 16；shared 上限 16）\n"
                   "  --repeat N      每项重复轮数（默认 5）\n"
                   "  --mode m        global | tile | both（默认 both）\n");
            return 0;
        }
    }

    gfx::GLContext ctx;
    if (!pb::tools::createOffscreenContext(ctx, g_w, g_h, "tile_probe")) {
        fprintf(stderr, "[tile] 无法创建 OpenGL 上下文\n");
        return 1;
    }
    printf("[tile] GPU: %s\n", (const char*)glGetString(GL_RENDERER));

    const std::vector<int> dirs = buildDirs(g_radius);
    const int ndirs = (int)dirs.size() / 2;

    // 方向 SSBO
    GLuint dirSSBO = 0;
    glGenBuffers(1, &dirSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(dirs.size() * sizeof(GLint)),
                 dirs.data(), GL_STATIC_DRAW);

    // 累积 SSBO（3 个平面）
    const int wh = g_w * g_h;
    std::vector<GLuint> zero((size_t)wh * 3, 0u);
    GLuint accSSBO = 0;
    glGenBuffers(1, &accSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, accSSBO);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(zero.size() * sizeof(GLuint)),
                 zero.data(), GL_DYNAMIC_DRAW);

    // 画布 + 材质（内容随意：只要有覆盖率和自发光，取样的缓存行为就真实）
    std::vector<uint8_t> px((size_t)wh * 4);
    for (int y = 0; y < g_h; y++) {
        for (int x = 0; x < g_w; x++) {
            const bool solid = ((x / 8 + y / 8) % 5 == 0);
            uint8_t* p = &px[((size_t)y * g_w + x) * 4];
            p[0] = (uint8_t)(x * 7 % 256);
            p[1] = (uint8_t)(y * 5 % 256);
            p[2] = (uint8_t)((x + y) * 3 % 256);
            p[3] = (uint8_t)(solid ? 255 : 0);
        }
    }

    GLuint ccTex = gfx::createTextureArray(GL_RGBA8, g_w, g_h, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    GLuint matTex = gfx::createTextureArray(GL_RGB8, g_w, g_h, 1, GL_RGB, GL_UNSIGNED_BYTE);
    std::vector<uint8_t> mats((size_t)wh * 3);
    for (size_t i = 0; i < (size_t)wh; i++) {
        mats[i * 3 + 0] = (uint8_t)(i * 3 % 64);
        mats[i * 3 + 1] = (uint8_t)(i * 5 % 64);
        mats[i * 3 + 2] = (uint8_t)(i * 7 % 64);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ccTex);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, g_w, g_h, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                    px.data());
    glBindTexture(GL_TEXTURE_2D_ARRAY, matTex);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, g_w, g_h, 1, GL_RGB, GL_UNSIGNED_BYTE,
                    mats.data());
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    GLuint progG = gfx::createComputeProgram("tile/global", kGlobalComp);
    GLuint progT = gfx::createComputeProgram("tile/tile", kTileComp);
    if (!progG || !progT) {
        fprintf(stderr, "[tile] 着色器编译失败\n");
        return 1;
    }

    printf("[tile] 画布 %dx%d  方向 %d  块 %dx%d\n", g_w, g_h, ndirs, g_tile, g_tile);
    printf("\n%-10s %10s %10s   %s\n", "写入方式", "min", "mean", "说明");
    printf("----------------------------------------------------------------------\n");

    const int chunks = (2 * std::max(g_w, g_h) + 1 + 255) / 256;
    const int tilesX = (g_w + g_tile - 1) / g_tile;
    const int tilesY = (g_h + g_tile - 1) / g_tile;

    auto bindCommon = [&](GLuint prog) {
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, ccTex);
        glUniform1i(glGetUniformLocation(prog, "u_cc"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, matTex);
        glUniform1i(glGetUniformLocation(prog, "u_mat"), 1);
        glUniform2i(glGetUniformLocation(prog, "u_cs"), g_w, g_h);
        glUniform3f(glGetUniformLocation(prog, "u_ambient"), 0.1f, 0.1f, 0.12f);
        glUniform1i(glGetUniformLocation(prog, "u_ndirs"), ndirs);
        glUniform1f(glGetUniformLocation(prog, "u_cp"), 1.0f);
        glUniform1f(glGetUniformLocation(prog, "u_emit_scale"), 1.0f);
        glUniform1i(glGetUniformLocation(prog, "u_T"), g_tile);
        glUniform1i(glGetUniformLocation(prog, "u_colBase"), 0);
        glUniform1i(glGetUniformLocation(prog, "u_group"), g_group);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, accSSBO);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirSSBO);
    };

    if (g_mode == "global" || g_mode == "both") {
        bindCommon(progG);
        const Stats s = timeKernel(progG, chunks, 1, ndirs, g_repeat * 4);
        printf("%-10s %9.3f %9.3f   %d 次全局原子/像素 × %d 方向\n", "global", s.minMs,
               s.meanMs, 3, ndirs);
    }
    if (g_mode == "tile" || g_mode == "both" || g_mode == "tilew") {
        bindCommon(progT);
        const Stats s = timeKernel(progT, tilesX, tilesY, 1, g_repeat * 4);
        printf("%-10s %9.3f %9.3f   %d×%d 块，块内 shared 合并后整块写一次\n", "tile", s.minMs,
               s.meanMs, tilesX, tilesY);
    }
    // 波前模拟：严格按列推进（每列一次 dispatch + barrier）。
    // 这一项量的是"跨块依赖"的串行化代价 —— 每列只有 tilesY 个 workgroup 在跑。
    if (g_mode == "tilew") {
        bindCommon(progT);
        const GLint locCol = glGetUniformLocation(progT, "u_colBase");
        std::vector<double> v;
        for (int r = 0; r < g_repeat * 2; r++) {
            glFinish();
            const auto t0 = std::chrono::steady_clock::now();
            for (int c = 0; c < tilesX; c++) {
                glUniform1i(locCol, c);
                glDispatchCompute(1, (GLuint)tilesY, 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            }
            glFinish();
            const auto t1 = std::chrono::steady_clock::now();
            v.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        printf("%-10s %9.3f %9.3f   %d 列串行 dispatch（每列只有 %d 个 workgroup）\n", "wavefront",
               v.front(), sum / (double)v.size(), tilesX, tilesY);
    }

    // 修正版：方向分组 + 列波前。每列 workgroup 数 = tilesY × ceil(D/G)。
    if (g_mode == "tileg") {
        GLuint progTG = gfx::createComputeProgram("tile/group", kTileGComp);
        if (!progTG) { fprintf(stderr, "[tile] tileg 着色器编译失败\n"); return 1; }
        bindCommon(progTG);
        const GLint locCol = glGetUniformLocation(progTG, "u_colBase");
        const int groups = (ndirs + g_group - 1) / g_group;
        std::vector<double> v;
        for (int r = 0; r < g_repeat * 2; r++) {
            glFinish();
            const auto t0 = std::chrono::steady_clock::now();
            for (int c = 0; c < tilesX; c++) {
                glUniform1i(locCol, c);
                glDispatchCompute((GLuint)tilesY, (GLuint)groups, 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            }
            glFinish();
            const auto t1 = std::chrono::steady_clock::now();
            v.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        printf("%-10s %9.3f %9.3f   每列 %d×%d 个 workgroup（G=%d），共 %d 列\n", "tileg", v.front(),
               sum / (double)v.size(), tilesY, groups, g_group, tilesX);
        glDeleteProgram(progTG);
    }

    glDeleteProgram(progG);
    glDeleteProgram(progT);
    glDeleteTextures(1, &ccTex);
    glDeleteTextures(1, &matTex);
    glDeleteBuffers(1, &accSSBO);
    glDeleteBuffers(1, &dirSSBO);
    return 0;
}
