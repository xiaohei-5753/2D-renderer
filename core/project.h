// core/project.h — 工程文件（.pbproj）：把"作品 + 界面 + 工具设置"整体存取
//
// ## 设计目标
//
// 一个文件装下**让下次打开时的一切都跟关掉时一样**所需要的全部东西：
//
//   * 画布：每个图层的像素（albedo + 逐像素材质）、名字、可见性、不透明度、锁定
//   * 调色板：自定义色 + 每色的材质 + 当前选中项
//   * 工具：当前工具、笔刷大小/形状、抖动与加减光强度、前景色与材质
//   * 界面：左右栏宽度与开合、缩放、相机偏移、像素网格
//   * 光照：整份 LightSettings（半径 / 环境光 / 自发光倍率 / 沿程增益 / 方向扩散）
//
// 刻意**不存**的：撤销栈、缓存、缩略图、GPU 句柄 —— 这些都是会话内的东西。
//
// ## 为什么不用 PNG
//
// 本仓库只有 stb_image_write（**只能写**），没有 stb_image。工程文件必须能
// 读回来，所以自己定了一个带游程压缩的二进制容器（见 core/rle.h）。
//
// ## 格式（小端）
//
//   ---- 头 24 字节 ----
//     u32 magic      'P','B','P','J'
//     u32 version    当前 1
//     u32 width, height
//     u32 layerCount
//     u32 activeLayer
//
//   ---- 每个图层 ----
//     u16 nameLen, name[utf8]
//     u8  flags       bit0=可见 bit1=锁定
//     u8  opacity
//     u32 rawBytes    未压缩的 albedo 字节数（= w*h*4，用来校验）
//     u32 compBytes   压缩后的长度
//     u8  comp[compBytes]      RLE(albedo)
//     u32 rawBytes2, u32 compBytes2
//     u8  comp2[compBytes2]    RLE(material)
//
//   ---- 调色板 ----
//     u32 count
//     每项：u8 r,g,b   u8 eR,eG,eB   u8 a
//     u32 currentIndex
//
//   ---- 工具 ----
//     u32 toolId, color(rgba), material(4B), brushSize, brushRound, shapeFilled,
//     ditherLevel, shadeAmount
//
//   ---- 界面 ----
//     f32 leftWidth, rightWidth, zoom, camX, camY
//     u8  leftOpen, rightOpen, showPixelGrid
//
//   ---- 光照 ----
//     u32 rayRadius
//     f32 ambient[3], emitScale, propagate
//     u8  diffusionOn
//     f32 diffusion
//     u32 diffusionStep
//     f32 fieldStrength
//     u32 lightMode   保留字段：总是写 0，读时忽略（路径追踪已于 2026-10-19 移除）
//
// ## 健壮性
//
// 工程文件是**外部输入**，可能被截断、手改、或来自更新的版本。所有读取都
// 检查边界，任何异常都返回 false 而不是崩。`version > kProjectVersion` 时
// 明确拒绝（宁可说"这个文件太新"也不猜格式）。
//
// ## 兼容性纪律
//
// 以后加字段**只能往尾部追加**，并且要提升 version（旧版本读新文件会因
// version 检查而拒绝，这是对的）。不要改动已有字段的顺序或类型。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pb {

class Document;
class Palette;
struct ToolOptions;

namespace gfx {
struct LightSettings;
}

// 相机 / 视图状态。
struct ViewState {
    float zoom = 1.0f;
    float camX = 0.0f, camY = 0.0f;
};

// 界面布局。
struct UiState {
    float leftWidth = 236.0f;
    float rightWidth = 322.0f;
    bool leftOpen = true;
    bool rightOpen = true;
    bool showPixelGrid = false;
};

// 光照之外的"会话设置"集合。把工程文件要存的东西集中到一处，
// 免得 save/load 变成一长串参数（那样最容易漏项且难查）。
struct ProjectState {
    Document* doc = nullptr;
    Palette* palette = nullptr;
    ToolOptions* tool = nullptr;
    ViewState* view = nullptr;
    UiState* ui = nullptr;
    gfx::LightSettings* light = nullptr;
    // 历史字段（0 = sweep, 1 = path trace）。路径追踪已于 2026-10-19 移除，
    // 但**为了文件格式兼容必须保留**：保存时固定写 0，读取时忽略该值。
    // 不要删字段、也不要提升 kProjectVersion。
    int lightMode = 0;
    float fieldStrength = 1.0f;
};

// 当前格式版本。加字段时 +1。
constexpr unsigned kProjectVersion = 1;

// 保存 / 读取。成功返回 true。
// 失败时打印具体原因（哪个字段越界），方便定位损坏的文件。
bool saveProject(const std::string& path, const ProjectState& st);
bool loadProject(const std::string& path, ProjectState& st);

// 供测试用：把工程序列化到内存 / 从内存恢复（不碰文件系统）。
// 单元测试靠这两个验证"存了再读，数据逐字节相同"。
bool projectToBytes(const ProjectState& st, std::vector<uint8_t>& out);
// 注意：读取会**写入** st 指向的各处状态，所以这里必须是非 const 引用。
bool projectFromBytes(const std::vector<uint8_t>& in, ProjectState& st);

}  // namespace pb
