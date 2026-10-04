// core/history.cpp
#include "history.h"

#include <algorithm>

namespace pb {

const std::string History::kEmptyLabel;

void History::reset() {
    undo_.clear();
    redo_.clear();
    snapshot_ = Blob{};
    inStroke_ = false;
}

// ---------------------------------------------------------------------------
// 快照的采集与应用
// ---------------------------------------------------------------------------

History::Blob History::captureRect(const Canvas& c, const Rect& r) {
    Blob b;
    if (c.empty() || r.empty()) return b;

    Rect rc = r;
    rc.clipTo(c.width(), c.height());
    if (rc.empty()) return b;

    b.rect = rc;
    const int w = rc.width(), h = rc.height();
    b.color.resize((size_t)w * (size_t)h);
    b.mat.resize((size_t)w * (size_t)h);

    const Image& img = c.image();
    const Material* mats = c.materialData();
    const int cw = c.width();

    for (int y = 0; y < h; y++) {
        const RGBA8* srcC = img.data() + (size_t)(rc.y0 + y) * (size_t)cw + (size_t)rc.x0;
        const Material* srcM = mats + (size_t)(rc.y0 + y) * (size_t)cw + (size_t)rc.x0;
        RGBA8* dstC = b.color.data() + (size_t)y * (size_t)w;
        Material* dstM = b.mat.data() + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            dstC[x] = srcC[x];
            dstM[x] = srcM[x];
        }
    }
    return b;
}

void History::applyRect(Canvas& c, const Blob& b) {
    if (c.empty() || b.empty()) return;

    const int w = b.rect.width(), h = b.rect.height();
    if (b.color.size() != (size_t)w * (size_t)h) return;

    Image& img = c.image();
    Material* mats = c.materialData();
    const int cw = c.width();
    const int ch = c.height();

    for (int y = 0; y < h; y++) {
        const int gy = b.rect.y0 + y;
        if (gy < 0 || gy >= ch) continue;
        RGBA8* dstC = img.data() + (size_t)gy * (size_t)cw + (size_t)b.rect.x0;
        Material* dstM = mats + (size_t)gy * (size_t)cw + (size_t)b.rect.x0;
        const RGBA8* srcC = b.color.data() + (size_t)y * (size_t)w;
        const Material* srcM = b.mat.data() + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            const int gx = b.rect.x0 + x;
            if (gx < 0 || gx >= cw) continue;
            dstC[x] = srcC[x];
            dstM[x] = srcM[x];
        }
    }
    c.markDirty(b.rect);
}

// ---------------------------------------------------------------------------
// 笔画
// ---------------------------------------------------------------------------

void History::beginStroke(const Canvas& c) {
    if (c.empty()) {
        snapshot_ = Blob{};
        inStroke_ = false;
        return;
    }
    snapshot_ = captureRect(c, Rect{0, 0, c.width() - 1, c.height() - 1});
    inStroke_ = true;
}

void History::abortStroke() {
    snapshot_ = Blob{};
    inStroke_ = false;
}

bool History::endStroke(const Canvas& c, const std::string& label) {
    if (!inStroke_ || c.empty()) {
        abortStroke();
        return false;
    }

    const int w = c.width(), h = c.height();
    // 用 size_t 比较：大画布下 w*h 会溢出 int
    if (snapshot_.color.size() != (size_t)w * (size_t)h) {
        abortStroke();
        return false;
    }

    // 与快照做差，求出变化像素的外接矩形
    const Image& img = c.image();
    const Material* mats = c.materialData();
    const RGBA8* snapC = snapshot_.color.data();
    const Material* snapM = snapshot_.mat.data();

    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) {
        const size_t row = (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            const size_t i = row + (size_t)x;
            if (img.data()[i] == snapC[i] && mats[i] == snapM[i]) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 < x0 || y1 < y0) {
        abortStroke();
        return false;  // 无变化
    }

    // 从快照里取出"改前"状态
    Blob before;
    before.rect = Rect{x0, y0, x1, y1};
    {
        const int bw = before.rect.width(), bh = before.rect.height();
        before.color.resize((size_t)bw * (size_t)bh);
        before.mat.resize((size_t)bw * (size_t)bh);
        for (int y = 0; y < bh; y++) {
            const size_t srcRow = (size_t)(y0 + y) * (size_t)w + (size_t)x0;
            const size_t dstRow = (size_t)y * (size_t)bw;
            for (int x = 0; x < bw; x++) {
                before.color[dstRow + (size_t)x] = snapC[srcRow + (size_t)x];
                before.mat[dstRow + (size_t)x] = snapM[srcRow + (size_t)x];
            }
        }
    }

    abortStroke();  // 快照用完即可释放

    undo_.push_back(Entry{std::move(before), label});
    redo_.clear();  // 新的操作使重做链失效
    trim();
    return true;
}

// ---------------------------------------------------------------------------
// 撤销 / 重做
// ---------------------------------------------------------------------------

bool History::undo(Canvas& c) {
    if (undo_.empty() || c.empty()) return false;

    Entry e = std::move(undo_.back());
    undo_.pop_back();

    // 先把"改后"状态存进 redo 栈，再恢复"改前"
    Blob after = captureRect(c, e.state.rect);
    redo_.push_back(Entry{std::move(after), e.label});

    applyRect(c, e.state);
    return true;
}

bool History::redo(Canvas& c) {
    if (redo_.empty() || c.empty()) return false;

    Entry e = std::move(redo_.back());
    redo_.pop_back();

    Blob before = captureRect(c, e.state.rect);
    undo_.push_back(Entry{std::move(before), e.label});

    applyRect(c, e.state);
    return true;
}

// ---------------------------------------------------------------------------
// 内存管理
// ---------------------------------------------------------------------------

size_t History::bytes() const {
    size_t n = 0;
    for (const auto& e : undo_) n += e.state.bytes();
    for (const auto& e : redo_) n += e.state.bytes();
    n += snapshot_.bytes();
    return n;
}

void History::trim() {
    while ((int)undo_.size() > maxSteps) undo_.pop_front();
    while ((int)redo_.size() > maxSteps) redo_.pop_front();
    while (bytes() > maxBytes && undo_.size() > 1) undo_.pop_front();
}

const std::string& History::undoLabel() const {
    return undo_.empty() ? kEmptyLabel : undo_.back().label;
}

const std::string& History::redoLabel() const {
    return redo_.empty() ? kEmptyLabel : redo_.back().label;
}

}  // namespace pb
