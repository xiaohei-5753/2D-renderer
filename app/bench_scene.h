// app/bench_scene.h — 实时性能基准 / 渲染器视觉测试集
//
// ## 设计目标（两件事同时成立）
//
//  1. **能测** —— 每个场景针对性地压某一类视觉特征，让"不同视觉效果下的
//     渲染器表现"可以被量化比较（配合 tools/dir_probe 的误差放大图）。
//  2. **好看** —— 构图能一眼看懂，能当作品展示。不是一堆随机粒子。
//
// ## 与旧版的关系
//
// 原来的"5 层动画基准"保留为 `legacy`，保证历史性能数字仍可复现。
// 新场景共用同一份 buildDoc/stepDoc，`bench_view` 与 `paintboard --bench-scene`
// 依然跑同一份代码 —— 这个约束不能破，否则两边数字不可比。
//
// ## 硬约束（改代码前必读）
//
//  * **确定性**：一切随时间变化的量都写成 f(frame)，只用 sinf / 取模 / 坐标
//    哈希。禁 rand()、time()、未初始化状态。同一帧号在任何设备上一致。
//  * **每层每帧都要被碰一次**，哪怕只改 1 个像素 —— 这样光照的脏层优化失效，
//    量到的才是"每帧把 N 层全部重算"的真实上限。
//  * **CPU 侧每帧代价 O(像素)**，禁止 O(像素²)。
//  * **不走 History**：直接 setPixelRaw。否则逐帧快照会让 CPU 成为瓶颈。
//  * ⚠️ `Layer::setPixelRaw` **没有边界检查**（直接 data[y*w+x] 就写）。
//    所有坐标必须自己夹紧 —— 负坐标 / 越界会踩坏内存。统一用 .cpp 里的 pset()。
#pragma once

#include <string>

namespace pb {

class Document;

namespace bench {

// 场景清单。每个场景一句话说明"测什么"。
enum class SceneId {
    NeonCity,      // 1px 霓虹锐度 + 大面积泛光 + 垂直自发光传输
    LanternFog,    // 垂直光路：光要穿过上方多层半透明雾
    StainedGlass,  // 半透明染色 + 硬阴影 + 彩色光互染
    Chromatic,     // 自发光与 albedo 解耦（"暗表面发绿光"）
    GridLab,       // 定量诊断：点/线/网格/方块（dir_probe 专用，静止）
    Legacy,        // 旧的 5 层动画基准（保持历史数字可复现）
    LowEnd,        // 低配档：单层 + R=32 + 扩散 w=0.1/K=8（集显可玩）
    Count
};

struct SceneDesc {
    const char* name;     // 命令行用的名字
    const char* purpose;  // 一句话"测什么"，会印在 HUD 上
    int layers;           // 推荐层数
    int radius;           // 推荐方向表半径（越小越快、辐条越明显）
    float recW;           // 推荐 diffusion w（0 = 建议关闭）
    int recK;             // 推荐 diffusionStep
    bool ptOnly;          // 只在路径追踪模式下有意义（扫描 GI 不支持折射）
    // 非 0 = 这是个"预设"：层数/半径/扩散会被一起定死（显式命令行参数仍优先）。
    // 有些档位的意义就在于**几个参数一起**，单独给一个没意义 ——
    // 比如 lowend 必须是"单层 + R=32 + 扩散"，拆开就不是低配档了。
    bool preset;
};

// 场景表（定义在 .cpp 里）
extern const SceneDesc kScenes[(int)SceneId::Count];

// 按名字找场景；找不到返回 false。"diffscene" 是 "grid-lab" 的别名。
bool sceneByName(const std::string& name, SceneId& out);
const char* sceneName(SceneId id);

// --- 主入口 ---
//
// 核心逻辑只依赖 `Document`，不依赖 `App` —— 所以 paintboard 的
// `--bench-scene` 和独立的显示程序 tools/bench_view 用的是**同一份**场景代码，
// 两边的画面与负载必然一致。
void buildDoc(Document& doc, SceneId id, int layers);
void stepDoc(Document& doc, SceneId id, int frame);

}  // namespace bench
}  // namespace pb
