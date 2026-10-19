// gfx/light_sweep.cpp
//
// 管线（逐层版）：
//   对每个要算的图层 i：
//     1. 在 GPU 端清零累积 SSBO
//     2. scan  ：一次 dispatch 覆盖所有方向（z 维 = 方向索引），每条射线沿直线步进，
//                每一步把 lt 按第 i 层的覆盖率衰减、并把第 i 层自己的自发光叠上去；
//                结果以原子加写入 u32 定点 SSBO
//     3. copy  ：SSBO -> RGBA16F（单层，逐层复用）。这份扫描结果同时被当作
//                第 i 层的**光场**存进 fieldArray_
//     4. blend ：与自发光合成，写进结果数组纹理的第 i 层
//   最后 composite() 把各层的光场累加、乘到下面各层的颜色上（见 layer_composite.cpp）
//
// 每一层都是**独立**的一轮平面内扫描，层与层之间没有耦合 —— 层间关系全部由
// 合成那一步的光场表达。单层文档时整套公式就退化成原来的平面内扫描，
// 这正是 tools/render_cli 走的路径。
#include "light_sweep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
// u_mat 是**数组纹理**，用 u_layer 取当前正在算的那一层。
//
// 两个量在这里是互相独立的（这是本项目光照模型的核心）：
//   cov   该像素自身的遮挡（alpha 或"强制实心"标志）
//   em    自发光（独立的 RGB，不从上色派生）
//
// 层与层之间**没有**耦合：每一层各自做一轮独立的平面内扫描，层间关系由合成
// 那一步（layer_composite）用"光场"表达。曾经这里还有一条 occl/emit 的层间
// 耦合路径，它会让空图层也拿到别的层的光、导致"每新建一层画面就更亮"，已删除。
// ---------------------------------------------------------------------------
// 扫描 + 相邻射线扩散（第二版，2026-10）
// ---------------------------------------------------------------------------
//
// ## 第一版为什么失败（留作教训）
//
// 第一版我自己"重新设计"了循环结构（分块 + 固定迭代上界 + 重排线程映射），
// 结果**沉积次数变成原来的 3 倍**（5878 万），能量涨 10 倍。
// 更糟的是我用同一个输出路径做对比、被文件覆盖误导，先后得出两个相反的
// 错误结论，浪费了大量时间。
//
// 第二版的原则：**循环体逐字复制 kScanComp，只加两处改动**：
//   1. 内层步进按 W 步打包，`lt` 跨步保留
//   2. 每个 W 步片段末尾插一次"与相邻 lane 交换"
// 线程映射、射线起点、DDA、沉积表达式**一律不动** —— 这样"关掉交换时
// 与原实现逐位等价"是**结构性成立**的，不依赖我额外验证。
//
// ## 权重（用户给的规格）
//
//     L_i <- (L_i + w*L_{i-1} + w*L_{i+1}) / (1 + 2w)
//
// 原地做有读写冲突（邻居互相依赖），但它是线性算子，可严格等价地展开成
//
//     L_i' = (1 - 2g)*L_i + g*(L_{i-1} + L_{i+1}),   g = w/(1+2w)
//
// 权重和恒为 1 → 总能量守恒。
//
// ## barrier 安全性的关键
//
// 交换要求"相邻 lane = 相邻射线"，且 barrier 要求**所有 lane 走同一条路径**。
// 所以交换只能放在"所有 lane 都到达"的位置：即每个 W 步片段的末尾。
// 射线提前结束时不退出循环，只用 `alive` 守住"还要不要做事"。
static const char* kScanDiffComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit_scale;
uniform int u_layer;
uniform int u_band;
uniform int u_bandn;
uniform int u_contig;
uniform float u_diff;      // w（0 = 关闭交换；此时结果与原实现逐位相同）
uniform int u_diffstep;    // K = 片段长度（每走 K 步混一次）
uniform int u_diffdbg;     // 诊断：2 = 完全跳过交换（用来验证实现是否等价）
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
// 交换用的**全局**暂存：每个 (方向, 射线) 一个 vec4（xyz=携带的光）。
//
// ## 为什么用全局暂存而不是 shared memory
//
// 第一版用 shared：好处是快，代价是**必须逐方向 dispatch**（让一个 workgroup
// 只装一个方向的射线），于是 184 个方向 = 184 次 dispatch —— 实测这才是最大
// 的开销（比 R=128 还慢）。
//
// 改成全局暂存后，就能**一次 dispatch 覆盖所有方向**（z 维 = 方向），
// 代价是每个片段边界要写/读一次显存。实测这一版明显更快。
//
// 注意：这里**不需要 barrier** —— 每个线程只写自己的槽、只读邻居的槽，
// 而邻居的槽在它的片段里已经写过了（同一片段内不互相依赖）。
layout(std430,binding=2)buffer CarBuf{vec4 carry[];};
// 每个方向的射线数上限（与 host 的 chunks 计算一致）。暂存按
// [方向][射线] 排布，所以这个常量必须是两个 kernel 都认同的值。
const int HRAYS=65536;
void main(){
  int ri=int(gl_GlobalInvocationID.x);
  int dz=int(gl_GlobalInvocationID.z);          // 方向索引（单次 dispatch，z 维 = 方向）
  ivec2 ud=dirs[dz];
  if(u_band==1&&!(ud.x==0||ud.y==0))return;
  if(u_band==2&&(ud.x==0||ud.y==0))return;
  int dx=abs(ud.x),dy=abs(ud.y);
  int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1,W=u_cs.x,H=u_cs.y;
  int rc=(dx>=dy)?(2*H+1):(2*W+1);
  // 与 kScanComp 完全相同的线程集合（补齐到 256 的整数倍）
  if(ri>=((rc+255)/256)*256)return;
  int x,y;if(dx>=dy){x=(sx>0)?0:W-1;y=-H+ri;}else{x=-W+ri;y=(sy>0)?0:H-1;}
  vec3 lt=u_ambient;float invN=1.0/float(max(1,u_bandn));int er=dx-dy;
  int fidx=(dz*7919 + ri) % max(1,W*H);
  // ## 边走边混（用户方案的**正确**形式）
  //
  // ⚠️ 我前一版把混合放在射线走完**之后** —— 那时 lt 已是死变量，改了也不
  // 沉积。实测所有 w/K 配置能量完全相同（65526），等于根本没测到。必须
  // **在行进过程中**混合，让后续沉积用上混合后的值。
  //
  // ⚠️ 注意：这段注释在 C++ raw string 内部。**绝对不能出现右括号紧跟
  // 双引号的序列** —— 那正是 raw string 的结束定界符，会把 shader 从中间
  // 掐断，后面的 GLSL 被当成 C++ 解析，报一堆莫名其妙的"非法字符"。
  // 我就踩了两次：第一次在注释里写了括号加数字再加引号；修它的时候，
  // 解释这件事本身又把那个序列写了一遍。
  //
  // ## barrier 安全：迭代次数必须与 lane 无关
  //
  // 写成 while-alive 会让早死的 lane 跳过 barrier → UB。所以用固定次数
  // 循环，alive 只做"这一步要不要做事"的守卫；空转轮次很便宜（一判即过）。
  //
  // 上界推导：DDA 每步让 x 或 y 前进 1；射线起点落在 [-W,W)x[-H,H) 内，
  // 走出画布最多 (W+H)/max(dx,dy) 次"单位步"，每次单位步含 (dx+dy) 个 DDA 步。
  // 所以 2(W+H)(dx+dy)/max(dx,dy) 足够，再加常数余量兜底。
  const int dmax=max(1,max(dx,dy));
  const int bound=(2*(W+H)*(dx+dy))/dmax+64;
  const int step=max(1,u_diffstep);
  const int iters=bound/step+2;
  bool alive=true;
  for(int it=0;it<iters;it++){
    // --- 走 step 步（与 kScanComp 的内层相同，只多一个 alive 守卫）---
    for(int s=0;s<step;s++){
      if(alive&&x>=0&&x<W&&y>=0&&y<H){
        ivec3 sp;
        if(u_contig!=0){
          sp=ivec3(fidx%W,(fidx/W)%H,u_layer);
          fidx=(fidx+1)%max(1,W*H);
        }else{
          sp=ivec3(x,y,u_layer);
        }
        vec4 cc=texelFetch(u_cc,sp,0);
        vec4 mt=texelFetch(u_mat,sp,0);
        float cov=cc.a;
        vec3 em=mt.rgb*u_emit_scale;
        lt=lt*mix(vec3(1.0),cc.rgb,cov)*(1.0-cov)+em;
        lt+=em*(1.0-cov)*u_cp;
        int bi=(y*W+x)*3;
        vec3 add=lt*invN;
        atomicAdd(d[bi  ],uint(clamp(add.r,0.0,1.0)*65535.0));
        atomicAdd(d[bi+1],uint(clamp(add.g,0.0,1.0)*65535.0));
        atomicAdd(d[bi+2],uint(clamp(add.b,0.0,1.0)*65535.0));
      }
      if(!alive)break;
      int e2=2*er;if(e2>-dy){er-=dy;x+=sx;}if(e2<dx){er+=dx;y+=sy;}
      if((sx>0&&x>=W)||(sx<0&&x<0)||(sy>0&&y>=H)||(sy<0&&y<0))alive=false;
    }
    // --- 交换：用全局暂存取邻居 ---
    //
    // u_diff     : w，混合比例（0 = 整段跳过 → 此时与原实现逐位相同）
    // u_diffstep : K = 片段长度，即"每走 K 步混一次"
    //
    // 权重（用户规格）：L_i <- (L_i + w*L_{i-1} + w*L_{i+1}) / (1+2w)
    // 展开成 per-lane 等价形式：
    //     L_i' = (1 - 2g)L_i + g(L_{i-1} + L_{i+1}),   g = w/(1+2w)
    // 权重和恒为 1 → **总能量守恒**（可测：与基线一致）。
    //
    // 为什么不需要 barrier：每个线程只写**自己**的槽、只读邻居的槽；
    // 邻居的槽由它在**本片段**里写入（先写槽、再读邻居，同一片段内不循环
    // 依赖）。暂存数组按 (方向, 射线) 索引，不同方向互不干扰。
    if(u_diff>0.0){
      const int base=dz*HRAYS;
      carry[base+ri]=vec4(lt,1.0);
      const float g=u_diff/(1.0+2.0*u_diff);
      // 邻居 = 相邻射线（射线是平行线，索引相邻即沿垂直方向相邻）
      const int r0=(ri>0)?ri-1:ri;
      const int r1=ri+1;
      vec3 t0=carry[base+r0].xyz;
      vec3 t1=(r1<HRAYS)?carry[base+r1].xyz:t0;
      lt=(1.0-2.0*g)*lt+g*(t0+t1);
    }
  }
})";

static const char* kScanComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit_scale;
uniform int u_layer;
uniform int u_band;       // 诊断：0=不过滤 1=只轴向 2=只斜向（见 setDirectionBand）
uniform int u_bandn;      // 该扇区内实际参与的方向数（归一化用，保证与全量可比）
uniform int u_contig;     // 诊断：1=取样地址按"行内连续"走（见 setContiguousFetch）
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
void main(){
  int ri=int(gl_GlobalInvocationID.x);
  ivec2 ud=dirs[gl_GlobalInvocationID.z];
  // 诊断扇区过滤：同一份数据、同样步数、同一段代码，**唯一差异是访问模式**。
  // 轴向射线在纹理上顺序走（一条 500 步的射线只碰 ~40 条 cache line），
  // 斜向每步跨行（~500 条）。两者成本差多少，就说明内存局部性值多少钱。
  if(u_band==1&&!(ud.x==0||ud.y==0))return;
  if(u_band==2&&(ud.x==0||ud.y==0))return;
  int dx=abs(ud.x),dy=abs(ud.y);
  int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1,W=u_cs.x,H=u_cs.y;
  int rc=(dx>=dy)?(2*H+1):(2*W+1);
  // 保持与旧版逐方向 dispatch 完全相同的线程集合 ceil(rc/256)*256：
  // 这些补齐出来的射线并非无用——它们中有些从网格外起步并走进网格，会贡献真实光照。
  // 丢掉它们会改变渲染结果，所以这里显式保留。
  if(ri>=((rc+255)/256)*256)return;
  int x,y;if(dx>=dy){x=(sx>0)?0:W-1;y=-H+ri;}else{x=-W+ri;y=(sy>0)?0:H-1;}
  vec3 lt=u_ambient;float invN=1.0/float(max(1,u_bandn));int er=dx-dy;
  // 诊断：把取样地址换成"行内连续"（等价于把纹理按射线的自然顺序打包，
  // 也就是剪切布局要做的事），而**沉积地址仍然按 DDA** —— 这样
  // "步数 / 取样次数 / 原子加次数"全都完全一致，唯一差异是取样地址的连续性。
  int fidx=(int(gl_GlobalInvocationID.z)*7919 + ri) % max(1,W*H);
  while(true){
    if(x>=0&&x<W&&y>=0&&y<H){
      ivec3 sp;
      if(u_contig!=0){
        // 连续递增：模拟"剪切后逐行推进"的地址模式
        sp=ivec3(fidx%W,(fidx/W)%H,u_layer);   // 行内连续递增
        fidx=(fidx+1)%max(1,W*H);
      }else{
        sp=ivec3(x,y,u_layer);
      }
      vec4 c=texelFetch(u_cc,sp,0);
      vec4 mt=texelFetch(u_mat,sp,0);
      // 材质是 RGB8，就是纯自发光图（见 gfx/material_pack.h）。
      // 覆盖率直接取 canvas 的 alpha —— flags/kSolid 已删除。
      float cov=c.a;
      // 自发光是**独立的 RGB**（mt.rgb），不再乘 albedo。
      // albedo 只负责"显示成什么颜色 / 透光时过滤成什么颜色"。
      vec3 em=mt.rgb*u_emit_scale;
      // 透射部分按 albedo 染色：cov 越大染色越强，cov=1 时完全阻断。
      lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)+em;
      lt+=em*(1.0-cov)*u_cp;
      // 三个通道**分开平面**存放（SoA）：地址相隔 W*H。
      //
      // ⚠️ 别改回 AoS（bi / bi+1 / bi+2 相邻）——实测慢约 25%。
      // 320x180、R=128、单层：AoS 3.10 ms → SoA 2.12 ms（4 层 10.69 → 8.73）。
      // 原因不是带宽（SoA 的扇区数是 AoS 的 3 倍），而是**同一扇区的原子加被串行化**：
      // 三个相邻 uint 落在同一个 32B 扇区、同一个 L2 slice 上，RMW 只能排队；
      // 拆成三个相隔 W*H*4 字节（320x180 时 225 KB）的平面后，三次原子加落在
      // 不同 slice 上并行执行。数值完全不变（整数加法与顺序无关）——
      // 实测输出**逐位相同**（png_stat 0/57600 像素差）。
      //
      // 还试过"每个方向按 (d & 3) 落到 4 个平面、最后归约"（进一步摊薄
      // 不同方向命中同一像素地址的冲突）：实测 1 层 2.58→2.18、4 层 8.77→8.68，
      // 差异在噪声内 —— 说明**跨方向的同地址竞争不是瓶颈**，收益全来自
      // "同一个线程的 3 次原子加不要落在同一扇区"。所以别再加平面，白占内存。
      int pi=y*W+x;int wh=W*H;
      vec3 add=lt*invN;
      atomicAdd(d[pi],uint(clamp(add.r,0.0,1.0)*65535.0));
      atomicAdd(d[pi+wh],uint(clamp(add.g,0.0,1.0)*65535.0));
      atomicAdd(d[pi+2*wh],uint(clamp(add.b,0.0,1.0)*65535.0));
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
  // 与 kScanComp 的 SoA 布局对应：三个通道各占一个 W*H 平面
  int pi=p.y*u_cs.x+p.x;int wh=u_cs.x*u_cs.y;
  imageStore(u_img,p,vec4(vec3(float(d[pi])/65535.0,float(d[pi+wh])/65535.0,float(d[pi+2*wh])/65535.0),0));
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
uniform sampler2DArray u_cc,u_mat;
uniform sampler2D u_scan;
uniform ivec2 u_cs;
uniform float u_emit_scale;
uniform int u_layer;
layout(rgba8,binding=0)writeonly uniform image2DArray u_out;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  ivec3 sp=ivec3(p,u_layer);
  vec4 c=texelFetch(u_cc,sp,0);
  vec4 mt=texelFetch(u_mat,sp,0);
  // 材质是 RGB8 = 纯自发光；覆盖率取 canvas 的 alpha（见 gfx/material_pack.h）。
  float cov=c.a;
  vec3 e=mt.rgb*u_emit_scale;                 // 自发光：独立的 RGB
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
// 渐进哈希采样：每帧每个像素只采一个方向，跨帧累积
//
// 为什么值得有：确定性扫描是"每帧把 D 个方向全扫一遍"，成本 Θ(D·W·H)，
// 瓶颈是原子加（实测约 2/3）。这里改成"每帧每像素一个方向"：
//   * 每个像素只写自己的累积槽 ⇒ **零原子**；
//   * 一个 dispatch 覆盖整帧 ⇒ 帧内**零 barrier**；
//   * 单帧成本 ≈ 0.1–0.35× 一次确定性扫描（取决于画布与方向数）；
//   * 代价：需要多帧累积，**第 D 帧才与确定性扫描精确相等**。
//
// 方向由 (哈希(像素) + 帧号·黄金步长) mod D 决定 —— 步长必须低差异：
// 步长 1 的"连续轮换"会让每个像素在 N≪D 时只覆盖一个局部扇区，
// 实测误差会卡住不动（见 tools/hash_accum.cpp 的 HashRotate vs SpreadRotate）。
// ---------------------------------------------------------------------------
static const char* kClearAccumComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform ivec2 u_cs;
uniform int u_layer;
layout(rgba32f,binding=0)writeonly uniform image2DArray u_img;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  imageStore(u_img,ivec3(p,u_layer),vec4(0.0));
})";

static const char* kHashGatherComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform int u_layer;
uniform int u_ndirs;
uniform int u_frame;
uniform int u_stride;     // 哈希轮换步长（host 保证与 u_ndirs 互素）
uniform vec3 u_ambient;uniform float u_cp,u_emit_scale;
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
layout(rgba32f,binding=0)uniform image2DArray u_acc;

uint hashp(ivec2 p){
  uint h=uint(p.x)*73856093u ^ uint(p.y)*19349663u;
  h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;h^=h>>16;return h;
}

void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  int W=u_cs.x,H=u_cs.y;

  // 步长由 host 给（保证与方向数**互素**）。
  //
  // ⚠️ 这里踩过一个很隐蔽的坑：先在 shader 里直接算 (D*0.618)，结果
  // D=728 时步长 450 与 728 有公因子 2 ⇒ 每个像素只能走到 **一半**的方向
  // （偶数偏移的那 364 个），序列 364 帧后就重复。于是画面永远收敛不了，
  // 而且不同像素落在不同陪集上 ⇒ 看起来像"永久噪点"。
  int stride=max(1,u_stride);
  int d=int((hashp(p)+uint(u_frame)*uint(stride))%uint(u_ndirs));
  ivec2 ud=dirs[d];
  int dx=abs(ud.x),dy=abs(ud.y);
  int sx=(ud.x>=0)?1:-1, sy=(ud.y>=0)?1:-1;

  // 解析求出"这条射线在画布外的起点"，保证从起点到 p 恰好 t 步、
  // 且 DDA 相位与扫描内核**完全一致**（否则采样路径会偏一格）。
  //
  // 推导：x-major 时每步 x 走 1、y 每 (dx/dy) 步走 1，于是
  //     t = |p.x - 起点x|,  k = 走了多少次 y = floor((t·dy + (dx-dy)) / dx)
  // 起点 = (边界 x, p.y - sy·k)。y-major 对称。
  int x0,y0,t;
  if(dx>=dy){
    t=(sx>0)?p.x:(W-1-p.x);
    int k=(t*dy+dx-dy)/dx;
    x0=(sx>0)?0:W-1;
    y0=p.y - sy*k;
  }else{
    t=(sy>0)?p.y:(H-1-p.y);
    int k=(t*dx+dy-dx)/dy;
    y0=(sy>0)?0:H-1;
    x0=p.x - sx*k;
  }

  // 从起点正向 DDA 走到 p，累积 lt（与 kScanComp 的递推逐项一致）
  vec3 lt=u_ambient;
  int er=dx-dy,x=x0,y=y0;
  for(int s=0;s<=t;s++){
    if(x>=0&&x<W&&y>=0&&y<H){
      vec4 c=texelFetch(u_cc,ivec3(x,y,u_layer),0);
      vec4 mt=texelFetch(u_mat,ivec3(x,y,u_layer),0);
      float cov=c.a;
      vec3 em=mt.rgb*u_emit_scale;
      lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)+em;
      lt+=em*(1.0-cov)*u_cp;
    }
    if(s==t)break;
    int e2=2*er;
    if(e2>-dy){er-=dy;x+=sx;}
    if(e2<dx){er+=dx;y+=sy;}
  }

  // 滑动平均 + 二阶矩（alpha；去噪用它算逐像素方差）
  //
  // ⚠️ 累积缓冲必须是 **fp32**：这里是"滑动平均"，每帧增量是 (lt-mean)/(N+1)。
  // 在 fp16 里 N 大一点（几百~两千）增量就小于它的最小可表示间隔，均值会**冻结**
  // 在早期样本上 —— 表现为"帧数一直在涨，画面却停在低样本的样子"（辐条/颜色偏移）。
  vec4 prev=imageLoad(u_acc,ivec3(p,u_layer));
  float lum=dot(lt,vec3(0.2126,0.7152,0.0722));
  float w=1.0/(float(u_frame)+1);
  imageStore(u_acc,ivec3(p,u_layer),vec4(mix(prev.rgb,lt,w),mix(prev.a,lum*lum,w)));
})";

// 累积（fp32）-> scanTex_（fp16）。glCopyImageSubData 要求格式一致，所以这里
// 必须过一个转换通道；顺便把 alpha 置 1（blend 不读 scanTex 的 alpha）。
static const char* kHashCopyComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_src;
uniform ivec2 u_cs;
uniform int u_layer;
layout(rgba16f,binding=0)writeonly uniform image2D u_dst;
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);
  if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  imageStore(u_dst,p,vec4(texelFetch(u_src,ivec3(p,u_layer),0).rgb,1.0));
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

    // 累积缓冲：3 个 u32 通道（定点，0..65535），三个通道各占一个 W*H 平面（SoA）
    glGenBuffers(1, &scanSSBO_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, scanSSBO_);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 (GLsizeiptr)((size_t)w * h * 3 * sizeof(GLuint)), nullptr,
                 GL_DYNAMIC_DRAW);

    glGenBuffers(1, &dirSSBO_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirSSBO_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(GLint) * 2, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    scanProg_ = createComputeProgram("sweep/scan", kScanComp);
    // 扩散版扫描（B1/B2 实验用）：线程布局不同（一个 workgroup 一个方向），
    // 所以必须是独立 program，不能用同一个 dispatch。
    scanDiffProg_ = createComputeProgram("sweep/scan_diff", kScanDiffComp);
    copyProg_ = createComputeProgram("sweep/copy", kCopyComp);
    blendProg_ = createComputeProgram("sweep/blend", kBlendComp);
    // 渐进哈希采样（可选路径）
    hashProg_ = createComputeProgram("sweep/hash", kHashGatherComp);
    clearAccumProg_ = createComputeProgram("sweep/clear_accum", kClearAccumComp);
    hashCopyProg_ = createComputeProgram("sweep/hash_copy", kHashCopyComp);
    if (!scanProg_ || !scanDiffProg_ || !copyProg_ || !blendProg_ || !hashProg_ ||
        !clearAccumProg_ || !hashCopyProg_ || !comp_.init()) {
        fprintf(stderr, "[sweep] shader initialization failed\n");
        shutdown();
        return false;
    }
    if (!dn_.init(kMaxLayers)) {
        fprintf(stderr, "[sweep] 去噪器初始化失败（渐进模式不可用）\n");
    }

    uScan_.cc = glGetUniformLocation(scanProg_, "u_cc");
    uScan_.mat = glGetUniformLocation(scanProg_, "u_mat");
    uScan_.cs = glGetUniformLocation(scanProg_, "u_cs");
    uScan_.ambient = glGetUniformLocation(scanProg_, "u_ambient");
    uScan_.ndirs = glGetUniformLocation(scanProg_, "u_ndirs");
    uScan_.cp = glGetUniformLocation(scanProg_, "u_cp");
    uScan_.emitScale = glGetUniformLocation(scanProg_, "u_emit_scale");
    uScan_.layer = glGetUniformLocation(scanProg_, "u_layer");
    uScan_.band = glGetUniformLocation(scanProg_, "u_band");
    uScan_.contig = glGetUniformLocation(scanProg_, "u_contig");
    uScan_.bandn = glGetUniformLocation(scanProg_, "u_bandn");
    uScan_.diff = glGetUniformLocation(scanDiffProg_, "u_diff");
    uScan_.diffstep = glGetUniformLocation(scanDiffProg_, "u_diffstep");
    uScan_.diffdbg = glGetUniformLocation(scanDiffProg_, "u_diffdbg");
    uScan_.cc_d = glGetUniformLocation(scanDiffProg_, "u_cc");
    uScan_.mat_d = glGetUniformLocation(scanDiffProg_, "u_mat");
    uScan_.cs_d = glGetUniformLocation(scanDiffProg_, "u_cs");
    uScan_.ambient_d = glGetUniformLocation(scanDiffProg_, "u_ambient");
    uScan_.ndirs_d = glGetUniformLocation(scanDiffProg_, "u_ndirs");
    uScan_.cp_d = glGetUniformLocation(scanDiffProg_, "u_cp");
    uScan_.emitScale_d = glGetUniformLocation(scanDiffProg_, "u_emit_scale");
    uScan_.layer_d = glGetUniformLocation(scanDiffProg_, "u_layer");
    uScan_.band_d = glGetUniformLocation(scanDiffProg_, "u_band");
    uScan_.bandn_d = glGetUniformLocation(scanDiffProg_, "u_bandn");
    uScan_.contig_d = glGetUniformLocation(scanDiffProg_, "u_contig");
    uCopy_.cs = glGetUniformLocation(copyProg_, "u_cs");
    uBlend_.cc = glGetUniformLocation(blendProg_, "u_cc");
    uBlend_.mat = glGetUniformLocation(blendProg_, "u_mat");
    uBlend_.scan = glGetUniformLocation(blendProg_, "u_scan");
    uBlend_.cs = glGetUniformLocation(blendProg_, "u_cs");
    uBlend_.emitScale = glGetUniformLocation(blendProg_, "u_emit_scale");
    uBlend_.layer = glGetUniformLocation(blendProg_, "u_layer");
    uHash_.cc = glGetUniformLocation(hashProg_, "u_cc");
    uHash_.mat = glGetUniformLocation(hashProg_, "u_mat");
    uHash_.cs = glGetUniformLocation(hashProg_, "u_cs");
    uHash_.ndirs = glGetUniformLocation(hashProg_, "u_ndirs");
    uHash_.frame = glGetUniformLocation(hashProg_, "u_frame");
    uHash_.stride = glGetUniformLocation(hashProg_, "u_stride");
    uHash_.ambient = glGetUniformLocation(hashProg_, "u_ambient");
    uHash_.cp = glGetUniformLocation(hashProg_, "u_cp");
    uHash_.emitScale = glGetUniformLocation(hashProg_, "u_emit_scale");
    uHash_.layer = glGetUniformLocation(hashProg_, "u_layer");
    uClearAccum_.cs = glGetUniformLocation(clearAccumProg_, "u_cs");
    uClearAccum_.layer = glGetUniformLocation(clearAccumProg_, "u_layer");
    uHashCopy_.cs = glGetUniformLocation(hashCopyProg_, "u_cs");
    uHashCopy_.layer = glGetUniformLocation(hashCopyProg_, "u_layer");
    uHashCopy_.src = glGetUniformLocation(hashCopyProg_, "u_src");

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
    if (scanDiffProg_) glDeleteProgram(scanDiffProg_);
    if (copyProg_) glDeleteProgram(copyProg_);
    if (blendProg_) glDeleteProgram(blendProg_);
    if (hashProg_) glDeleteProgram(hashProg_);
    if (clearAccumProg_) glDeleteProgram(clearAccumProg_);
    dn_.shutdown();
    renderTex_ = scanTex_ = 0;
    scanSSBO_ = dirSSBO_ = 0;
    scanProg_ = scanDiffProg_ = copyProg_ = blendProg_ = 0;
    hashProg_ = clearAccumProg_ = 0;
    if (carrySSBO_) { glDeleteBuffers(1, &carrySSBO_); carrySSBO_ = 0; carryCap_ = 0; }
    w_ = h_ = 0;
    layers_ = 0;
    dirRadius_ = -1;
    dirCount_ = 0;
}

void LightSweep::releaseLayerTextures() {
    if (canvasArray_) glDeleteTextures(1, &canvasArray_);
    if (matArray_) glDeleteTextures(1, &matArray_);
    if (litArray_) glDeleteTextures(1, &litArray_);
    if (opacityArray_) glDeleteTextures(1, &opacityArray_);
    if (hashAccum_) glDeleteTextures(1, &hashAccum_);
    if (hashDenoise_) glDeleteTextures(1, &hashDenoise_);
    canvasArray_ = matArray_ = 0;
    litArray_ = opacityArray_ = 0;
    hashAccum_ = hashDenoise_ = 0;
    hashFrame_ = 0;
    hashNeedsClear_ = true;
}

void LightSweep::setLayerCount(int n) {
    n = std::clamp(n, 1, kMaxLayers);
    if (n == layers_ && canvasArray_ != 0) return;
    releaseLayerTextures();
    layers_ = n;

    canvasArray_ = createTextureArray(GL_RGBA8, w_, h_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    // 材质用 RGBA32F：完全解耦后一个像素需要 5 个量（自发光 RGB + 折射率 + 标志位），
    // RGBA8 装不下，硬塞就得量化折射率
    // 扫描阶段既不需要 ior（直线扫描做不了折射），也不需要 flags
    // （kSolid 已删，它是"把 alpha 设成 255"的冗余写法），所以材质只要
    // **RGB = 自发光**，3 字节/像素。见 gfx/material_pack.h。
    matArray_ = createTextureArray(GL_RGB8, w_, h_, layers_, GL_RGB, GL_UNSIGNED_BYTE);
    litArray_ = createTextureArray(GL_RGBA8, w_, h_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    opacityArray_ =
        createTextureArray(GL_R32F, w_, h_, layers_, GL_RED, GL_FLOAT);
    // 渐进哈希采样用的累积与去噪目标（RGB=E[lt]，A=E[luma²]）
    hashAccum_ = createTextureArray(GL_RGBA32F, w_, h_, layers_, GL_RGBA, GL_FLOAT);
    hashDenoise_ = createTextureArray(GL_RGBA16F, w_, h_, layers_, GL_RGBA, GL_HALF_FLOAT);
    hashFrame_ = 0;
    hashNeedsClear_ = true;
    // 注意：这里**没有**"每层光场"的独立纹理。
    // 光场就是受光结果里**空气像素的颜色**，合成阶段直接从 litArray_ 取
    // （见 layer_composite.cpp）—— 早先专门拷一份到 fieldArray_ 纯属浪费：
    // 那份纹理只被写、从未被采样。

    clearAllLayers();
}

void LightSweep::clearAllLayers() {
    if (!canvasArray_) return;
    const Rect all{0, 0, w_ - 1, h_ - 1};
    std::vector<RGBA8> zero((size_t)w_ * h_, RGBA8{0, 0, 0, 0});
    for (int i = 0; i < layers_; i++) {
        uploadArrayLayer(canvasArray_, i, w_, h_, zero.data());
        // 材质是 RGB8 = 纯自发光图，整层零 = 不自发光
        {
            static std::vector<uint8_t> mz;
            mz.assign((size_t)w_ * (size_t)h_ * 3, 0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, w_, h_, 1, GL_RGB,
                            GL_UNSIGNED_BYTE, mz.data());
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        }
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

    // --- 角度超采样（ss > 1）---
    //
    // 把每个方向按其**角度**细分成 ss 个子方向（均分该方向原来的角度间隔），
    // 射线长度不变（半径仍是 R）—— 这是它相对"直接加大半径"的关键优势：
    // 加大半径会让射线变长（成本 ∝ R），而角度细分只增加射线条数。
    //
    // 归一化：shader 里是 sum/ndirs。ss 倍子方向的均值 = 原来的中点法则值，
    // 所以**不需要改归一化**，结果与未超采样时同量级，可直接对比。
    const int ss = std::max(1, ss_);
    if (ss > 1 && !dirs.empty()) {
        // 按角度排序，才能知道每个方向的角度间隔
        std::vector<std::pair<double, int>> byAng;   // (angle, 原索引)
        const int n0 = (int)(dirs.size() / 2);
        byAng.reserve((size_t)n0);
        for (int i = 0; i < n0; i++)
            byAng.emplace_back(std::atan2((double)dirs[(size_t)i * 2 + 1],
                                          (double)dirs[(size_t)i * 2 + 0]),
                               i);
        std::sort(byAng.begin(), byAng.end());

        std::vector<GLint> dense;
        dense.reserve(dirs.size() * (size_t)ss);
        for (size_t j = 0; j < byAng.size(); j++) {
            const double a0 = byAng[j].first;
            double dA;
            if (byAng.size() >= 2) {
                // 相邻间隔（绕圈取前后平均，保证首尾也连续）
                const double prev = byAng[(j + byAng.size() - 1) % byAng.size()].first;
                const double next = byAng[(j + 1) % byAng.size()].first;
                double dp = a0 - prev, dn = next - a0;
                if (dp < 0) dp += 2.0 * 3.14159265358979323846;
                if (dn < 0) dn += 2.0 * 3.14159265358979323846;
                dA = 0.5 * (dp + dn);
            } else {
                dA = 2.0 * 3.14159265358979323846;
            }
            for (int s = 0; s < ss; s++) {
                // 子方向在该角度间隔内均匀分布（含端点内侧，避免与邻居重复）
                const double off = dA * ((double)s / (double)ss - 0.5) +
                                   dA * 0.5 / (double)ss;
                const double a = a0 + off;
                int dx = (int)std::lround((double)R * std::cos(a));
                int dy = (int)std::lround((double)R * std::sin(a));
                // 不允许出现 (0,0)：DDA 会退化
                if (dx == 0 && dy == 0) dx = 1;
                dense.push_back(dx);
                dense.push_back(dy);
            }
        }
        dirs.swap(dense);
        if (dirs.size() / 2 > 65535) dirs.resize((size_t)65535 * 2);
    }

    dirCount_ = (int)(dirs.size() / 2);
    dirRadius_ = st_.rayRadius;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirSSBO_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(dirs.size() * sizeof(GLint)),
                 dirs.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void LightSweep::ensureDirectionTable() {
    if (dirRadius_ == st_.rayRadius && dirCount_ > 0 && dirSS_ == ss_) return;
    rebuildDirectionTable();
    dirSS_ = ss_;
    // 哈希轮换的步长：黄金比例 ≈0.618D，但**必须与方向数互素**，否则
    // 每个像素只能覆盖一半（或 1/k）的方向，序列提前重复 ⇒ 永远收敛不了。
    // 这个 bug 在 D=728 时的表现就是"1300 帧后仍然满是噪点"。
    {
        int s = std::max(1, (int)((double)dirCount_ * 0.6180339887 + 0.5));
        auto gcd = [](int a, int b) { while (b) { const int t = a % b; a = b; b = t; } return a; };
        int guard = 0;
        while (dirCount_ > 1 && gcd(s, dirCount_) != 1 && guard++ < 64) s++;
        hashStride_ = std::max(1, s);
    }
    fprintf(stderr, "[sweep] rayRadius=%d%s -> %d directions（哈希步长 %d）\n", st_.rayRadius,
            ss_ > 1 ? " (角度超采样)" : "", dirCount_, hashStride_);
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

    // 材质打包成 3 字节/像素（纯自发光，见 gfx/material_pack.h），只打包脏矩形。
    // 扫描不需要 ior、也没有 flags，所以这里是无损的 —— 由
    // tools/material_pack_test 穷举证明。
    static std::vector<uint8_t> pack;
    packMaterialsScan(mats, w_, r, pack);
    if (!pack.empty()) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   // RGB 是 3 字节，必须设 1
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, r.x0, r.y0, layer, r.width(), r.height(),
                        1, GL_RGB, GL_UNSIGNED_BYTE, pack.data());
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
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
    if (progressive_) { renderLayerProgressive(layer); return; }

    // --- 1. 在 GPU 端清零累积缓冲（省掉每帧 CPU->GPU 上传） ---
    const GLsizeiptr accBytes = (GLsizeiptr)((size_t)w_ * h_ * 3 * sizeof(GLuint));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, scanSSBO_);
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, accBytes, GL_RED_INTEGER,
                         GL_UNSIGNED_INT, nullptr);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, scanSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirSSBO_);

    // --- 2. 一次 dispatch 覆盖所有方向（z 维 = 方向索引） ---
    // 诊断用的并行度上限：只跑前 n 个方向。射线长度不变，只有并行度变了。
    const int ndirs = (dirLimit_ > 0 && dirLimit_ < dirCount_) ? dirLimit_ : dirCount_;
    glUseProgram(scanProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
    glUniform1i(uScan_.cc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
    glUniform1i(uScan_.mat, 1);
    glUniform2i(uScan_.cs, w_, h_);
    glUniform3fv(uScan_.ambient, 1, st_.ambient);
    glUniform1i(uScan_.ndirs, ndirs);
    glUniform1f(uScan_.cp, st_.propagate);
    glUniform1f(uScan_.emitScale, st_.emitScale);
    glUniform1i(uScan_.layer, layer);
    // 诊断扇区：统计该扇区内有多少方向（归一化用）
    int bandn = ndirs;
    if (dirBand_ != 0) {
        bandn = 0;
        std::vector<GLint> dt((size_t)ndirs * 2, 0);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirSSBO_);
        void* mp = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                    (GLsizeiptr)(dt.size() * sizeof(GLint)), GL_MAP_READ_BIT);
        if (mp) { std::memcpy(dt.data(), mp, dt.size() * sizeof(GLint));
                  glUnmapBuffer(GL_SHADER_STORAGE_BUFFER); }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        for (int i = 0; i < ndirs; i++) {
            const bool axial = (dt[(size_t)i * 2] == 0 || dt[(size_t)i * 2 + 1] == 0);
            if ((dirBand_ == 1 && axial) || (dirBand_ == 2 && !axial)) bandn++;
        }
        if (bandn <= 0) bandn = 1;
    }
    glUniform1i(uScan_.band, dirBand_);
    glUniform1i(uScan_.bandn, bandn);
    glUniform1i(uScan_.contig, contiguousFetch_ ? 1 : 0);

    // 需要的射线数上限（与 kernel 里 rc 的定义一致）：
    //   dx>=dy 时射线横向走 -> 2*H+1 条；否则纵向走 -> 2*W+1 条。
    // 旧实现直接用了 2*max(W,H)+1（对两种情形都够大），这里保持一致。
    const int chunks = (2 * std::max(w_, h_) + 1 + 255) / 256;

    // --- 扩散路径（第二版：单次 dispatch + 全局暂存）---
    //
    // 第一版用 shared memory 交换，代价是**必须逐方向 dispatch**（一个
    // workgroup 只能装一个方向的射线），184 个方向 = 184 次 dispatch，
    // 实测比 R=128 还慢。第二版改用全局暂存，于是可以一次 dispatch 覆盖
    // 所有方向（z 维 = 方向），去掉了那 184 次 dispatch 的开销。
    //
    // 等价性验证（脚本核对 + 实测）：`--diff 1e-9` 时与原 kernel **逐位相同**
    // （0/82944 像素差异）。这条是"实现正确"的硬证据，任何改动后都要重测。
    // 从 settings 同步扩散参数：UI（面板）改的就是 settings，所以这里读它，
    // 不要另设一份状态，否则面板上的值不会生效。
    diffuseEnable_ = st_.diffusionOn;
    diffuse_ = st_.diffusion;
    diffuseStep_ = st_.diffusionStep;

    if (diffuseEnable_ && diffuse_ > 0.0f) {
        glUseProgram(scanDiffProg_);
        glUniform1i(uScan_.cc_d, 0);
        glUniform1i(uScan_.mat_d, 1);
        glUniform2i(uScan_.cs_d, w_, h_);
        glUniform3fv(uScan_.ambient_d, 1, st_.ambient);
        glUniform1i(uScan_.ndirs_d, ndirs);
        glUniform1f(uScan_.cp_d, st_.propagate);
        glUniform1f(uScan_.emitScale_d, st_.emitScale);
        glUniform1i(uScan_.layer_d, layer);
        glUniform1i(uScan_.band_d, dirBand_);
        glUniform1i(uScan_.bandn_d, bandn);
        glUniform1i(uScan_.contig_d, contiguousFetch_ ? 1 : 0);
        glUniform1f(uScan_.diff, diffuse_);
        glUniform1i(uScan_.diffstep, std::max(1, diffuseStep_));
        glUniform1i(uScan_.diffdbg, diffuseDbg_);
        // 暂存：[方向][射线] 各一个 vec4。按需扩容，避免每帧重分配。
        {
            const size_t need = (size_t)ndirs * (size_t)kCarryRays;
            if (need > carryCap_) {
                if (carrySSBO_) glDeleteBuffers(1, &carrySSBO_);
                glGenBuffers(1, &carrySSBO_);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, carrySSBO_);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             (GLsizeiptr)(need * sizeof(float) * 4), nullptr,
                             GL_DYNAMIC_DRAW);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
                carryCap_ = need;
            }
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, carrySSBO_);
        }
        glDispatchCompute((GLuint)chunks, 1, (GLuint)ndirs);
    } else {
        glDispatchCompute((GLuint)chunks, 1, (GLuint)ndirs);
    }
    // scan 写 SSBO，后面的 copy 要读它
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    // --- 3. SSBO -> RGBA16F ---
    glUseProgram(copyProg_);
    glUniform2i(uCopy_.cs, w_, h_);
    glBindImageTexture(0, scanTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    // copy 写的是 image2D，下一步 blend 用 texelFetch 读它 —— 只需要 image 写可见，
    // 用 GL_ALL_BARRIER_BITS 会把所有缓存（包括顶点/纹理/SSBO）全刷一遍，
    // 而这条链路上没有别的东西被写过。
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    // 这里原来还有一次 "把这一层的光场存进 fieldArray_" 的 glCopyImageSubData
    // （整张画布 RGBA16F，每层每帧一次）。但合成的着色器**从来没有采样过它**：
    // u_field 声明了、绑了，函数体里的光场全部取自 u_tex 的空气像素。
    // 所以那是纯粹的显存带宽浪费（5 层 320x180 约 1.2 MB/帧，2048² 时 168 MB/帧），
    // 已经连同 fieldArray_ 一起删除。见 light_sweep.h 的说明。

    // --- 4. 合成这一层，写进结果数组纹理的第 layer 层 ---
    blendLayer(layer);

    glUseProgram(0);
}

// 把 scanTex_（这一层的平面内光场）与自发光合成进 litArray_ 的第 layer 层。
// 确定性扫描与"渐进哈希采样"两条路径共用它 —— 上游怎么算出 scanTex_ 不影响这里。
void LightSweep::blendLayer(int layer) {
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
    glUniform2i(uBlend_.cs, w_, h_);
    glUniform1f(uBlend_.emitScale, st_.emitScale);
    glUniform1i(uBlend_.layer, layer);
    // 注意：glBindImageTexture 的 layered=GL_TRUE 才能把 image2DArray 的
    // 所有层都暴露给着色器，否则只有 layer 0 可写。
    glBindImageTexture(0, litArray_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

// ---------------------------------------------------------------------------
// 渐进哈希采样
// ---------------------------------------------------------------------------

void LightSweep::setProgressive(bool on) {
    if (progressive_ == on) return;
    progressive_ = on;
    resetProgressive();
}

void LightSweep::setDenoise(bool on, float strength, int iters) {
    denoise_ = on;
    denoiseStrength_ = std::clamp(strength, 0.5f, 8.0f);
    denoiseIters_ = std::clamp(iters, 1, 5);
}

void LightSweep::resetProgressive() {
    hashFrame_ = 0;
    hashNeedsClear_ = true;
}

void LightSweep::clearAccumLayer(int layer) {
    if (!clearAccumProg_ || !hashAccum_) return;
    glUseProgram(clearAccumProg_);
    glUniform2i(uClearAccum_.cs, w_, h_);
    glUniform1i(uClearAccum_.layer, layer);
    glBindImageTexture(0, hashAccum_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glUseProgram(0);
}

void LightSweep::beginFrame() {
    if (!progressive_) return;
    if (hashNeedsClear_) {
        if (getenv("PB_HASH_TRACE"))
            fprintf(stderr, "[hash] clear accum (frame was %d)\n", hashFrame_);
        // 只清**所有层**一次（在 beginFrame 里做，避免每层重复清）
        for (int i = 0; i < layers_; i++) clearAccumLayer(i);
        hashNeedsClear_ = false;
    }
}

void LightSweep::endFrame() {
    if (!progressive_) return;
    if (hashFrame_ < (1 << 20)) hashFrame_++;
}

void LightSweep::renderLayerProgressive(int layer) {
    if (!hashProg_ || !hashAccum_ || !hashDenoise_ || !scanTex_) return;
    if (getenv("PB_HASH_TRACE"))
        fprintf(stderr, "[hash] layer %d frame %d (u_frame loc %d)\n", layer, hashFrame_,
                uHash_.frame);

    // --- 1. 每像素采一个方向、正向走到该像素、并入滑动平均 ---
    glUseProgram(hashProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
    glUniform1i(uHash_.cc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
    glUniform1i(uHash_.mat, 1);
    glUniform2i(uHash_.cs, w_, h_);
    glUniform1i(uHash_.ndirs, dirCount_);
    glUniform1i(uHash_.frame, hashFrame_);
    glUniform1i(uHash_.stride, hashStride_);
    glUniform3fv(uHash_.ambient, 1, st_.ambient);
    glUniform1f(uHash_.cp, st_.propagate);
    glUniform1f(uHash_.emitScale, st_.emitScale);
    glUniform1i(uHash_.layer, layer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirSSBO_);
    glBindImageTexture(0, hashAccum_, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RGBA32F);
    glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // --- 2. 去噪（可选）：fp32 累积 -> fp16 的 hashDenoise_ ---
    bool haveFp16 = false;
    if (denoise_ && dn_.valid()) {
        dn_.apply(hashAccum_, canvasArray_, hashDenoise_, layer, w_, h_, denoiseIters_, 0.08f,
                  denoiseStrength_, 1.0f / (float)(hashFrame_ + 1));
        haveFp16 = true;
    }

    // --- 3. 写进 scanTex_（blend 只认这张 2D fp16 纹理）---
    if (haveFp16) {
        // 同格式（fp16 数组纹理的某一层 -> fp16 2D），可以直接拷
        glCopyImageSubData(hashDenoise_, GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, scanTex_,
                           GL_TEXTURE_2D, 0, 0, 0, 0, w_, h_, 1);
    } else {
        // 累积是 fp32，格式不同不能拷；过一趟转换通道（顺带 alpha 置 1）
        glUseProgram(hashCopyProg_);
        glUniform2i(uHashCopy_.cs, w_, h_);
        glUniform1i(uHashCopy_.layer, layer);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, hashAccum_);
        glUniform1i(uHashCopy_.src, 0);
        glBindImageTexture(0, scanTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
    }
    glMemoryBarrier(GL_ALL_BARRIER_BITS);

    // --- 4. 与自发光合成（与确定性扫描共用）---
    blendLayer(layer);
    glUseProgram(0);
}

void LightSweep::composite() {
    if (!valid() || !litArray_ || !comp_.valid()) return;
    // 材质数组也要传进去：合成阶段靠它拿自发光，才能把发光像素的亮度
    // **垂直**透过上方各层照到相机上（见 layer_composite.h 的推导）。
    // fieldTex 传 0：逐层光场是从 u_tex 的空气像素里取的，不需要额外纹理
    // （原来传的 fieldArray_ 着色器从未采样，已删除）。
    comp_.composite(litArray_, opacityArray_, layers_, w_, h_, renderTex_, false, nullptr,
                    canvasArray_, fieldStrength_, matArray_, st_.emitScale);
}

}  // namespace pb::gfx
