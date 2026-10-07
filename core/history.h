// core/history.h — 撤销 / 重做
//
// 像素画板上"整图快照"式撤销很快就把内存吃光（一张 512×512 的图每步要 1 MB 颜色
// + 1 MB 材质），所以这里存的是**变化矩形内的补丁**。
//
// 有了图层之后，一个撤销步骤可能同时涉及多层，所以每个步骤是一个**补丁列表**：
//
//   Step { label, before[], after[], layerCount, layerCountBefore, metaBefore[] }
//   Blob { layerIndex, rect, 像素[], 材质[] }
//
// 两种提交方式，覆盖了所有编辑行为：
//
//   beginStroke/commit  —— 一笔画。只跟踪**当前图层**，是常见路径。
//   beginAll/commit     —— 结构性操作（新建 / 删除 / 合并 / 重排 / 整层清空）。
//                          这类操作会改变"哪些层参与合成"，逐层取差既麻烦又容易漏，
//                          而整份文档本来就不大，直接整体快照最稳妥。
//
// 每个步骤同时存"改前"和"改后"两份，于是撤销与重做的行为完全对称 ——
// 不必像老实现那样在撤销的那一刻从画布现采状态（一旦有绕过 History 的改动，
// 那条路就会把重做链采坏）。
//
// 一个必须说清楚的**已知限制**：结构性操作撤销后，"改后"状态里不包含当时新建的层
// 的内容（新建时它们本来就是空的）。所以「新建一个图层 → 在上面画 → 撤销两次 →
// 重做两次」会重做出一张**空的新图层**，那笔画不会回来。要修得存"新建层的内容"，
// 但那需要一个真正的图层 ID（现在图层是按索引寻址的，插入/删除会让索引整体错位，
// 存内容反而会写错层）。这是有意的取舍，不是遗漏。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "layer.h"
#include "material.h"
#include "types.h"

namespace pb {

class History {
public:
    // 换文档 / 新建文档时调用
    void reset();

    // --- 笔画：只跟踪当前图层 ---
    void beginStroke(const Document& doc);
    // 结构性操作：整份文档逐层快照（含图层名/可见性/不透明度等元数据）
    void beginAll(const Document& doc);
    void abortStroke();

    // 提交：没有实际变化时返回 false（不会污染撤销栈）
    bool commit(const Document& doc, const std::string& label = std::string());

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo(Document& d);
    bool redo(Document& d);

    // 任何**绕过 beginStroke/commit** 的改动（载入文件、外部模块直接写像素等）
    // 之后都必须调用它 —— 重做栈里存的状态已经和当前文档对不上了。
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
    // 某一层某个矩形区域的像素 + 材质
    struct Blob {
        int layerIndex = 0;
        Rect rect;
        std::vector<RGBA8> color;
        std::vector<Material> mat;
        size_t bytes() const {
            return color.size() * sizeof(RGBA8) + mat.size() * sizeof(Material);
        }
    };

    // 图层的非像素属性。撤销"删除图层"时，光把层数补回来是不够的 ——
    // 名字、可见性、不透明度、锁定状态都得跟着回来。
    struct LayerMeta {
        std::string name;
        bool visible = true;
        uint8_t opacity = 255;
        BlendMode blend = BlendMode::Normal;
        bool locked = false;
    };

    // 一个步骤：改前状态 + 改后状态
    struct Step {
        std::string label;
        std::vector<Blob> before;
        std::vector<Blob> after;
        std::vector<LayerMeta> metaBefore;
        // 结构性操作也会改图层的**非像素属性**（删除时名字/不透明度会跟着消失、
        // 合并时底层的名字会留下）。既然重做要能精确回到"当时的样子"，
        // 这里就必须也存一份改后元数据 —— 否则撤销再重做会得到一个名字不对的图层。
        std::vector<LayerMeta> metaAfter;
        // "当时选中的是哪一层"。撤销/重做把它也一起还原：加删图层会顺手改动
        // 当前层，如果撤销后选中层跑到别处，接着画就会画到错的层上。
        int activeBefore = 0;
        int activeAfter = 0;
        int layerCountBefore = 1;
        int layerCount = 1;
        // true = beginAll（整份文档逐层快照）。恢复时要按位置重指 layerIndex，
        // 因为图层的顺序/数量可能已经变了。详见 history.cpp 的 collectForApply。
        bool isWholeDocument = false;
        size_t bytes() const {
            size_t n = 0;
            for (const Blob& b : before) n += b.bytes();
            for (const Blob& b : after) n += b.bytes();
            return n;
        }
    };

    static Blob captureRect(const Layer& L, int layerIndex, const Rect& r);
    static Blob captureWholeLayer(const Document& d, int layerIndex);
    static bool blobEqual(const Blob& a, const Blob& b);
    static void applyBlobs(Document& d, const std::vector<Blob>& blobs);
    static void collectForApply(const std::vector<Blob>& in, bool wholeDocument,
                                std::vector<Blob>& out);
    static void restoreLayerCount(Document& d, int n);

    static LayerMeta metaOf(const Layer& L) {
        LayerMeta m;
        m.name = L.name;
        m.visible = L.visible;
        m.opacity = L.opacity;
        m.blend = L.blend;
        m.locked = L.locked;
        return m;
    }
    static void applyMeta(Layer& L, const LayerMeta& m) {
        L.name = m.name;
        L.visible = m.visible;
        L.opacity = m.opacity;
        L.blend = m.blend;
        L.locked = m.locked;
    }

    void trim();

    std::deque<Step> undo_;
    std::deque<Step> redo_;

    // 进行中的操作
    bool inStroke_ = false;
    bool wholeLayer_ = false;         // true = beginAll，提交时整份文档取差
    int strokeLayer_ = 0;
    int activeBefore_ = 0;            // 操作开始时选中的图层
    Blob snapshot_;                   // 单层路径：操作开始时该层的整层快照
    std::vector<Blob> snapshotAll_;   // 整文档路径：每层的整层快照
    std::vector<LayerMeta> metaBefore_;
    std::vector<Blob> scratch_;       // undo/redo 时的临时落位表（避免每次分配）
    std::string strokeLabel_;

    static const std::string kEmptyLabel;
};

}  // namespace pb
