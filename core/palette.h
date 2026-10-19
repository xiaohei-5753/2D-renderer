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

    // ------------------------------------------------------------------
    // 色板项 = **材质**（7 维）：颜色 RGB + 自发光 RGB + 透明度
    //
    // 为什么把材质放进色板：以前色板只管颜色，材质是另一套逐像素的东西，
    // 两边没关系 —— 从色板点一个颜色，实际这一笔带什么材质用户无从得知，
    // 前后画的两笔材质还可能不同。放进色板之后"选色 = 同时选定材质"。
    //
    // 存储上颜色仍放在 colors_（现有代码全都不用改），材质放在平行的 mats_。
    // mats_ 长度不足时取默认值，所以不需要和各处改动严格同步。
    struct Mat7 {
        uint8_t r = 255, g = 255, b = 255;   // 颜色
        uint8_t eR = 0, eG = 0, eB = 0;      // 自发光（"亮度"，带颜色）
        uint8_t a = 255;                     // 透明度
    };
    // 返回值（不是引用）：因为 r/g/b 是**派生**的，始终以颜色表为准。
    //
    // 为什么不直接返回 mats_[i]：mats_ 是平行数组，可能被 addColor / setMat /
    // setAll 各自扩展，里面的 r/g/b 副本很容易与颜色表脱节（loadPreset 不重置
    // 材质时就会留下默认的白色 255）。实测踩到过：色板第 0 项是黑色，
    // mat().r 却是 255 —— 工程文件存下来再读就"不一致"了。
    //
    // 与其在四处小心同步，不如让副本不参与结果：**单一真相来源**。
    // eR/eG/eB/a 才是 mats_ 真正独有的信息。
    Mat7 mat(int i) const {
        Mat7 out{};
        if (i < 0 || (size_t)i >= mats_.size()) return out;
        out = mats_[(size_t)i];
        if ((size_t)i < colors_.size()) {
            out.r = colors_[(size_t)i].r;
            out.g = colors_[(size_t)i].g;
            out.b = colors_[(size_t)i].b;
        }
        return out;
    }
    void setMat(int i, const Mat7& m) {
        if (i < 0) return;
        if (mats_.size() < colors_.size()) mats_.resize(colors_.size());
        if ((size_t)i >= mats_.size()) mats_.resize((size_t)i + 1);
        mats_[(size_t)i] = m;
    }
    // 由当前颜色与材质，取这一笔真正要用的颜色 / 自发光
    RGBA8 matColor(int i) const {
        const Mat7& m = mat(i);
        return RGBA8{m.r, m.g, m.b, m.a};
    }

    // 换预设后调用：材质回到"本项颜色 + 不发光 + 全不透明"的干净默认值。
    // 不做的话 mats_ 会残留上一个色板的材质，挂到新颜色上。
    void resetMats() {
        mats_.assign(colors_.size(), Mat7{});
        for (size_t i = 0; i < mats_.size() && i < colors_.size(); i++) {
            mats_[i].r = colors_[i].r;
            mats_[i].g = colors_[i].g;
            mats_[i].b = colors_[i].b;
            mats_[i].a = 255;
        }
    }

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
    // 删除一个颜色（至少保留 1 个）。真的删掉了才返回 true。
    bool removeColor(int i);

    // 整表替换：读取工程文件时用。
    //
    // 为什么不复用 removeColor 逐个清：它有"至少保留 1 个"的保护，逐个删会在
    // 只剩 1 个时**死循环**（踩过）。而且"从磁盘恢复整份色板"本来就是另一个
    // 语义 —— 它不归"至少留一个"这条规则管（空色板是合法的加载结果）。
    // col 为空时结果就是空色板。
    void setAll(const std::vector<RGBA8>& col, const std::vector<Mat7>& mats, int current);

    // 当前色板里是否已经存在这个颜色
    bool contains(RGBA8 c) const;

private:
    std::vector<RGBA8> colors_;
    std::vector<Mat7> mats_;      // 与 colors_ 平行的材质表（可能短于它）
    int current_ = 0;
    int preset_ = 0;
};

}  // namespace pb
