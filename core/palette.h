// core/palette.h — 调色板
//
// 双轨：内置的标准像素画色板（PICO-8 / DawnBringer 16 / Game Boy 等）便于快速画出
// 风格统一的图；同时允许随时加入自定义颜色（自由 RGB），自定义色会追加到当前色板末尾。
#pragma once

#include <vector>

#include "types.h"

namespace pb {

struct PalettePreset {
    const char* name;
    const RGBA8* colors;
    int count;
};

// 内置色板数量与访问
int builtinPaletteCount();
const PalettePreset& builtinPalette(int index);

class Palette {
public:
    Palette() { loadPreset(0); }

    // 载入内置色板；index 会被夹到合法范围
    void loadPreset(int index);

    int presetIndex() const { return preset_; }
    const char* presetName() const;

    int size() const { return (int)colors_.size(); }
    bool empty() const { return colors_.empty(); }
    RGBA8 color(int i) const;

    int currentIndex() const { return current_; }
    void setCurrent(int i);
    RGBA8 current() const { return color(current_); }

    // 在当前色板内循环切换（delta 可以是负数）
    void step(int delta);

    // 追加一个自定义颜色并选中它，返回其下标
    int addColor(RGBA8 c);
    void setColorAt(int i, RGBA8 c);

    // 当前色板里是否已经存在这个颜色
    bool contains(RGBA8 c) const;

private:
    std::vector<RGBA8> colors_;
    int current_ = 0;
    int preset_ = 0;
};

}  // namespace pb
