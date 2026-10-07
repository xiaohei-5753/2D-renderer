// core/tool.cpp
#include "tool.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace pb {

const char* toolName(ToolId t) {
    switch (t) {
        case ToolId::Pencil: return "铅笔";
        case ToolId::Line: return "直线";
        case ToolId::Rect: return "矩形";
        case ToolId::Ellipse: return "椭圆";
        case ToolId::Fill: return "油漆桶";
        case ToolId::Dither: return "抖动笔";
        case ToolId::Eraser: return "橡皮";
        case ToolId::Picker: return "取色器";
        case ToolId::Lighten: return "加光";
        case ToolId::Darken: return "减光";
        default: return "?";
    }
}

namespace {

// Bayer 4x4 有序抖动矩阵，值域 0..15。
// 必须是**位置确定**的：如果用随机抖动，像素画在每次重绘时会闪。
const int kBayer4x4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

inline uint8_t lerpToByte(int from, int to, int amount) {
    if (amount <= 0) return (uint8_t)from;
    if (amount >= 255) return (uint8_t)to;
    const int v = from + (to - from) * amount / 255;
    return (uint8_t)std::clamp(v, 0, 255);
}

// 圆形笔刷的半径平方。半整数半径让 size=2 也能得到合理的两点宽度。
inline float brushRadiusSq(int half) {
    const float r = (float)half + 0.5f;
    return r * r;
}

}  // namespace

bool ditherTest(int x, int y, int levelPercent) {
    if (levelPercent <= 0) return false;
    if (levelPercent >= 100) return true;
    const int t = kBayer4x4[((y & 3) << 2) | (x & 3)];
    // 阈值 0..16 与百分比比较，等价于"按比例保留像素"
    return t * 100 < levelPercent * 16;
}

bool pickPixel(const Layer& L, int x, int y, RGBA8& colorOut, Material& matOut) {
    if (L.empty()) return false;
    if (x < 0 || y < 0 || x >= L.width() || y >= L.height()) return false;
    colorOut = L.image().at(x, y);
    matOut = L.materialAt(x, y);
    return true;
}

// ---------------------------------------------------------------------------
// 笔刷落点
// ---------------------------------------------------------------------------

void stampBrush(Layer& L, int cx, int cy, const ToolOptions& o, RGBA8 color,
                Material mat) {
    if (L.empty()) return;

    if (o.brushSize <= 1) {
        const bool dither = (o.tool == ToolId::Dither);
        if (!dither || ditherTest(cx, cy, o.ditherLevel)) L.setPixel(cx, cy, color, mat);
        return;
    }

    const int half = (std::max(1, o.brushSize) - 1) / 2;
    const int W = L.width(), H = L.height();
    const int x0 = std::max(0, cx - half);
    const int x1 = std::min(W - 1, cx + half);
    const int y0 = std::max(0, cy - half);
    const int y1 = std::min(H - 1, cy + half);
    if (x1 < x0 || y1 < y0) return;

    const float r2 = brushRadiusSq(half);
    const bool dither = (o.tool == ToolId::Dither);

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (o.brushRound) {
                const int dx = x - cx, dy = y - cy;
                if ((float)(dx * dx + dy * dy) > r2) continue;
            }
            if (dither && !ditherTest(x, y, o.ditherLevel)) continue;
            L.setPixelRaw(x, y, color, mat);
        }
    }
    L.markDirty(Rect{x0, y0, x1, y1});
}

void shadeBrush(Layer& L, int cx, int cy, const ToolOptions& o, bool lighten) {
    if (L.empty()) return;

    const int half = (std::max(1, o.brushSize) - 1) / 2;
    const int W = L.width(), H = L.height();

    const int x0 = std::max(0, cx - half);
    const int x1 = std::min(W - 1, cx + half);
    const int y0 = std::max(0, cy - half);
    const int y1 = std::min(H - 1, cy + half);
    if (x1 < x0 || y1 < y0) return;

    const float r2 = brushRadiusSq(half);
    const int amount = std::clamp(o.shadeAmount, 0, 255);

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (o.brushRound) {
                const int dx = x - cx, dy = y - cy;
                if ((float)(dx * dx + dy * dy) > r2) continue;
            }
            RGBA8& p = L.image().at(x, y);
            if (p.a == 0) continue;  // 空气不加不减，避免凭空出现像素
            if (lighten) {
                p.r = lerpToByte(p.r, 255, amount);
                p.g = lerpToByte(p.g, 255, amount);
                p.b = lerpToByte(p.b, 255, amount);
            } else {
                p.r = lerpToByte(p.r, 0, amount);
                p.g = lerpToByte(p.g, 0, amount);
                p.b = lerpToByte(p.b, 0, amount);
            }
        }
    }
    L.markDirty(Rect{x0, y0, x1, y1});
}

// ---------------------------------------------------------------------------
// ToolSession
// ---------------------------------------------------------------------------

void ToolSession::saveBackup(const Layer& L) {
    if (L.empty()) {
        hasBackup_ = false;
        return;
    }
    backupColor_ = L.image();
    backupMat_.assign(L.materialData(), L.materialData() + L.pixelCount());
    hasBackup_ = true;
}

void ToolSession::restoreBackup(Layer& L) {
    if (!hasBackup_ || L.empty()) return;
    if (backupColor_.width() != L.width() || backupColor_.height() != L.height()) return;
    if (backupMat_.size() != L.pixelCount()) return;
    L.image() = backupColor_;
    std::copy(backupMat_.begin(), backupMat_.end(), L.materialData());
    L.revision++;
    // 只把**开始时就已经脏的区域**重标一次，而不是整层。
    //
    // 理由：预览回滚只会把本会话画过的东西抹掉，而这些像素一定落在 begin 时那份脏区
    // 里。如果这里改成 markAllDirty()，每拖动一格都会让**整张画布**（所有图层）重新
    // 合成并整块重传 GPU —— 交互会立刻卡死。这是个很容易踩的坑。
    L.markDirty(backupDirty_);
}

void ToolSession::begin(Layer& L, const ToolOptions& o, int x, int y) {
    end();
    if (L.empty()) return;

    tool_ = o.tool;
    active_ = true;
    modified_ = false;
    startX_ = lastX_ = x;
    startY_ = lastY_ = y;
    backupDirty_ = L.dirty();

    // 形状工具需要整层备份来做预览回滚
    const bool shapeTool = (tool_ == ToolId::Line || tool_ == ToolId::Rect ||
                            tool_ == ToolId::Ellipse);
    if (shapeTool) {
        saveBackup(L);
    } else {
        hasBackup_ = false;
    }

    switch (tool_) {
        case ToolId::Fill:
            L.floodFill(x, y, o.color, o.material);
            modified_ = true;
            break;

        case ToolId::Picker:
            // 取色不修改画布；由 app 调 pickPixel() 把结果写回工具选项。
            // 这里刻意什么都不做，于是 modified() 保持 false，app 不会压入撤销步骤。
            modified_ = false;
            break;

        case ToolId::Pencil:
        case ToolId::Eraser:
        case ToolId::Dither:
        case ToolId::Lighten:
        case ToolId::Darken:
        case ToolId::Line:
        case ToolId::Rect:
        case ToolId::Ellipse:
            // 起点也走一次 update：单击即可留下一个点/一条极短线段，
            // 同时形状工具的预览逻辑也不必在 begin 里重复一遍。
            update(L, o, x, y);
            break;

        default:
            break;
    }
}

void ToolSession::update(Layer& L, const ToolOptions& o, int x, int y) {
    if (!active_ || L.empty()) return;

    // 沿上一次位置到当前位置逐步落笔，避免快速拖动出现断点
    const int steps = std::max(std::abs(x - lastX_), std::abs(y - lastY_));

    switch (tool_) {
        case ToolId::Pencil:
        case ToolId::Eraser:
        case ToolId::Dither: {
            const bool erasing = (tool_ == ToolId::Eraser);
            const RGBA8 col = erasing ? RGBA8{0, 0, 0, 0} : o.color;
            const Material mat = erasing ? Material{} : o.material;
            for (int i = 0; i <= steps; i++) {
                const int t = (steps == 0) ? 0 : i;
                const int px = (steps == 0) ? x : lastX_ + (x - lastX_) * t / steps;
                const int py = (steps == 0) ? y : lastY_ + (y - lastY_) * t / steps;
                stampBrush(L, px, py, o, col, mat);
            }
            modified_ = true;
            break;
        }

        case ToolId::Lighten:
        case ToolId::Darken: {
            for (int i = 0; i <= steps; i++) {
                const int t = (steps == 0) ? 0 : i;
                const int px = (steps == 0) ? x : lastX_ + (x - lastX_) * t / steps;
                const int py = (steps == 0) ? y : lastY_ + (y - lastY_) * t / steps;
                shadeBrush(L, px, py, o, tool_ == ToolId::Lighten);
            }
            modified_ = true;
            break;
        }

        case ToolId::Line:
            restoreBackup(L);
            L.drawLine(startX_, startY_, x, y, o.color, o.material);
            modified_ = true;
            break;

        case ToolId::Rect:
            restoreBackup(L);
            if (o.shapeFilled) L.fillRect(startX_, startY_, x, y, o.color, o.material);
            else L.rectOutline(startX_, startY_, x, y, o.color, o.material);
            modified_ = true;
            break;

        case ToolId::Ellipse:
            restoreBackup(L);
            L.drawEllipse(startX_, startY_, x, y, o.shapeFilled, o.color, o.material);
            modified_ = true;
            break;

        default:
            // Fill / Picker 在 begin 里一次性完成，拖动不再产生效果
            return;
    }

    lastX_ = x;
    lastY_ = y;
}

void ToolSession::end() {
    active_ = false;
    hasBackup_ = false;
    backupColor_ = Image{};
    backupMat_.clear();
}

}  // namespace pb
