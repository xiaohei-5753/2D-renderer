// core/history.cpp
#include "history.h"

#include <algorithm>

namespace pb {

const std::string History::kEmptyLabel;

// ---------------------------------------------------------------------------
// 快照捕获与写回
// ---------------------------------------------------------------------------

History::Blob History::captureRect(const Layer& L, int layerIndex, const Rect& rIn) {
    Blob b;
    b.layerIndex = layerIndex;
    if (L.empty()) return b;   // rect 保持为空 = 这个 blob 没有内容

    Rect r = rIn;
    r.clipTo(L.width(), L.height());
    if (r.empty()) return b;

    b.rect = r;
    const int w = r.width(), h = r.height();
    b.color.resize((size_t)w * (size_t)h);
    b.mat.resize((size_t)w * (size_t)h);

    for (int y = 0; y < h; y++) {
        const size_t srcRow = (size_t)(r.y0 + y) * (size_t)L.width() + (size_t)r.x0;
        const size_t dstRow = (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            b.color[dstRow + (size_t)x] = L.albedo.data()[srcRow + (size_t)x];
            b.mat[dstRow + (size_t)x] = L.material[srcRow + (size_t)x];
        }
    }
    return b;
}

History::Blob History::captureWholeLayer(const Document& d, int layerIndex) {
    if (layerIndex < 0 || layerIndex >= d.layerCount()) return Blob{};
    return captureRect(d.layer(layerIndex), layerIndex,
                       Rect{0, 0, d.width() - 1, d.height() - 1});
}

bool History::blobEqual(const Blob& a, const Blob& b) {
    if (a.rect.empty() != b.rect.empty()) return false;
    if (a.rect.empty()) return true;
    if (a.rect.x0 != b.rect.x0 || a.rect.y0 != b.rect.y0 || a.rect.x1 != b.rect.x1 ||
        a.rect.y1 != b.rect.y1) {
        return false;
    }
    return a.color == b.color && a.mat == b.mat;
}

void History::applyBlobs(Document& d, const std::vector<Blob>& blobs) {
    for (const Blob& b : blobs) {
        if (b.rect.empty()) continue;
        if (b.layerIndex < 0 || b.layerIndex >= d.layerCount()) continue;
        Layer& L = d.layer(b.layerIndex);
        if (b.rect.x1 >= L.width() || b.rect.y1 >= L.height()) continue;
        if (b.color.size() != (size_t)b.rect.width() * (size_t)b.rect.height()) continue;

        for (int y = 0; y < b.rect.height(); y++) {
            const size_t srcRow = (size_t)y * (size_t)b.rect.width();
            const size_t dstRow =
                (size_t)(b.rect.y0 + y) * (size_t)L.width() + (size_t)b.rect.x0;
            for (int x = 0; x < b.rect.width(); x++) {
                L.albedo.data()[dstRow + (size_t)x] = b.color[srcRow + (size_t)x];
                L.material[dstRow + (size_t)x] = b.mat[srcRow + (size_t)x];
            }
        }
        L.revision++;
        L.markDirty(b.rect);
    }
}

// 把文档的图层结构调成 n 层（不够就补空层，多了就砍掉）。
// 撤销结构性操作时用：那个时刻的层数是确定的，先恢复结构再写像素。
void History::restoreLayerCount(Document& d, int n) {
    n = std::max(1, n);
    while (d.layerCount() < n) d.addLayer();
    if (d.layerCount() > n) {
        d.setActiveIndex(0);   // 避免删除过程中 active 落在被删掉的位置上
        while (d.layerCount() > n) d.removeLayer(d.layerCount() - 1);
    }
    // 每层都可能刚被补出来 / 被砍过，内容一律当作脏的（自底向上重算合成）
    for (int i = 0; i < d.layerCount(); i++) d.layer(i).markAllDirty();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void History::reset() {
    undo_.clear();
    redo_.clear();
    snapshot_ = Blob{};
    snapshotAll_.clear();
    metaBefore_.clear();
    inStroke_ = false;
    wholeLayer_ = false;
    strokeLabel_.clear();
}

void History::beginStroke(const Document& doc) {
    if (doc.empty()) {
        inStroke_ = false;
        return;
    }
    strokeLayer_ = doc.activeIndex();
    activeBefore_ = doc.activeIndex();
    snapshot_ = captureWholeLayer(doc, strokeLayer_);
    snapshotAll_.clear();
    metaBefore_.clear();
    inStroke_ = true;
    wholeLayer_ = false;
    strokeLabel_.clear();
}

void History::beginAll(const Document& doc) {
    snapshotAll_.clear();
    metaBefore_.clear();
    strokeLayer_ = doc.activeIndex();
    activeBefore_ = doc.activeIndex();
    for (int i = 0; i < doc.layerCount(); i++) {
        snapshotAll_.push_back(captureWholeLayer(doc, i));
        metaBefore_.push_back(metaOf(doc.layer(i)));
    }
    snapshot_ = Blob{};
    inStroke_ = true;
    wholeLayer_ = true;
    strokeLabel_.clear();
}

void History::abortStroke() {
    inStroke_ = false;
    wholeLayer_ = false;
    snapshot_ = Blob{};
    snapshotAll_.clear();
    metaBefore_.clear();
}

bool History::commit(const Document& doc, const std::string& label) {
    if (!inStroke_) return false;
    inStroke_ = false;

    Step s;
    s.label = label;
    s.layerCount = doc.layerCount();
    s.activeBefore = activeBefore_;
    s.activeAfter = doc.activeIndex();
    s.isWholeDocument = wholeLayer_;
    bool changed = false;

    if (wholeLayer_) {
        // 结构性操作：层数变了必然算改动（否则不会有人调 beginAll）
        s.layerCountBefore = (int)snapshotAll_.size();
        s.metaBefore = metaBefore_;
        if ((int)snapshotAll_.size() != doc.layerCount()) {
            changed = true;
        } else {
            for (int i = 0; i < doc.layerCount(); i++) {
                Blob now = captureWholeLayer(doc, i);
                if (!blobEqual(snapshotAll_[(size_t)i], now)) {
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            s.before = snapshotAll_;
            // "改后"要存**整份文档**（不只是 beginAll 时就存在的那些层）。
            //
            // 这里踩过一个很隐蔽的坑：一开始只存了"当时已存在的层"，理由是"新加的层
            // 是空的，撤销后重做时它自动是空的"。但撤销与重做必须是**对称**的完整快照 ——
            // 因为重做并不是"再做一遍那个操作"，而是"把文档恢复成改后的样子"。
            // 一旦中间还发生过别的操作，"空层"就不再成立了。测试抓到的正是这个：
            // 新建图层 -> 画 -> 撤销 -> 重做，那笔画必须回来。
            for (int i = 0; i < doc.layerCount(); i++) {
                s.after.push_back(captureWholeLayer(doc, i));
                s.metaAfter.push_back(metaOf(doc.layer(i)));
            }
        }
    } else {
        s.layerCountBefore = doc.layerCount();
        if (strokeLayer_ >= 0 && strokeLayer_ < doc.layerCount()) {
            Blob now = captureWholeLayer(doc, strokeLayer_);
            if (!blobEqual(snapshot_, now)) {
                changed = true;
                s.before.push_back(snapshot_);
                s.after.push_back(now);
                s.metaBefore.push_back(metaOf(doc.layer(strokeLayer_)));
                s.metaAfter.push_back(metaOf(doc.layer(strokeLayer_)));
            }
        }
    }

    snapshot_ = Blob{};
    snapshotAll_.clear();
    metaBefore_.clear();
    wholeLayer_ = false;

    if (!changed) return false;

    redo_.clear();   // 新的操作使重做链失效
    undo_.push_back(std::move(s));
    trim();
    return true;
}

// ---------------------------------------------------------------------------
// 撤销 / 重做
// ---------------------------------------------------------------------------

// 收集一个步骤要恢复的层。
//
// 两条必须遵守的语义（都踩过坑，第二个特别隐蔽）：
//
// 1. **只恢复快照里有的层。** 一笔画只给当前图层存快照，其它层必须原样不动 ——
//    否则撤销一笔会把别的层的改动一起回退掉。
//
// 2. **只有"整份文档快照"才需要重指 layerIndex。** 结构性操作给每一层都存了快照，
//    而撤销"删除图层"之后图层顺序会整体上移，快照里记的旧索引已经指错层了，
//    所以必须按"它就是第 i 层"来写回。
//    但一笔画的快照里**只有一条**（当前图层），这条的 layerIndex 本来就是对的；
//    一旦也按位置重指成 0，撤销就会把状态写到最底层去 —— 表现为"撤销没反应"。
void History::collectForApply(const std::vector<Blob>& in, bool wholeDocument,
                              std::vector<Blob>& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        Blob b = in[i];
        if (wholeDocument) b.layerIndex = (int)i;
        out.push_back(std::move(b));
    }
}

bool History::undo(Document& doc) {
    if (undo_.empty() || doc.empty()) return false;

    Step s = std::move(undo_.back());
    undo_.pop_back();

    restoreLayerCount(doc, s.layerCountBefore);
    for (int i = 0; i < (int)s.metaBefore.size() && i < doc.layerCount(); i++)
        applyMeta(doc.layer(i), s.metaBefore[(size_t)i]);

    collectForApply(s.before, s.isWholeDocument, scratch_);
    applyBlobs(doc, scratch_);
    doc.setActiveIndex(s.activeBefore);

    redo_.push_back(std::move(s));
    return true;
}

bool History::redo(Document& doc) {
    if (redo_.empty() || doc.empty()) return false;

    Step s = std::move(redo_.back());
    redo_.pop_back();

    restoreLayerCount(doc, s.layerCount);
    for (int i = 0; i < (int)s.metaAfter.size() && i < doc.layerCount(); i++)
        applyMeta(doc.layer(i), s.metaAfter[(size_t)i]);

    collectForApply(s.after, s.isWholeDocument, scratch_);
    applyBlobs(doc, scratch_);
    doc.setActiveIndex(s.activeAfter);

    undo_.push_back(std::move(s));
    return true;
}

// ---------------------------------------------------------------------------
// 内存管理
// ---------------------------------------------------------------------------

size_t History::bytes() const {
    size_t n = 0;
    for (const Step& s : undo_) n += s.bytes();
    for (const Step& s : redo_) n += s.bytes();
    n += snapshot_.bytes();
    for (const Blob& b : snapshotAll_) n += b.bytes();
    return n;
}

void History::trim() {
    while ((int)undo_.size() > maxSteps) undo_.pop_front();
    while (bytes() > maxBytes && undo_.size() > 1) undo_.pop_front();
}

const std::string& History::undoLabel() const {
    return undo_.empty() ? kEmptyLabel : undo_.back().label;
}

const std::string& History::redoLabel() const {
    return redo_.empty() ? kEmptyLabel : redo_.back().label;
}

}  // namespace pb
