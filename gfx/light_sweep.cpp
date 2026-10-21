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
#include <chrono>
#include <cstring>
#include <string>
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
// ## 同步（2026-10 修正）
// 旧注释说"不需要 barrier"是**错的**：写自己的槽之后立刻读邻居，跨 warp /
// 跨 workgroup 的邻居会读到上一片段的值，混入比例随 warp 变化 = 横向带纹。
// 现在改成**双缓冲 + 每片段一道 barrier**（见文件末尾的交换段），既消除竞争，
// 又比对就地缓冲做两道 barrier 便宜。
layout(std430,binding=2)buffer CarBuf{vec4 carry[];};
// 每个方向的射线数上限（与 host 的 chunks 计算一致）。暂存按
// [方向][射线] 排布，所以这个常量必须是两个 kernel 都认同的值。
// 每个方向的射线数上限：由 host 传入（原来是常量 65536，按 368 方向会白占 386MB）。
uniform int u_hrays;
void main(){
  int ri=int(gl_GlobalInvocationID.x);
  int dz=int(gl_GlobalInvocationID.z);          // 方向索引（单次 dispatch，z 维 = 方向）
  ivec2 ud=dirs[dz];
  // 早退会让 workgroup 里部分 lane 跳过 barrier（UB），改成标志位
  const bool bandOk = !(u_band==1&&!(ud.x==0||ud.y==0)) && !(u_band==2&&(ud.x==0||ud.y==0));
  int dx=abs(ud.x),dy=abs(ud.y);
  int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1,W=u_cs.x,H=u_cs.y;
  int rc=(dx>=dy)?(2*H+1):(2*W+1);
  const bool inRange = ri < ((rc+255)/256)*256;   // 早退的等价物（barrier 要求全体到达）
  const bool laneOk = bandOk && inRange;
  // 与 kScanComp 完全相同的线程集合（补齐到 256 的整数倍）

  int x,y;if(dx>=dy){x=(sx>0)?0:W-1;y=-H+ri;}else{x=-W+ri;y=(sy>0)?0:H-1;}
  vec3 lt=u_ambient;float invN=1.0/float(max(1,u_bandn));int er=dx-dy;
  int lastKey=-1;
  int lastBK=-1;
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
    if(laneOk){
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
        // ⚠️ 必须和主内核/copy 一致用 **SoA**（三个通道各占一个 W*H 平面）。
        // 原来这里是 AoS（bi=(y*W+x)*3），而 copy 按 SoA 解码 —— 开扩散时
        // 光场被写乱，表现为整幅横向带纹；关扩散走主内核所以干净。
        int bi=y*W+x;int wh=W*H;
        vec3 add=lt*invN;
        atomicAdd(d[bi+wh],uint(clamp(add.g,0.0,1.0)*65535.0));
        atomicAdd(d[bi+2*wh],uint(clamp(add.b,0.0,1.0)*65535.0));
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
    }   // if(laneOk)
    if(u_diff>0.0){
      // ## 双缓冲交换（每个片段一道 barrier）
      //
      // 就地缓冲需要"写 -> barrier -> 读 -> barrier"两道同步，而且跨 workgroup 的
      // 邻居本来就同步不到。改成两份缓冲交替：本片段写 cur，读 prev（上一片段已
      // 全部写完，且中间有 barrier），所以每片段只要一道 barrier。
      //
      // 权重（用户规格）：L_i <- (L_i + w*L_{i-1} + w*L_{i+1}) / (1+2w)
      //     L_i' = (1 - 2g)L_i + g(L_{i-1} + L_{i+1}),  g = w/(1+2w)
      // 权重和恒为 1 → 总能量守恒。
      const int base=dz*u_hrays;
      const int halfSize=u_hrays*u_ndirs;          // 一份缓冲的大小（方向 × 射线）
      const int cur=(it&1)*halfSize;
      const int prev=(1-(it&1))*halfSize;
      if(laneOk)carry[cur+base+ri]=vec4(lt,1.0);
      // 只同步"本片段写完了"，读的是上一片段的数据（天然不冲突）
      memoryBarrier();
      barrier();
      if(it>0&&laneOk){
        const float g=u_diff/(1.0+2.0*u_diff);
        // 邻居 = 相邻射线（射线是平行线，索引相邻即沿垂直方向相邻）
        const int r0=(ri>0)?ri-1:ri;
        const int r1=ri+1;
        vec3 t0=carry[prev+base+r0].xyz;
        vec3 t1=(r1<u_hrays)?carry[prev+base+r1].xyz:t0;
        lt=(1.0-2.0*g)*lt+g*(t0+t1);
      }
    }
  }
})";

// 扫描内核的**函数体**（不含 #version）：init 时会拼上版本行，并在支持
// GL_NV_shader_atomic_int64 的机器上再拼一个 #define U64 的变体，
// 用来做"每像素 1 次 64 位原子加（21/21/21 位打包）"。
static const char* kScanBody = R"(
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform int u_ndirs;uniform float u_cp,u_emit_scale;
uniform int u_layer;
uniform int u_band;       // 诊断：0=不过滤 1=只轴向 2=只斜向（见 setDirectionBand）
uniform int u_bandn;      // 该扇区内实际参与的方向数（归一化用，保证与全量可比）
uniform int u_contig;     // 诊断：1=取样地址按"行内连续"走（见 setContiguousFetch）
uniform int u_no_write;   // 诊断：1=只走 DDA/取样/累加，**不写任何内存**（见 setScanNoWrite）
// 每像素原子加的次数（见 setAtomicPack）：
//   0 = 3 次：三个通道各一个平面（默认，16 位/通道）
//   1 = 2 次：R 放高 16 位、G 放低 16 位（一次原子），B 单独一次
//   2 = 1 次：R/G/B 各 10 位打包进一个 u32（量化 ~4 级，仅用于量时间）
//   3 = 1 次：64 位原子，R/G/B 各 21 位（需要 GL_NV_shader_atomic_int64）
uniform int u_pack;
uniform int u_major;      // 0=全部 1=只 x 主方向 2=只 y 主方向（分块路径用）
// 光场降采样：射线仍按**全分辨率**推进（薄墙的透射是逐像素精确的），
// 但只在**进入新的粗格时**沉积一次 —— 原子加数量降 s^2 倍，画质几乎不变。
uniform int u_s;          // 粗格边长（1 = 关）
uniform ivec2 u_fcs;      // 光场（粗格）尺寸
// 细趟：只算"需要细化"的块（块内有 1px 遮挡混合或自发光）。
// 掩码为 0 的块直接跳过取样与沉积（DDA 仍然推进，保证相位正确）。
uniform int u_maskOn;
uniform sampler2DArray u_mask;
uniform sampler2DArray u_ccd,u_matd;   // 降采样的 canvas/材质：平滑块按块平均传播
uniform int u_bs;                      // 掩码块边长（= 光场降采样倍率）
uniform int u_ms;
#ifdef U64
layout(std430,binding=0)buffer AccBuf{uint64_t d[];};
#else
layout(std430,binding=0)buffer AccBuf{uint d[];};
#endif
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
  // 只跑某一类方向（分块路径会用另一半；归一化仍按 D_total，见 u_bandn）
  if(u_major==1&&dx<dy)return;
  if(u_major==2&&dx>=dy)return;
  int sx=(ud.x>=0)?1:-1,sy=(ud.y>=0)?1:-1,W=u_cs.x,H=u_cs.y;
  int rc=(dx>=dy)?(2*H+1):(2*W+1);
  // 保持与旧版逐方向 dispatch 完全相同的线程集合 ceil(rc/256)*256：
  // 这些补齐出来的射线并非无用——它们中有些从网格外起步并走进网格，会贡献真实光照。
  // 丢掉它们会改变渲染结果，所以这里显式保留。
  if(ri>=((rc+255)/256)*256)return;
  int x,y;if(dx>=dy){x=(sx>0)?0:W-1;y=-H+ri;}else{x=-W+ri;y=(sy>0)?0:H-1;}
  vec3 lt=u_ambient;float invN=1.0/float(max(1,u_bandn));int er=dx-dy;
  int lastKey=-1;
  int lastBK=-1;
  // 诊断用：不写内存时把沉积值累在寄存器里（下面那句"永不成立"的写保证
  // 编译器不会把整条 DDA/取样链优化掉）。
  vec3 accNW=vec3(0.0);
  // 诊断：把取样地址换成"行内连续"（等价于把纹理按射线的自然顺序打包，
  // 也就是剪切布局要做的事），而**沉积地址仍然按 DDA** —— 这样
  // "步数 / 取样次数 / 原子加次数"全都完全一致，唯一差异是取样地址的连续性。
  int fidx=(int(gl_GlobalInvocationID.z)*7919 + ri) % max(1,W*H);
  while(true){
    if(x>=0&&x<W&&y>=0&&y<H){
      // 平滑块（掩码=0）：按**块平均**做一次等价传播，每块只做一次。
      //
      // ⚠️ 这里绝不能"跳过"：实心墙也落在平滑块里，跳过就等于墙变透明、光从
      // 远处漏过来（实测整幅偏亮 37% 并出现块状接缝）。块内覆盖率是均匀的，
      // 所以块级传播在这些块里是**精确**的，不损失质量。
      bool coarse=false;
      if(u_maskOn!=0){
        ivec2 mb=ivec2(x/u_bs,y/u_bs);
        coarse = texelFetch(u_mask,ivec3(mb,u_layer),0).r < 0.5;
        if(coarse){
          const int mw=(W+u_bs-1)/u_bs;
          int bkey=mb.y*mw+mb.x;
          if(bkey!=lastBK){
            lastBK=bkey;
            vec4 bc=texelFetch(u_ccd,ivec3(mb,u_layer),0);
            vec4 bm=texelFetch(u_matd,ivec3(mb,u_layer),0);
            float bcov=bc.a;
            vec3 bem=bm.rgb*u_emit_scale;
            lt=lt*mix(vec3(1.0),bc.rgb,bcov)*(1.0-bcov)+bem;
            lt+=bem*(1.0-bcov)*u_cp;
          }
        }
      }
      if(!coarse){
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
      // 粗格键：只有"进入新的一格"时才沉积。射线本身仍逐像素推进，
      // 所以穿过 1px 薄墙时的衰减、自发光累加都是精确的（不会漏光/过暗）。
      // 粗格键：只有"进入新的一格"时才沉积。射线本身仍逐像素推进，
      // 所以穿过 1px 薄墙时的衰减、自发光累加都是精确的（不会漏光/过暗）。
      int cw=u_fcs.x,ch=u_fcs.y;
      int key=(y/u_s)*cw+(x/u_s);
      if(key!=lastKey){
        lastKey=key;
        int pi=key;int wh=cw*ch;
        vec3 add=lt*invN;
        if(u_no_write!=0){
          accNW+=add;   // 诊断：只累加，不落盘
        }else if(u_pack==0){
#ifndef U64
          atomicAdd(d[pi],uint(clamp(add.r,0.0,1.0)*65535.0));
          atomicAdd(d[pi+wh],uint(clamp(add.g,0.0,1.0)*65535.0));
          atomicAdd(d[pi+2*wh],uint(clamp(add.b,0.0,1.0)*65535.0));
#endif
        }else if(u_pack==1){
#ifndef U64
          uint r=uint(clamp(add.r*0.0625,0.0,1.0)*65535.0);
          uint g=uint(clamp(add.g*0.0625,0.0,1.0)*65535.0);
          atomicAdd(d[pi],(r<<16)|g);
          atomicAdd(d[pi+2*wh],uint(clamp(add.b,0.0,1.0)*65535.0));
#endif
        }else if(u_pack==2){
#ifndef U64
          uint r=uint(clamp(add.r*0.0625,0.0,1.0)*1020.0);
          uint g=uint(clamp(add.g*0.0625,0.0,1.0)*1020.0);
          uint b=uint(clamp(add.b*0.0625,0.0,1.0)*1020.0);
          atomicAdd(d[pi],(r<<20)|(g<<10)|b);
#endif
        }else{
#ifdef U64
          uint64_t r=uint64_t(clamp(add.r*0.0625,0.0,1.0)*2097152.0);
          uint64_t g=uint64_t(clamp(add.g*0.0625,0.0,1.0)*2097152.0);
          uint64_t b=uint64_t(clamp(add.b*0.0625,0.0,1.0)*2097152.0);
          atomicAdd(d[pi],(r<<42)|(g<<21)|b);
#endif
        }
      }
      }   // if(!coarse)
    }
    int e2=2*er;if(e2>-dy){er-=dy;x+=sx;}if(e2<dx){er+=dx;y+=sy;}
    if((sx>0&&x>=W)||(sx<0&&x<0)||(sy>0&&y>=H)||(sy<0&&y<0))break;
  }
  // 防 DCE：这个条件永远不成立（accNW 恒为非负），但编译器无法证明，
  // 所以整条 DDA/取样/累加链都会被保留。真正执行时**没有任何写操作**。
  if(accNW.r<0.0)d[0]=uint(accNW.g);
})";

static const char* kCopyBody = R"(
layout(local_size_x=8,local_size_y=8)in;
#ifdef U64
layout(std430,binding=0)buffer AccBuf{uint64_t d[];};
#else
layout(std430,binding=0)buffer AccBuf{uint d[];};
#endif
layout(rgba16f,binding=0)writeonly uniform image2D u_img;
uniform ivec2 u_cs;
uniform int u_pack;   // 与扫描内核一致
void main(){
  ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=u_cs.x||p.y>=u_cs.y)return;
  // 与 kScanBody 的布局对应：三个通道各占一个 W*H 平面（pack=0）
  int pi=p.y*u_cs.x+p.x;int wh=u_cs.x*u_cs.y;
#ifdef U64
  uint64_t v=d[pi];
#else
  uint v=d[pi];
#endif
  vec3 w;
#ifndef U64
  if(u_pack==0){
    w=vec3(float(d[pi])/65535.0,float(d[pi+wh])/65535.0,float(d[pi+2*wh])/65535.0);
  }else if(u_pack==1){
    // pack=1 的 R/G 是按 1/16 缩放存的（留动态范围余量），解码乘回 16
    w=vec3(float(v>>16)/65535.0*16.0,float(v&0xFFFFu)/65535.0*16.0,
           float(d[pi+2*wh])/65535.0);
  }else if(u_pack==2){
    w=vec3(float((v>>20)&1023u)/1020.0*16.0,float((v>>10)&1023u)/1020.0*16.0,
           float(v&1023u)/1020.0*16.0);
  }
#else
  // 21/21/21，1/16 缩放
  {
    const uint64_t mask=2097151u;
    w=vec3(float((v>>42)&mask)/2097152.0*16.0,float((v>>21)&mask)/2097152.0*16.0,
           float(v&mask)/2097152.0*16.0);
  }
#endif
  imageStore(u_img,p,vec4(w,0));
})";

// ---------------------------------------------------------------------------
// 光场降采样：把 canvas（albedo+覆盖率）与材质（自发光）按 s×s 盒式平均
//
// 为什么只降这两张而不降整个画面：扫描只在"光场"上跑，它输出的是**低频**的
// 入射光；而 albedo 与自发光是**高频**的（1px 霓虹管、窗格）。所以：
//   扫描 (s倍降)  ->  光场 (低分辨率)  ->  合成时双线性放大
//   albedo / 自发光 / 最终 lit  ->  仍是全分辨率
// 这样泛光变平滑、而霓虹边缘一点不糊 —— 这是"降采样不显著损画质"的关键。
static const char* kFieldDownComp = R"(#version 430 core
layout(local_size_x=8,local_size_y=8)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_fcs;     // 光场尺寸
uniform ivec2 u_cs;      // 画布尺寸
uniform int u_s;         // 降采样倍率
uniform int u_layer;
layout(rgba8,binding=0)writeonly uniform image2DArray u_outCC;
layout(rgba8,binding=1)writeonly uniform image2DArray u_outMat;
layout(r8,binding=2)writeonly uniform image2DArray u_outMask;
void main(){
  ivec2 f=ivec2(gl_GlobalInvocationID.xy);
  if(f.x>=u_fcs.x||f.y>=u_fcs.y)return;
  // ⚠️ 覆盖率必须取 **max**（保守），不能取平均：
  // 1px 的墙在 4×4 块里平均后只剩 1/16 覆盖率，扫描就会"透光"，
  // 光漏到墙后，整幅画变亮（实测平均亮度 19.6 -> 124）。
  // 遮挡要保守，颜色才可以是平均（按覆盖率加权，空气不稀释墙色）。
  // ⚠️ 覆盖率归一化：texelFetch 取 .a 是 0..1，但早先版本里累加的是 0..255 的
  // 原始字节，导致 cov 远超 1 —— 内核按 (1-cov) 衰减就变成"负光"，
  // 整幅画异常偏亮且出现块状台阶。这里统一按 0..1 处理。
  float covMax=0.0, covSum=0.0;
  vec3 colSum=vec3(0.0), emSum=vec3(0.0);
  int n=0;
  for(int dy=0;dy<u_s;dy++)
    for(int dx=0;dx<u_s;dx++){
      int x=f.x*u_s+dx,y=f.y*u_s+dy;
      if(x>=u_cs.x||y>=u_cs.y)continue;
      ivec3 sp=ivec3(x,y,u_layer);
      vec4 cc=texelFetch(u_cc,sp,0);
      vec4 mm=texelFetch(u_mat,sp,0);
      float a=cc.a;
      covMax=max(covMax,a);
      covSum+=a;
      colSum+=cc.rgb*a;             // 按覆盖率加权
      emSum+=mm.rgb;                // 自发光是能量：直接求和
      ++n;
    }
  if(n==0)return;
  float meanCov=covSum/float(n);
  vec3 albedo=(covSum>1e-4)?(colSum/covSum):vec3(0.0);
  vec3 em=emSum/float(n);
  // 遮挡取 max（保守）但完全用 max 会让 1px 缝隙全闭合、整体偏暗（实测暗 35%）。
  // 折中：max 与 mean 各半 —— 既挡住 1px 墙，又保留半透明/缝隙的透光。
  float cov=0.5*covMax+0.5*meanCov;
  // 细化判据：**块内有"实心+空气"混合**（1px 墙/缝）或**块内有自发光**（1px 灯管）,
  // 这两种块的场在块内变化剧烈，必须按全分辨率算。实测 neon-rain 只占 4.6%+3.6%。
  bool mixed=(covMax>0.5&&meanCov<0.5);
  bool hasEm=(em.x+em.y+em.z)>0.002;
  imageStore(u_outMask,ivec3(f,u_layer),vec4((mixed||hasEm)?1.0:0.0,0,0,1));
  imageStore(u_outCC,ivec3(f,u_layer),vec4(albedo,cov));
  imageStore(u_outMat,ivec3(f,u_layer),vec4(em,1.0));
})";
// ---------------------------------------------------------------------------
// 分块 + shared 合并的扫描（x 主方向，按列波前）
//
// 为什么这么写：全局原子占到扫描耗时的 83%（见 BENCHMARK.md）。分块的意义是
// 把"同一块内多个方向的沉积"先在 **shared memory** 里合并，最后整块往全局写一次。
//
// 正确性靠三件事（缺一不可，任何一项错了都会在块边界出现台阶）：
//   1. **DDA 相位必须和 kScanBody 逐位一致**：在列边界处直接解析算出
//      k(t)=floor((t·dy + dx − dy)/dx)、er(t)=(dx−dy) − t·dy + k(t)·dx，
//      等价于从画布边界一路走过来（见 gather 内核同样的推导）。
//   2. **块内窗口要够高**：一列里一条射线最多走 T 步、y 最多变 T，
//      所以按 ri 分块（每块 T 条）时，写入窗口取 3T 行一定够。
//   3. **跨列状态**：每个 (方向, 射线) 的 lt 存在 st[] 里，第 0 列用 ambient。
//
// 归一化用 1/D_total（不是本 pass 的方向数），与主内核完全一致。
// ---------------------------------------------------------------------------
// 光场降采样：把 canvas（albedo+覆盖率）与材质（自发光）按 s×s 盒式平均
//
// 为什么只降这两张而不降整个画面：扫描只在"光场"上跑，它输出的是**低频**的
// 入射光；而 albedo 与自发光是**高频**的（1px 霓虹管、窗格）。所以：
//   扫描 (s倍降)  ->  光场 (低分辨率)  ->  合成时双线性放大
//   albedo / 自发光 / 最终 lit  ->  仍是全分辨率
// 这样泛光变平滑、而霓虹边缘一点不糊 —— 这是"降采样不显著损画质"的关键。
// ---------------------------------------------------------------------------
// 分块 + shared 合并的扫描（x 主方向，按列波前）
//
// 为什么这么写：全局原子占扫描耗时 83%（见 BENCHMARK.md）。分块的意义是把
// "同一块内多个方向的沉积"先在 **shared memory** 里合并，最后整块往全局写一次。
//
// 正确性靠三件事（任何一项错了都会在块边界看到台阶/暗带）：
//   1. **DDA 相位必须和 kScanBody 逐位一致**：直接解析算出
//      k(t)=floor((t*dy + dx - dy)/dx)、er(t)=(dx-dy) - t*dy + k(t)*dx，
//      等价于从画布边界一路走过来（和 gather 内核同一条推导）。
//   2. **块窗口要够高**：一列里一条射线最多走 T 步、y 最多变 T，
//      所以按"y_in 落在 [B*T, B*T+T)"分配射线、写入窗口取 3T 行，一定装得下。
//   3. **跨列状态**：每个 (方向, 射线) 的 lt 存在 st[] 里；第 0 列用 ambient。
//      没被处理过的射线状态保持初始化值 ambient —— 它们本来就还没进画布。
//
// 归一化用 1/D_total（不是本 pass 的方向数），与主内核完全一致 ⇒ 整数加法顺序
// 无关 ⇒ 与旧内核**逐位相同**（验收标准就是 png_stat 0 像素差）。
static const char* kScanColComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform float u_cp,u_emit_scale;
uniform int u_layer;
uniform int u_T;          // 列宽 = 每块每方向的射线数
uniform int u_colBase;    // 当前列
uniform int u_sign;       // +1 左->右，-1 右->左
uniform int u_group;      // 本 workgroup 处理的方向数
uniform int u_dirCount;   // 本 pass 的方向总数
uniform int u_rays;       // 每方向射线数（padded，与主内核一致）
uniform float u_invN;     // 1/D_total
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
layout(std430,binding=3)buffer StateBuf{vec4 st[];};
shared uint acc[3*3*32*32];

void main(){
  int T=u_T;
  int W=u_cs.x,H=u_cs.y,wh=W*H;
  int by=int(gl_WorkGroupID.x);      // 0..nBlocks-1；块号 B = by-1
  int gIx=int(gl_WorkGroupID.y);
  int tid=int(gl_LocalInvocationID.x);
  int B=by-1;
  int rowWin=B*T-T;                  // 窗口 [B*T-T, B*T+2T)，共 3T 行
  for(int i=tid;i<3*3*T*T;i+=256)acc[i]=0u;
  barrier();

  int rayIdx=tid%T;                  // y_in = B*T + rayIdx
  int dirOff=tid/T;
  int dirsPerPass=256/T;

  for(int k=0;k<u_group;k+=dirsPerPass){
    int di=gIx*u_group+k+dirOff;
    if(di<u_dirCount){
      ivec2 ud=dirs[di];
      int dx=abs(ud.x),dy=abs(ud.y);
      int sy=(ud.y>=0)?1:-1;
      int t=u_colBase*T;
      int kt=(t*dy+dx-dy)/dx;
      int er=(dx-dy)-t*dy+kt*dx;
      int yIn=B*T+rayIdx;
      int ri=yIn+H-sy*kt;
      if(ri>=0&&ri<u_rays){
        int y=yIn;
        int x=(u_sign>0)?t:(W-1-t);
        vec3 lt=(u_colBase==0)?u_ambient:st[di*u_rays+ri].rgb;
        for(int s=0;s<T;s++){
          if(x>=0&&x<W&&y>=0&&y<H){
            int ly=y-rowWin;
            int lx=(u_sign>0)?(x-t):((W-1-x)-t);
            if(ly>=0&&ly<3*T&&lx>=0&&lx<T){
              ivec3 sp=ivec3(x,y,u_layer);
              vec4 c=texelFetch(u_cc,sp,0);
              vec4 mt=texelFetch(u_mat,sp,0);
              float cov=c.a;
              vec3 em=mt.rgb*u_emit_scale;
              lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)+em;
              lt+=em*(1.0-cov)*u_cp;
              int pi=ly*T+lx;
              vec3 add=lt*u_invN;
              atomicAdd(acc[pi],uint(clamp(add.r,0.0,1.0)*65535.0));
              atomicAdd(acc[pi+3*T*T],uint(clamp(add.g,0.0,1.0)*65535.0));
              atomicAdd(acc[pi+2*3*T*T],uint(clamp(add.b,0.0,1.0)*65535.0));
            }
          }
          int e2=2*er;if(e2>-dy){er-=dy;x+=u_sign;}if(e2<dx){er+=dx;y+=sy;}
          if((u_sign>0&&x>=W)||(u_sign<0&&x<0)||(sy>0&&y>=H)||(sy<0&&y<0))break;
        }
        st[di*u_rays+ri]=vec4(lt,1.0);
      }
    }
  }
  barrier();

  // 整块写回全局（不同方向组会写同一块 -> 仍需原子，但次数只剩 WH×组数）
  int xBase=(u_sign>0)?(u_colBase*T):(W-1-u_colBase*T);
  for(int i=tid;i<3*T*T;i+=256){
    if(acc[i]==0u)continue;
    int lx=i%T, ly=i/T;
    int gx=(u_sign>0)?(xBase+lx):(xBase-lx);
    int gy=rowWin+ly;
    if(gx<0||gx>=W||gy<0||gy>=H)continue;
    int gpi=gy*W+gx;
    atomicAdd(d[gpi],acc[i]);
    atomicAdd(d[gpi+wh],acc[i+3*T*T]);
    atomicAdd(d[gpi+2*wh],acc[i+2*3*T*T]);
  }
})";

// ---------------------------------------------------------------------------
// 转置版：y 主方向（按行波前）
//
// 与列版本的唯一实质差异：**相位不用闭式公式，而是按方向携带 (k, er)**。
//
// 为什么：引擎的 DDA 循环在 x/y 上不对称（`if(2er>-dy)` 用 -dy、`if(2er<dx)` 用 dx），
// 把 (dx,dy) 互换得到的闭式对 y 主方向不成立（CPU 穷举对拍：dx=1,dy=5 时真实
// k(t)=0,0,1,1,1,1,1，互换公式给 k(1)=1）。而 DDA 是**平移不变**的：
// 同一方向的所有射线共享同一个 er 序列、每 T 步共享同一个 Δk。所以只需要
// 每方向携带 (k, er)，射线索引就能精确反推：ri = x_in + W - sx*k。
//
// 相位推进不看画布边界（它是整条直线的性质），由每组的锚点线程算一次写回。
static const char* kScanRowComp = R"(#version 430 core
layout(local_size_x=256)in;
uniform sampler2DArray u_cc,u_mat;
uniform ivec2 u_cs;
uniform vec3 u_ambient;uniform float u_cp,u_emit_scale;
uniform int u_layer;
uniform int u_T;
uniform int u_rowBase;
uniform int u_sign;       // +1 上->下，-1 下->上
uniform int u_group;
uniform int u_dirCount;
uniform int u_rays;
uniform float u_invN;
layout(std430,binding=0)buffer AccBuf{uint d[];};
layout(std430,binding=1)readonly buffer DirBuf{ivec2 dirs[];};
layout(std430,binding=3)buffer StateBuf{vec4 st[];};
layout(std430,binding=5)buffer PhaseBuf{ivec2 ph[];};   // 每方向 (k, er)
shared uint acc[3*3*16*16];

void main(){
  int T=u_T;
  int W=u_cs.x,H=u_cs.y,wh=W*H;
  int bx=int(gl_WorkGroupID.x);
  int gIx=int(gl_WorkGroupID.y);
  int tid=int(gl_LocalInvocationID.x);
  int B=bx-1;
  int colWin=B*T-T;
  for(int i=tid;i<3*3*T*T;i+=256)acc[i]=0u;
  barrier();

  int rayIdx=tid%T;
  int dirOff=tid/T;
  int dirsPerPass=256/T;

  for(int kk=0;kk<u_group;kk+=dirsPerPass){
    int di=gIx*u_group+kk+dirOff;
    if(di<u_dirCount){
      ivec2 ud=dirs[di];
      int dx=abs(ud.x),dy=abs(ud.y);
      int sx=(ud.x>=0)?1:-1;
      ivec2 ph0=(u_rowBase==0)?ivec2(0,dx-dy):ph[di];
      int kNow=ph0.x, er0=ph0.y;
      int xIn=B*T+rayIdx;
      int ri=xIn+W-sx*kNow;
      int y=(u_sign>0)?(u_rowBase*T):(H-1-u_rowBase*T);
      if(ri>=0&&ri<u_rays){
        int x=xIn;
        int er=er0;
        vec3 lt=(u_rowBase==0)?u_ambient:st[di*u_rays+ri].rgb;
        for(int s=0;s<T;s++){
          if(x>=0&&x<W&&y>=0&&y<H){
            int lx=x-colWin;
            int ly=(u_sign>0)?(y-u_rowBase*T):((H-1-y)-u_rowBase*T);
            if(lx>=0&&lx<3*T&&ly>=0&&ly<T){
              ivec3 sp=ivec3(x,y,u_layer);
              vec4 c=texelFetch(u_cc,sp,0);
              vec4 mt=texelFetch(u_mat,sp,0);
              float cov=c.a;
              vec3 em=mt.rgb*u_emit_scale;
              lt=lt*mix(vec3(1.0),c.rgb,cov)*(1.0-cov)+em;
              lt+=em*(1.0-cov)*u_cp;
              int pi=ly*(3*T)+lx;
              vec3 add=lt*u_invN;
              atomicAdd(acc[pi],uint(clamp(add.r,0.0,1.0)*65535.0));
              atomicAdd(acc[pi+3*T*T],uint(clamp(add.g,0.0,1.0)*65535.0));
              atomicAdd(acc[pi+2*3*T*T],uint(clamp(add.b,0.0,1.0)*65535.0));
            }
          }
          int e2=2*er;if(e2>-dy){er-=dy;x+=sx;}if(e2<dx){er+=dx;y+=u_sign;}
          if((sx>0&&x>=W)||(sx<0&&x<0)||(u_sign>0&&y>=H)||(u_sign<0&&y<0))break;
        }
        st[di*u_rays+ri]=vec4(lt,1.0);
        if(dirOff==0&&rayIdx==0){
          int px=xIn,per=er0,pk=kNow;
          for(int s=0;s<T;s++){
            int e2=2*per;
            if(e2>-dy){per-=dy;px+=sx;pk++;}
            if(e2<dx){per+=dx;}
          }
          ph[di]=ivec2(pk,per);
        }
      }
    }
  }
  barrier();

  int yBase=(u_sign>0)?(u_rowBase*T):(H-1-u_rowBase*T);
  for(int i=tid;i<3*T*T;i+=256){
    if(acc[i]==0u)continue;
    int lx=i%(3*T), ly=i/(3*T);
    int gx=colWin+lx;
    int gy=(u_sign>0)?(yBase+ly):(yBase-ly);
    if(gx<0||gx>=W||gy<0||gy>=H)continue;
    int gpi=gy*W+gx;
    atomicAdd(d[gpi],acc[i]);
    atomicAdd(d[gpi+wh],acc[i+3*T*T]);
    atomicAdd(d[gpi+2*wh],acc[i+2*3*T*T]);
  }
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
uniform sampler2D u_fine;            // 细趟：只在细化块内有效
uniform sampler2DArray u_mask;
uniform int u_maskOn, u_ms;
uniform int u_exact;   // 1 = 光场与画布同尺寸：直接用 texelFetch，保证与原始路径位级一致
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
  // 光场可能被降采样过：按归一化坐标双线性采样（覆盖同样的空间范围）。
  vec3 s;
  bool refined = u_maskOn!=0 && texelFetch(u_mask,ivec3(p/u_ms,u_layer),0).r>0.5;
  if(u_exact!=0)   s=texelFetch(u_scan,p,0).rgb;              // Exact：与原始路径位级一致
  else if(refined) s=texelFetch(u_fine,p,0).rgb;              // 细化块：全分辨率细算
  else             s=texture(u_scan,(vec2(p)+0.5)/vec2(u_cs)).rgb;   // 其余：粗光场双线性放大

  // 实心像素：显示自身 albedo，并**加上**独立的彩色自发光与收集到的光。
  // 两者相加而不是互相混合，所以"暗表面发绿光"这类组合才表达得出来。
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
    // 光场降采样时这张图会被放大采样，必须双线性，否则出现块状台阶
    if (scanTex_) {
        glBindTexture(GL_TEXTURE_2D, scanTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

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

    const char* kScanVer = "#version 430 core\n";
    const char* kScanVerU64 =
        "#version 430 core\n"
        "#extension GL_ARB_gpu_shader_int64 : require\n"
        "#extension GL_NV_shader_atomic_int64 : require\n"
        "#define U64 1\n";

    scanProg_ = createComputeProgram("sweep/scan", (std::string(kScanVer) + kScanBody).c_str());

    // 64 位原子变体：只有显卡/驱动支持 GL_NV_shader_atomic_int64 时才编。
    // 它是"每像素 1 次原子加"里唯一还能保住精度的方案（21/21/21 位）。
    {
        GLint nExt = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &nExt);
        bool hasAtomic64 = false;
        for (GLint i = 0; i < nExt; i++) {
            const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, (GLuint)i));
            if (e && std::strcmp(e, "GL_NV_shader_atomic_int64") == 0) hasAtomic64 = true;
        }
        if (hasAtomic64) {
            scanProgU64_ = createComputeProgram("sweep/scan_u64",
                                                (std::string(kScanVerU64) + kScanBody).c_str());
            copyProgU64_ = createComputeProgram("sweep/copy_u64",
                                                (std::string(kScanVerU64) + kCopyBody).c_str());
            if (!scanProgU64_ || !copyProgU64_) {
                // 编译失败就当不支持，退回 32 位路径（不能让整个引擎起不来）
                if (scanProgU64_) glDeleteProgram(scanProgU64_);
    if (tileProg_) glDeleteProgram(tileProg_);
    if (colStateSSBO_) glDeleteBuffers(1, &colStateSSBO_);
    if (dirPassSSBO_) glDeleteBuffers(1, &dirPassSSBO_);
                if (copyProgU64_) glDeleteProgram(copyProgU64_);
                scanProgU64_ = copyProgU64_ = 0;
    fieldDownProg_ = 0;
    fieldCanvas_ = fieldMat_ = 0;
    fw_ = fh_ = 0;
    tileProg_ = 0;
    colStateSSBO_ = dirPassSSBO_ = 0;
    colCap_ = 0;
                fprintf(stderr, "[sweep] 64 位原子着色器编译失败，pack=3 不可用\n");
            } else {
                // uniform 位置复用前提：两个变体的 active uniform 列表一致。
                // 不假设"驱动一定给同样的位置"，这里显式验一遍；不一致就放弃 pack=3。
                const char* names[] = {"u_pack", "u_cs", "u_cc", "u_no_write", "u_layer"};
                bool same = true;
                for (const char* nm : names) {
                    if (glGetUniformLocation(scanProgU64_, nm) !=
                        glGetUniformLocation(scanProg_, nm))
                        same = false;
                }
                if (!same || glGetUniformLocation(copyProgU64_, "u_pack") !=
                                 glGetUniformLocation(copyProg_, "u_pack")) {
                    glDeleteProgram(scanProgU64_);
                    glDeleteProgram(copyProgU64_);
                    scanProgU64_ = copyProgU64_ = 0;
    fieldDownProg_ = 0;
    fieldCanvas_ = fieldMat_ = 0;
    fw_ = fh_ = 0;
    tileProg_ = 0;
    colStateSSBO_ = dirPassSSBO_ = 0;
    colCap_ = 0;
                    fprintf(stderr, "[sweep] 64 位变体的 uniform 位置与 32 位不一致，pack=3 关闭\n");
                } else {
                    fprintf(stderr, "[sweep] 64 位原子变体就绪（pack=3：1 次原子，21/21/21 位）\n");
                }
            }
        } else {
            fprintf(stderr, "[sweep] 无 GL_NV_shader_atomic_int64，pack=3 不可用\n");
        }
    }

    // 扩散版扫描（B1/B2 实验用）：线程布局不同（一个 workgroup 一个方向），
    // 所以必须是独立 program，不能用同一个 dispatch。
    scanDiffProg_ = createComputeProgram("sweep/scan_diff", kScanDiffComp);
    copyProg_ = createComputeProgram("sweep/copy", (std::string(kScanVer) + kCopyBody).c_str());
    blendProg_ = createComputeProgram("sweep/blend", kBlendComp);
    // 渐进哈希采样（可选路径）
    hashProg_ = createComputeProgram("sweep/hash", kHashGatherComp);
    clearAccumProg_ = createComputeProgram("sweep/clear_accum", kClearAccumComp);
    hashCopyProg_ = createComputeProgram("sweep/hash_copy", kHashCopyComp);
    tileProg_ = createComputeProgram("sweep/scan_col", kScanColComp);
    fieldDownProg_ = createComputeProgram("sweep/field_down", kFieldDownComp);
    rowProg_ = createComputeProgram("sweep/scan_row", kScanRowComp);
    if (!scanProg_ || !scanDiffProg_ || !copyProg_ || !blendProg_ || !hashProg_ ||
        !clearAccumProg_ || !hashCopyProg_ || !tileProg_ || !fieldDownProg_ || !comp_.init()) {
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
    uScan_.noWrite = glGetUniformLocation(scanProg_, "u_no_write");
    uScan_.pack = glGetUniformLocation(scanProg_, "u_pack");
    uScan_.major = glGetUniformLocation(scanProg_, "u_major");
    uScan_.s = glGetUniformLocation(scanProg_, "u_s");
    uScan_.fcs = glGetUniformLocation(scanProg_, "u_fcs");
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
    uCopy_.pack = glGetUniformLocation(copyProg_, "u_pack");
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
    uCol_.cc = glGetUniformLocation(tileProg_, "u_cc");
    uCol_.mat = glGetUniformLocation(tileProg_, "u_mat");
    uCol_.cs = glGetUniformLocation(tileProg_, "u_cs");
    uCol_.ambient = glGetUniformLocation(tileProg_, "u_ambient");
    uCol_.cp = glGetUniformLocation(tileProg_, "u_cp");
    uCol_.emitScale = glGetUniformLocation(tileProg_, "u_emit_scale");
    uCol_.layer = glGetUniformLocation(tileProg_, "u_layer");
    uCol_.T = glGetUniformLocation(tileProg_, "u_T");
    uCol_.col = glGetUniformLocation(tileProg_, "u_colBase");
    uCol_.sign = glGetUniformLocation(tileProg_, "u_sign");
    uCol_.group = glGetUniformLocation(tileProg_, "u_group");
    uCol_.dirCount = glGetUniformLocation(tileProg_, "u_dirCount");
    uCol_.rays = glGetUniformLocation(tileProg_, "u_rays");
    uCol_.invN = glGetUniformLocation(tileProg_, "u_invN");
    uRow_.cc = glGetUniformLocation(rowProg_, "u_cc");
    uRow_.mat = glGetUniformLocation(rowProg_, "u_mat");
    uRow_.cs = glGetUniformLocation(rowProg_, "u_cs");
    uRow_.ambient = glGetUniformLocation(rowProg_, "u_ambient");
    uRow_.cp = glGetUniformLocation(rowProg_, "u_cp");
    uRow_.emitScale = glGetUniformLocation(rowProg_, "u_emit_scale");
    uRow_.layer = glGetUniformLocation(rowProg_, "u_layer");
    uRow_.T = glGetUniformLocation(rowProg_, "u_T");
    uRow_.row = glGetUniformLocation(rowProg_, "u_rowBase");
    uRow_.sign = glGetUniformLocation(rowProg_, "u_sign");
    uRow_.group = glGetUniformLocation(rowProg_, "u_group");
    uRow_.dirCount = glGetUniformLocation(rowProg_, "u_dirCount");
    uRow_.rays = glGetUniformLocation(rowProg_, "u_rays");
    uRow_.invN = glGetUniformLocation(rowProg_, "u_invN");

    setLayerCount(1);
    rebuildDirectionTable();
    loadTuneCache();   // 同一台机器（GL_RENDERER + 尺寸相同）就不必每次重测
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
    if (scanProgU64_) glDeleteProgram(scanProgU64_);
    if (tileProg_) glDeleteProgram(tileProg_);
    if (colStateSSBO_) glDeleteBuffers(1, &colStateSSBO_);
    if (dirPassSSBO_) glDeleteBuffers(1, &dirPassSSBO_);
    if (copyProgU64_) glDeleteProgram(copyProgU64_);
    dn_.shutdown();
    renderTex_ = scanTex_ = 0;
    scanSSBO_ = dirSSBO_ = 0;
    scanProg_ = scanDiffProg_ = copyProg_ = blendProg_ = 0;
    hashProg_ = clearAccumProg_ = 0;
    scanProgU64_ = copyProgU64_ = 0;
    fieldDownProg_ = 0;
    fieldCanvas_ = fieldMat_ = 0;
    fw_ = fh_ = 0;
    tileProg_ = 0;
    colStateSSBO_ = dirPassSSBO_ = 0;
    colCap_ = 0;
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
    // 光场降采样副本（扫描只在这两张上跑）
    fw_ = (w_ + fieldScale_ - 1) / fieldScale_;
    fh_ = (h_ + fieldScale_ - 1) / fieldScale_;
    if (fieldCanvas_) glDeleteTextures(1, &fieldCanvas_);
    if (fieldMat_) glDeleteTextures(1, &fieldMat_);
    fieldCanvas_ = createTextureArray(GL_RGBA8, fw_, fh_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    fieldMat_ = createTextureArray(GL_RGBA8, fw_, fh_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
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

    // 分块路径要用的两半：x 主方向按符号拆两趟（波前方向不同），
    // y 主方向也按 sy 符号拆两趟（按行波前）。
    dirsXPos_.clear();
    dirsXNeg_.clear();
    dirsYPos_.clear();
    dirsYNeg_.clear();
    for (int i = 0; i < dirCount_; i++) {
        const GLint dx = dirs[(size_t)i * 2], dy = dirs[(size_t)i * 2 + 1];
        std::vector<GLint>* dst = nullptr;
        if (std::abs((int)dx) >= std::abs((int)dy))
            dst = (dx > 0) ? &dirsXPos_ : &dirsXNeg_;
        else
            dst = (dy > 0) ? &dirsYPos_ : &dirsYNeg_;
        dst->push_back(dx);
        dst->push_back(dy);
    }
    dirCountY_ = dirCount_ - (int)((dirsXPos_.size() + dirsXNeg_.size()) / 2);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirSSBO_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(dirs.size() * sizeof(GLint)),
                 dirs.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // 分块路径的状态缓冲：[方向][射线] 各一个 vec4（lt + 1）。
    // 只初始化一次为 ambient；第 0 列显式用 ambient，所以不必每帧清。
    paddedRays_ = ((2 * h_ + 1 + 255) / 256) * 256;
    const size_t need = (size_t)paddedRays_ * (size_t)std::max(1, dirCount_);
    if (colCap_ < need) {
        if (colStateSSBO_) glDeleteBuffers(1, &colStateSSBO_);
        glGenBuffers(1, &colStateSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, colStateSSBO_);
        std::vector<float> init(need * 4, 0.0f);
        for (size_t i = 0; i < need; i++) {
            init[i * 4 + 0] = st_.ambient[0];
            init[i * 4 + 1] = st_.ambient[1];
            init[i * 4 + 2] = st_.ambient[2];
            init[i * 4 + 3] = 1.0f;
        }
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(need * 4 * sizeof(float)),
                     init.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        colCap_ = need;
    }
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

// ---------------------------------------------------------------------------
// 分块扫描：x 主方向按列波前（shared 合并），y 主方向仍走主内核
//
// 两段写的是同一套 16 位定点整数，所以最终结果与主内核**逐位相同**
// （整数加法与顺序无关）。验收就是 png_stat 0 像素差。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 自动探测元参数：经典全局原子 vs 分块（几组 T/G），在**真实内容**上取最快
//
// 为什么必须自动：分块的收益强烈依赖硬件（L2 大小、原子吞吐、SM 数），
// 而且"每列并行度 = 行块数 × 方向组数"在弱 GPU 上会先撞到并行度不足。
// 4080 上 T=16/G=16 最好，Intel UHD 上很可能是 T=8 甚至经典路径更快。
//
// 代价：一次性 ~10–30 ms（每组跑 2 帧）。只在第一次有内容时执行。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 调参缓存：按 (GL_RENDERER, 画布尺寸) 匹配才复用。
// 为什么值得做：自动探测要 30–60 ms，而玩家的 GPU/分辨率在同一台机器上是不变的，
// 只有换显卡或改画布尺寸才需要重测。
// 关掉：环境变量 PB_NO_TUNE_CACHE=1（回归测试时用，保证结果不受缓存影响）。
// ---------------------------------------------------------------------------
static const char* kTuneFile = "paintboard_tune.ini";

void LightSweep::loadTuneCache() {
    if (getenv("PB_NO_TUNE_CACHE")) return;
    if (!tileProg_ || !colStateSSBO_) return;
    // 注意用二进制模式：文本模式下 \n 会变成 \r\n，%[^\n] 就会把 \r 也读进 renderer，
    // 后面 strcmp 永远不相等 —— 缓存看起来"写了但永远用不上"。
    FILE* f = fopen(kTuneFile, "rb");
    if (!f) return;
    char renderer[256] = {0};
    int tw = 0, th = 0, tiled = 0, cT = 16, cG = 16, rT = 16, rG = 32;
    const int n = fscanf(f,
                         "renderer=%255[^\n]\nsize=%dx%d\ntiled=%d\ncolT=%d\ncolG=%d\nrowT=%d\n"
                         "rowG=%d\n",
                         renderer, &tw, &th, &tiled, &cT, &cG, &rT, &rG);
    fclose(f);
    if (n != 8) return;
    const char* cur = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    if (!cur || std::strcmp(cur, renderer) != 0 || tw != w_ || th != h_) return;
    // 缓存可能是旧版本（分块默认开启）写的：分块在修好之前不参与，直接压回经典。
    const bool allowTiled = []() { const char* v = getenv("PB_TILED"); return v && v[0] == '1'; }();
    tiledScan_ = (tiled != 0) && allowTiled;
    tileT_ = std::clamp(cT, 8, 32);
    dirGroup_ = std::clamp(cG, 8, 64);
    tileTRow_ = std::clamp(rT, 8, 32);
    dirGroupY_ = std::clamp(rG, 8, 64);
    autotunePending_ = false;
    tuneInfo_ = "cached";
    fprintf(stderr, "[sweep] 调参缓存命中：%s 列(T=%d G=%d) 行(T=%d G=%d)\n",
            tiledScan_ ? "tiled" : "classic", tileT_, dirGroup_, tileTRow_, dirGroupY_);
}

void LightSweep::saveTuneCache() const {
    const char* cur = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    FILE* f = fopen(kTuneFile, "wb");
    if (!f) return;
    fprintf(f, "renderer=%s\nsize=%dx%d\ntiled=%d\ncolT=%d\ncolG=%d\nrowT=%d\nrowG=%d\n",
            cur ? cur : "?", w_, h_, tiledScan_ ? 1 : 0, tileT_, dirGroup_, tileTRow_,
            dirGroupY_);
    fclose(f);
}
void LightSweep::autotune(int layer) {
    if (!autotunePending_) return;
    autotunePending_ = false;
    if (!valid() || dirCount_ <= 0 || !tileProg_ || !colStateSSBO_) return;

    GLint maxShared = 0, maxWg = 1024;
    glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, &maxShared);
    // GL_MAX_COMPUTE_WORK_GROUP_SIZE 是带下标的，必须 glGetIntegeri_v；
    // 用 glGetIntegerv 查它会留下 GL_INVALID_ENUM，后面回读纹理就会莫名失败。
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxWg);
    auto sharedFor = [](int T) { return 3 * 3 * T * T * 4; };   // 3T×T×3 平面

    auto timeOnce = [&]() {
        glFinish();
        const auto t0 = std::chrono::steady_clock::now();
        renderLayer(layer);
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };

    const auto tStart = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         tStart)
            .count();
    };
    // 时间预算。慢 GPU（比如 Intel UHD）单层就要 30–70 ms，12 个候选 × 4 次渲染
    // 会变成 3–5 秒的启动卡顿。所以先量一次经典路径当"标尺"：
    //   快卡 -> 全候选 + 多次计时取最小；
    //   慢卡 -> 只试 3 个最可能的配置 + 每档单次计时，总时长压到 ~0.3 秒级。
    const double kBudgetMs = 700.0;

    struct Cand {
        bool tiled;
        int T;
        int G;
    };
    std::vector<Cand> cands;
    cands.push_back({false, 16, 16});   // 占位：第 0 项 = 经典路径（下面单独量）

    bool savedTiled = tiledScan_;
    const int savedT = tileT_, savedG = dirGroup_;
    tiledScan_ = false;
    timeOnce();   // 预热
    const double tClassic = timeOnce();
    const bool slowGpu = (tClassic > 12.0);

    // ⚠️ 分块路径目前**默认不参与**自动选择：它在"大面积纯色"上会出现横向带纹
    // （行内核带边界），而且与经典路径的绝对值对不上（实测差 2.4×）。
    // 在修好之前，自动选择只在经典路径上挑参数；分块仍可由调用方显式开启
    // （setTiledScan(true)）或 setenv PB_TILED=1 参与候选。
    const bool allowTiled = []() {
        const char* v = getenv("PB_TILED");
        return v && v[0] == '1';
    }();
    if (!slowGpu && allowTiled) {
        const int Ts[] = {8, 16, 32};
        const int Gs[] = {8, 16, 32};
        for (int T : Ts) {
            if (maxShared > 0 && sharedFor(T) > maxShared) continue;
            for (int G : Gs) {
                if (G > maxWg) continue;
                cands.push_back({true, T, G});
            }
        }
    } else if (allowTiled && (maxShared <= 0 || sharedFor(16) <= maxShared)) {
        // T=8 在两种卡上实测都最差，直接不试
        cands.push_back({true, 16, 8});
        cands.push_back({true, 16, 16});
        cands.push_back({true, 16, 32});
    }

    char buf[256];
    std::string detail;
    double best = tClassic;
    Cand bestCand{false, 16, 16};
    std::snprintf(buf, sizeof(buf), "classic: %.2fms%s", tClassic,
                  slowGpu ? "（慢卡：精简候选）" : "");
    detail += buf;

    for (size_t i = 1; i < cands.size(); i++) {
        if (elapsed() > kBudgetMs) {
            detail += "; …(时间预算用尽)";
            break;
        }
        const Cand& c = cands[i];
        tiledScan_ = c.tiled;
        tileT_ = c.T;
        dirGroup_ = c.G;
        timeOnce();   // 预热
        double ms = timeOnce();
        if (!slowGpu) ms = std::min(ms, timeOnce());
        std::snprintf(buf, sizeof(buf), "; tile T=%d G=%d: %.2fms", c.T, c.G, ms);
        detail += buf;
        if (ms < best) {
            best = ms;
            bestCand = c;
        }
    }

    tiledScan_ = bestCand.tiled;
    if (bestCand.tiled) {
        tileT_ = bestCand.T;
        dirGroup_ = bestCand.G;
    } else {
        tileT_ = savedT;
        dirGroup_ = savedG;
    }

    // 阶段 B：行内核参数（同样受预算约束）
    double bestRow = 1e30;
    if (tiledScan_) {
        tileTRow_ = 16;
        dirGroupY_ = 16;
        timeOnce();
        bestRow = timeOnce();
        const int TRs[] = {8, 32};
        const int GYs[] = {8, 32, 64};
        int bestTR = tileTRow_, bestGY = dirGroupY_;
        for (int tr : TRs) {
            if (maxShared > 0 && sharedFor(tr) > maxShared) continue;
            for (int gy : GYs) {
                if (gy > maxWg) continue;
                if (elapsed() > kBudgetMs * 2.0) break;
                tileTRow_ = tr;
                dirGroupY_ = gy;
                timeOnce();
                double ms = timeOnce();
                if (!slowGpu) ms = std::min(ms, timeOnce());
                std::snprintf(buf, sizeof(buf), "; row T=%d G=%d: %.2fms", tr, gy, ms);
                detail += buf;
                if (ms < bestRow) {
                    bestRow = ms;
                    bestTR = tr;
                    bestGY = gy;
                }
            }
        }
        tileTRow_ = bestTR;
        dirGroupY_ = bestGY;
    }

    std::snprintf(buf, sizeof(buf),
                  "autotune -> %s 列(T=%d G=%d) 行(T=%d G=%d)；单层 %s %.2f ms；候选: %s",
                  tiledScan_ ? "tiled" : "classic", tileT_, dirGroup_, tileTRow_, dirGroupY_,
                  tiledScan_ ? "tiled" : "classic", tiledScan_ ? bestRow : best, detail.c_str());
    tuneInfo_ = buf;
    saveTuneCache();
    fprintf(stderr, "[sweep] %s\n", tuneInfo_.c_str());
    (void)savedTiled;
}
// 按当前 fieldScale_ 保证 (fw_,fh_) 的光场副本存在。懒执行：setFieldScale 可能在
// init 之后才被调用（CLI/面板），所以不能只在 setLayerCount 里分配。
void LightSweep::ensureFieldTextures() {
    const int s = std::clamp(fieldScale_, 1, 4);
    const int fw = (w_ + s - 1) / s;
    const int fh = (h_ + s - 1) / s;

    // ⚠️ scanTex_ 的**像素尺寸必须等于光场尺寸**：blend 用归一化 UV 采样它，
    // 尺寸不一致时会采到没写过的区域（实测 s=4 时整幅画变亮、最大差 248 级）。
    if (!scanTex_ || fw != scanW_ || fh != scanH_) {
        if (scanTex_) glDeleteTextures(1, &scanTex_);
        scanTex_ = createTexture(GL_RGBA16F, fw, fh, GL_RGBA, GL_HALF_FLOAT);
        glBindTexture(GL_TEXTURE_2D, scanTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        scanW_ = fw;
        scanH_ = fh;
    }

    if (s == 1) return;
    // 层数变了也必须重建：数组纹理只有 fieldLayers_ 层，写第 5 层会越界（表现为整片噪点）。
    if (fieldCanvas_ && fw == fw_ && fh == fh_ && fieldLayers_ == layers_) return;
    if (fieldCanvas_) glDeleteTextures(1, &fieldCanvas_);
    if (fieldMat_) glDeleteTextures(1, &fieldMat_);
    fw_ = fw;
    fh_ = fh;
    fieldCanvas_ = createTextureArray(GL_RGBA8, fw_, fh_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    fieldMat_ = createTextureArray(GL_RGBA8, fw_, fh_, layers_, GL_RGBA, GL_UNSIGNED_BYTE);
    fieldMask_ = createTextureArray(GL_R8, fw_, fh_, layers_, GL_RED, GL_UNSIGNED_BYTE);
    fineField_ = createTexture(GL_RGBA16F, w_, h_, GL_RGBA, GL_HALF_FLOAT);
    fieldLayers_ = layers_;
    // fineField_ 可能被 setFieldQuality 删掉了（它不在 fw_/fh_ 的比较里），这里补建
    if (!fineField_) {
        fineField_ = createTexture(GL_RGBA16F, w_, h_, GL_RGBA, GL_HALF_FLOAT);
        glBindTexture(GL_TEXTURE_2D, fineField_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}
// 诊断：把 fieldMask_ 读回来，统计每层细化块占比。
// 这不是"可有可无的调试代码"——掩码错了和细趟错了症状很像（都表现为整幅偏亮），
// 只有这个数字能一眼分辨。
void LightSweep::debugMaskStats(const char* tag) {
    if (!fieldMask_ || fw_ <= 0 || fh_ <= 0) {
        fprintf(stderr, "[mask] %s: 没有掩码（fieldScale=%d）\n", tag, fieldScale_);
        return;
    }
    const size_t n = (size_t)fw_ * (size_t)fh_ * (size_t)layers_;
    std::vector<uint8_t> buf(n, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fieldMask_);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    const size_t per = (size_t)fw_ * (size_t)fh_;
    for (int l = 0; l < layers_; l++) {
        size_t cnt = 0;
        for (size_t i = 0; i < per; i++)
            if (buf[l * per + i] > 127) ++cnt;
        fprintf(stderr, "[mask] %s 层%d: %zu/%zu 块需要细化（%.1f%%）\n", tag, l, cnt, per,
                100.0 * (double)cnt / (double)per);
    }
}
void LightSweep::debugFieldCompare(const char* tag, int layer) {
    if (!fieldMask_ || !fineField_ || !scanTex_ || fw_ <= 0) return;
    std::vector<uint8_t> mask((size_t)fw_ * fh_ * layers_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fieldMask_);
    glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RED, GL_UNSIGNED_BYTE, mask.data());
    std::vector<float> coarse((size_t)w_ * h_ * 4);
    glBindTexture(GL_TEXTURE_2D, scanTex_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, coarse.data());
    std::vector<float> fine((size_t)w_ * h_ * 4);
    glBindTexture(GL_TEXTURE_2D, fineField_);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, fine.data());
    const int s = std::max(1, fieldScale_);
    const int mw = (w_ + s - 1) / s;
    double sumC = 0, sumF = 0;
    long n = 0;
    for (int y = 0; y < h_; y++)
        for (int x = 0; x < w_; x++) {
            const int mb = (y / s) * mw + (x / s);
            // 掩码是逐层的：这里只看最后一层（scanTex_/fineField_ 里留的也是最后一层）
            const int li = (layer >= 0 && layer < layers_) ? layer : layers_ - 1;
            const size_t mo = (size_t)li * (size_t)fw_ * (size_t)fh_;
            if (!(mask[mo + mb] > 127)) continue;
            const size_t i = ((size_t)y * w_ + x) * 4;
            // 粗场是 fw×fh：用同一 UV 取最近格
            const size_t ci = ((size_t)(y / s) * fw_ + (x / s)) * 4;
            sumC += coarse[ci] + coarse[ci + 1] + coarse[ci + 2];
            sumF += fine[i] + fine[i + 1] + fine[i + 2];
            ++n;
        }
    fprintf(stderr, "[fieldcmp] %s 细化像素 %ld 粗场均 %.4f 细场均 %.4f 比值 %.3f\n", tag, n,
            n ? sumC / (3.0 * n) : 0.0, n ? sumF / (3.0 * n) : 0.0,
            (n && sumC > 0) ? sumF / sumC : 0.0);
}
// 把这一层的 canvas/材质降采样到 (fw_,fh_)：扫描只在降采样副本上跑。
// 合成阶段仍然用全分辨率 albedo/自发光，所以锐度不受影响。
void LightSweep::downsampleField(int layer) {
    if (fieldScale_ <= 1 || !fieldDownProg_ || !fieldCanvas_ || !fieldMat_) return;
    glUseProgram(fieldDownProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
    glUniform1i(glGetUniformLocation(fieldDownProg_, "u_cc"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
    glUniform1i(glGetUniformLocation(fieldDownProg_, "u_mat"), 1);
    glUniform2i(glGetUniformLocation(fieldDownProg_, "u_fcs"), fw_, fh_);
    glUniform2i(glGetUniformLocation(fieldDownProg_, "u_cs"), w_, h_);
    glUniform1i(glGetUniformLocation(fieldDownProg_, "u_s"), fieldScale_);
    glUniform1i(glGetUniformLocation(fieldDownProg_, "u_layer"), layer);
    glBindImageTexture(0, fieldCanvas_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(1, fieldMat_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glBindImageTexture(2, fieldMask_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R8);
    glDispatchCompute((GLuint)((fw_ + 7) / 8), (GLuint)((fh_ + 7) / 8), 1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glUseProgram(0);
}
void LightSweep::renderLayerTiled(int layer) {
    const int T = std::clamp(tileT_, 8, 32);
    const int tilesX = (w_ + T - 1) / T;
    const int nBlocks = (h_ + T - 1) / T + 2;   // 块号 B ∈ [-1, ceil(H/T)]
    const int rays = paddedRays_ > 0 ? paddedRays_ : ((2 * h_ + 1 + 255) / 256) * 256;
    const float invN = 1.0f / (float)std::max(1, dirCount_);

    // 1) 累积缓冲清零 + 绑定
    // 光场降采样：SSBO 只需覆盖 (fw_,fh_)，扫描成本按 1/s^2 下降
    downsampleField(layer);
    const int fW = (fieldScale_ > 1) ? fw_ : w_;
    const int fH = (fieldScale_ > 1) ? fh_ : h_;
    const GLsizeiptr accBytes = (GLsizeiptr)((size_t)fW * fH * 3 * sizeof(GLuint));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, scanSSBO_);
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, accBytes, GL_RED_INTEGER,
                         GL_UNSIGNED_INT, nullptr);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, scanSSBO_);

    if (!dirPassSSBO_) {
        glGenBuffers(1, &dirPassSSBO_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirPassSSBO_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(65535u * 2u * sizeof(GLint)), nullptr,
                     GL_DYNAMIC_DRAW);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, colStateSSBO_);

    glUseProgram(tileProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
    glUniform1i(uCol_.cc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
    glUniform1i(uCol_.mat, 1);
    glUniform2i(uCol_.cs, w_, h_);
    glUniform3fv(uCol_.ambient, 1, st_.ambient);
    glUniform1f(uCol_.cp, st_.propagate);
    glUniform1f(uCol_.emitScale, st_.emitScale);
    glUniform1i(uCol_.layer, layer);
    glUniform1i(uCol_.T, T);
    glUniform1i(uCol_.rays, rays);
    glUniform1f(uCol_.invN, invN);
    const int G = dirGroup_;
    glUniform1i(uCol_.group, G);

    // 2) x 主方向：两趟（sx>0 左->右，sx<0 右->左），每趟按列波前
    for (int pass = 0; pass < 2; pass++) {
        const std::vector<GLint>& list = (pass == 0) ? dirsXPos_ : dirsXNeg_;
        const int n = (int)(list.size() / 2);
        if (n <= 0) continue;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirPassSSBO_);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)(list.size() * sizeof(GLint)),
                        list.data());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirPassSSBO_);
        glUniform1i(uCol_.dirCount, n);
        glUniform1i(uCol_.sign, (pass == 0) ? 1 : -1);
        const int groups = (n + G - 1) / G;
        for (int c = 0; c < tilesX; c++) {
            glUniform1i(uCol_.col, c);
            glDispatchCompute((GLuint)nBlocks, (GLuint)groups, 1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
        }
    }

    // 3) y 主方向：主内核只跑这一类（归一化仍是 1/D_total）
    // 3) y 主方向：转置内核（按行波前），sy>0 / sy<0 两趟
    if (rowProg_ && (dirsYPos_.size() > 0 || dirsYNeg_.size() > 0) &&
        !getenv("PB_NO_YMAJOR")) {
        if (!rowPhaseSSBO_) {
            glGenBuffers(1, &rowPhaseSSBO_);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, rowPhaseSSBO_);
            glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(65535u * 2u * sizeof(GLint)),
                         nullptr, GL_DYNAMIC_DRAW);
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, rowPhaseSSBO_);
        glUseProgram(rowProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
        glUniform1i(uRow_.cc, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
        glUniform1i(uRow_.mat, 1);
        glUniform2i(uRow_.cs, w_, h_);
        glUniform3fv(uRow_.ambient, 1, st_.ambient);
        glUniform1f(uRow_.cp, st_.propagate);
        glUniform1f(uRow_.emitScale, st_.emitScale);
        glUniform1i(uRow_.layer, layer);
        const int TR = std::clamp(tileTRow_, 8, 32);
        const int GY = std::max(1, dirGroupY_);
        glUniform1i(uRow_.T, TR);
        glUniform1i(uRow_.rays, ((2 * w_ + 1 + 255) / 256) * 256);   // y 主方向是 2W+1 条
        glUniform1f(uRow_.invN, invN);
        glUniform1i(uRow_.group, GY);
        const int nBlocksX = (w_ + T - 1) / T + 2;
        const int bandsY = (h_ + TR - 1) / TR;
        for (int pass = 0; pass < 2; pass++) {
            const std::vector<GLint>& list = (pass == 0) ? dirsYPos_ : dirsYNeg_;
            const int n = (int)(list.size() / 2);
            if (n <= 0) continue;
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, dirPassSSBO_);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                            (GLsizeiptr)(list.size() * sizeof(GLint)), list.data());
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirPassSSBO_);
            glUniform1i(uRow_.dirCount, n);
            glUniform1i(uRow_.sign, (pass == 0) ? 1 : -1);
            const int groups = (n + GY - 1) / GY;
            for (int b = 0; b < bandsY; b++) {
                glUniform1i(uRow_.row, b);
                glDispatchCompute((GLuint)nBlocksX, (GLuint)groups, 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
            }
        }
    } else if (dirCountY_ > 0 && !getenv("PB_NO_YMAJOR")) {
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
        glUniform1i(uScan_.band, 0);
        glUniform1i(uScan_.bandn, dirCount_);   // 归一化按 D_total
        glUniform1i(uScan_.contig, 0);
        glUniform1i(uScan_.noWrite, 0);
        glUniform1i(uScan_.pack, 0);
        glUniform1i(uScan_.major, 2);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, scanSSBO_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirSSBO_);
        const int chunks = (2 * std::max(w_, h_) + 1 + 255) / 256;
        glDispatchCompute((GLuint)chunks, 1, (GLuint)ndirs);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        glUniform1i(uScan_.major, 0);           // 复位，别污染别的路径
    }

    // 4) SSBO -> scanTex_ -> 合成（与主路径相同）
    glUseProgram(copyProg_);
    glUniform2i(uCopy_.cs, fW, fH);
    glUniform1i(uCopy_.pack, 0);
    glBindImageTexture(0, scanTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute((GLuint)((fW + 7) / 8), (GLuint)((fH + 7) / 8), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    blendLayer(layer);
    glUseProgram(0);
}

void LightSweep::renderLayer(int layer) {
    if (!valid() || layer < 0 || layer >= layers_) return;
    ensureDirectionTable();
    if (dirCount_ <= 0) return;
    if (progressive_) { renderLayerProgressive(layer); return; }
    // 分块路径：x 主方向走列波前 + shared 合并，y 主方向仍走主内核。
    // 两条路径写的是同一套整数定点值，所以结果逐位相同。
    ensureFieldTextures();
    if (autotunePending_ && layer == 0) autotune(layer);
    if (tiledScan_ && tileProg_ && colStateSSBO_ && fieldScale_ <= 1) {
        renderLayerTiled(layer);
        return;
    }

    // --- 1. 在 GPU 端清零累积缓冲（省掉每帧 CPU->GPU 上传） ---
    // 光场降采样：SSBO 只需覆盖 (fw_,fh_)，扫描成本按 1/s^2 下降
    downsampleField(layer);
    const int fW = (fieldScale_ > 1) ? fw_ : w_;
    const int fH = (fieldScale_ > 1) ? fh_ : h_;
    const GLsizeiptr accBytes = (GLsizeiptr)((size_t)fW * fH * 3 * sizeof(GLuint));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, scanSSBO_);
    glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, accBytes, GL_RED_INTEGER,
                         GL_UNSIGNED_INT, nullptr);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, scanSSBO_);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirSSBO_);

    // --- 2. 一次 dispatch 覆盖所有方向（z 维 = 方向索引） ---
    // 诊断用的并行度上限：只跑前 n 个方向。射线长度不变，只有并行度变了。
    const int ndirs = (dirLimit_ > 0 && dirLimit_ < dirCount_) ? dirLimit_ : dirCount_;
    // pack=3 = 64 位原子（21/21/21）。着色器没编出来（驱动不支持）就退回 2 原子路径。
    const bool useU64 = (atomicPack_ == 3 && scanProgU64_ != 0 && copyProgU64_ != 0);
    const int effPack = useU64 ? 3 : std::min(atomicPack_, 2);
    glUseProgram(useU64 ? scanProgU64_ : scanProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
    glUniform1i(uScan_.cc, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
    glUniform1i(uScan_.mat, 1);
    glUniform2i(uScan_.cs, w_, h_);   // 射线是全分辨率
    glUniform1i(uScan_.s, (fieldScale_ > 1) ? fieldScale_ : 1);
    glUniform2i(uScan_.fcs, fW, fH);
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
    glUniform1i(uScan_.noWrite, scanNoWrite_ ? 1 : 0);
    glUniform1i(uScan_.pack, effPack);
    glUniform1i(uScan_.major, 0);

    // 需要的射线数上限（与 kernel 里 rc 的定义一致）：
    //   dx>=dy 时射线横向走 -> 2*H+1 条；否则纵向走 -> 2*W+1 条。
    // 旧实现直接用了 2*max(W,H)+1（对两种情形都够大），这里保持一致。
    const int chunks = (2 * std::max(fW, fH) + 1 + 255) / 256;

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

    // 光场降采样时先关掉扩散：细趟用的是普通扫描内核，两条路径混在一起会在块边界
    // 出现明显接缝（实测块状台阶）。关掉扩散后粗/细两趟口径一致。
    if (diffuseEnable_ && diffuse_ > 0.0f && fieldScale_ <= 1) {
        glUseProgram(scanDiffProg_);
        glUniform1i(uScan_.cc_d, 0);
        glUniform1i(uScan_.mat_d, 1);
        glUniform2i(uScan_.cs_d, fW, fH);   // 必须与降采样后的光场一致
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
        // 双缓冲 carry：每个方向的射线数上限（补齐到 256 的倍数，和 kernel 的 rc 一致）
        const int hrays = ((2 * std::max(w_, h_) + 1 + 255) / 256) * 256;
        glUniform1i(glGetUniformLocation(scanDiffProg_, "u_hrays"), hrays);
        glUniform1i(uScan_.diffdbg, diffuseDbg_);
        // 暂存：[方向][射线] 各一个 vec4。按需扩容，避免每帧重分配。
        {
            // 两份缓冲交替（双缓冲交换），每份 ndirs × hrays 个 vec4。
            const size_t need = (size_t)ndirs * (size_t)hrays * 2;
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
    glUseProgram(useU64 ? copyProgU64_ : copyProg_);
    glUniform2i(uCopy_.cs, w_, h_);
    glUniform1i(uCopy_.pack, effPack);
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

    // --- 细趟：全分辨率扫描，但只在"细化块"内取样与沉积 ---
    //
    // 粗趟用块平均覆盖率表示遮挡，薄墙会把光挡得太狠（实测整幅暗 30%）。
    // 这一趟按掩码筛一遍：命中的块按 1px 精度重算，未命中的块连取样都跳过，
    // 代价 ≈ 细化块占比（实测 neon-rain 4.6% 边缘块 + 3.6% 光源块）。
    // 只有 Adaptive 档才跑细趟（Coarse 档就是纯粗光场，Exact 档连粗光场都没有）。
    if (refine_ && fieldMask_ && fineField_) {
        const GLsizeiptr fullBytes = (GLsizeiptr)((size_t)w_ * h_ * 3 * sizeof(GLuint));
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, scanSSBO_);
        glClearBufferSubData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, 0, fullBytes, GL_RED_INTEGER,
                             GL_UNSIGNED_INT, nullptr);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, scanSSBO_);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, dirSSBO_);

        glUseProgram(useU64 ? scanProgU64_ : scanProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, canvasArray_);
        glUniform1i(uScan_.cc, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, matArray_);
        glUniform1i(uScan_.mat, 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, fieldMask_);
        glUniform1i(glGetUniformLocation(useU64 ? scanProgU64_ : scanProg_, "u_mask"), 2);
        glUniform1i(glGetUniformLocation(useU64 ? scanProgU64_ : scanProg_, "u_maskOn"), 1);
        glUniform1i(glGetUniformLocation(useU64 ? scanProgU64_ : scanProg_, "u_ms"), fieldScale_);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D_ARRAY, fieldCanvas_);
        glUniform1i(glGetUniformLocation(useU64 ? scanProgU64_ : scanProg_, "u_ccd"), 5);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D_ARRAY, fieldMat_);
        glUniform1i(glGetUniformLocation(useU64 ? scanProgU64_ : scanProg_, "u_matd"), 6);
        glUniform1i(glGetUniformLocation(useU64 ? scanProgU64_ : scanProg_, "u_bs"), fieldScale_);
        glUniform2i(uScan_.cs, w_, h_);
        glUniform1i(uScan_.s, 1);
        glUniform2i(uScan_.fcs, w_, h_);
        glUniform3fv(uScan_.ambient, 1, st_.ambient);
        glUniform1i(uScan_.ndirs, ndirs);
        glUniform1f(uScan_.cp, st_.propagate);
        glUniform1f(uScan_.emitScale, st_.emitScale);
        glUniform1i(uScan_.layer, layer);
        glUniform1i(uScan_.pack, effPack);
        glUniform1i(uScan_.major, 0);
        glUniform1i(uScan_.band, dirBand_);
        glUniform1i(uScan_.bandn, bandn);
        const int fullChunks = (2 * std::max(w_, h_) + 1 + 255) / 256;
        const int chunkBytesF = sizeof(GLuint) * 2 * 256 * 3;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, scanSSBO_);
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 2, scanSSBO_, 0,
                          (GLsizeiptr)fullChunks * chunkBytesF);
        glDispatchCompute((GLuint)fullChunks, 1, (GLuint)ndirs);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

        glUseProgram(copyProg_);
        glUniform2i(uCopy_.cs, w_, h_);
        glUniform1i(uCopy_.pack, 0);
        glBindImageTexture(0, fineField_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute((GLuint)((w_ + 7) / 8), (GLuint)((h_ + 7) / 8), 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    }
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
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, fineField_ ? fineField_ : scanTex_);
    glUniform1i(glGetUniformLocation(blendProg_, "u_fine"), 3);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fieldMask_ ? fieldMask_ : canvasArray_);
    glUniform1i(glGetUniformLocation(blendProg_, "u_mask"), 4);
    const bool haveRefine = (refine_ && fieldMask_ && fineField_);
    glUniform1i(glGetUniformLocation(blendProg_, "u_maskOn"), haveRefine ? 1 : 0);
    glUniform1i(glGetUniformLocation(blendProg_, "u_exact"), fieldScale_ == 1 ? 1 : 0);
    glUniform1i(glGetUniformLocation(blendProg_, "u_ms"), fieldScale_);
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
