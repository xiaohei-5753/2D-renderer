// core/tool.h — 绘图工具
//
// 工具的职责只是"把像素写进一个图层"，不碰历史记录、不碰渲染。
// 一次操作的生命周期：begin() -> update()* -> end()，由 app 配合 History 完成撤销。
//
// 参数类型是 core/layer.h 的 Layer（可绘制表面），不是 Document ——
// 工具永远只作用在**当前图层**上，图层以外的编排（哪个层、要不要记撤销）由 app 负责。
//
// 形状类工具（直线/矩形/椭圆）在拖动过程中要不断重画预览，所以会话里会保存一份
// 整层备份：每次 update 先把备份恢复回去，再画新的形状。像素画板尺寸不大，
// 这个做法比维护"部分回滚"简单得多，也不会出现预览残留。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "image.h"
#include "layer.h"
#include "material.h"
#include "types.h"

namespace pb {

enum class ToolId {
    Pencil = 0,
    Line,
    Rect,
    Ellipse,
    Fill,
    Dither,
    Eraser,
    Picker,
    Lighten,
    Darken,
    Count
};

const char* toolName(ToolId t);

struct ToolOptions {
    ToolId tool = ToolId::Pencil;

    // 当前颜色。alpha 是"遮挡程度"：255 = 实心，0 = 空气，中间 = 半透明介质。
    RGBA8 color{255, 255, 255, 255};
    // 当前材质（自发光 RGB / 折射率 / 实心标志）
    Material material{};

    int brushSize = 1;        // 1..64
    bool brushRound = false;  // false = 方形笔刷（像素画常用），true = 圆形
    bool shapeFilled = false; // 矩形 / 椭圆是否填充
    int ditherLevel = 50;     // 抖动笔覆盖率 0..100
    int shadeAmount = 40;     // 加光 / 减光强度 0..255

    // 当前颜色是否参与"清除"（橡皮走的是固定透明）
    bool isErase() const { return tool == ToolId::Eraser; }
};

class ToolSession {
public:
    // 返回后 active() 为 true；modified() 表示是否真的写过图层
    void begin(Layer& L, const ToolOptions& o, int x, int y);
    void update(Layer& L, const ToolOptions& o, int x, int y);
    void end();

    bool active() const { return active_; }
    bool modified() const { return modified_; }
    ToolId tool() const { return tool_; }

    // 该工具是否会改动图层（取色器不会，因此 app 不该为它压入撤销步骤）
    static bool toolModifies(ToolId t) { return t != ToolId::Picker; }

private:
    void restoreBackup(Layer& L);
    void saveBackup(const Layer& L);

    bool active_ = false;
    bool modified_ = false;
    bool hasBackup_ = false;
    ToolId tool_ = ToolId::Pencil;
    int startX_ = 0, startY_ = 0;
    int lastX_ = 0, lastY_ = 0;

    // 形状预览回滚时要重标的脏区（见 tool.cpp 里 restoreBackup 的说明）
    Rect backupDirty_;

    Image backupColor_;
    std::vector<Material> backupMat_;
};

// --- 供 app / 测试直接使用的单次操作 ---

// 读取某个像素的颜色与材质（取色器用）。越界返回 false。
bool pickPixel(const Layer& L, int x, int y, RGBA8& colorOut, Material& matOut);

// 按笔刷形状与大小在 (cx,cy) 落一个笔印
void stampBrush(Layer& L, int cx, int cy, const ToolOptions& o, RGBA8 color,
                Material mat);

// 抖动（Bayer 4x4 有序抖动）判定：返回该像素是否应该被写上
bool ditherTest(int x, int y, int levelPercent);

// 加光 / 减光：读改写已有像素。lighten=true 向白靠，false 向黑靠。
void shadeBrush(Layer& L, int cx, int cy, const ToolOptions& o, bool lighten);

}  // namespace pb
