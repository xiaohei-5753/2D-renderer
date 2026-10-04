// core/history.h — 撤销 / 重做
//
// 像素画板上"整图快照"式撤销很快就把内存吃光（一张 512×512 的图每步要 1 MB 颜色
// + 1 MB 材质），所以这里存的是**变化矩形内的补丁**：笔画结束时把当前状态与笔画
// 开始时的快照做差，只把真正变了的那个外接矩形入栈，通常只有几十个像素。
//
// 每个栈项只存单向状态（undo 栈存"改前"，redo 栈存"改后"），另一半在切换时
// 从画布现采。这样每步只占一份而非两份，内存直接减半。
#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "canvas.h"
#include "material.h"
#include "types.h"

namespace pb {

class History {
public:
    // 换画布 / 新建文档时调用
    void reset();

    // 笔画开始：整图快照（仅存于笔画期间，笔画结束后释放）
    void beginStroke(const Canvas& c);
    void abortStroke();

    // 笔画结束：与开始快照做差，把变化矩形入栈。
    // 没有任何像素变化时返回 false（不会污染撤销栈）。
    bool endStroke(const Canvas& c, const std::string& label = std::string());

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo(Canvas& c);
    bool redo(Canvas& c);

    // 任何**绕过 beginStroke/endStroke** 的画布改动（载入文件、外部模块直接写像素等）
    // 之后都必须调用它。
    //
    // 原因：撤销栈里存的是"改前"状态，而重做栈所需的"改后"状态是在**撤销的那一刻**
    // 从画布现采的（这样每步只占一份内存，见文件头的说明）。一旦画布被越带修改，
    // 采到的"改后"就不再是当初提交时的样子，重做会还原出错误内容——所以只能把重做链作废。
    void notifyExternalChange() { redo_.clear(); }

    // 当前可撤销的步骤描述（栈顶），用于界面显示
    const std::string& undoLabel() const;
    const std::string& redoLabel() const;

    size_t undoSteps() const { return undo_.size(); }
    size_t redoSteps() const { return redo_.size(); }
    size_t bytes() const;

    // 上限：步数与总字节数，超出后从最旧的一端丢弃
    int maxSteps = 128;
    size_t maxBytes = 192u * 1024u * 1024u;

private:
    // 一个矩形区域的像素 + 材质快照
    struct Blob {
        Rect rect;
        std::vector<RGBA8> color;
        std::vector<Material> mat;
        bool empty() const { return rect.empty(); }
        size_t bytes() const {
            return color.size() * sizeof(RGBA8) + mat.size() * sizeof(Material);
        }
    };

    struct Entry {
        Blob state;
        std::string label;
    };

    static Blob captureRect(const Canvas& c, const Rect& r);
    static void applyRect(Canvas& c, const Blob& b);
    void trim();

    std::deque<Entry> undo_;
    std::deque<Entry> redo_;

    // 笔画开始时的整图快照
    Blob snapshot_;
    bool inStroke_ = false;

    static const std::string kEmptyLabel;
};

}  // namespace pb
