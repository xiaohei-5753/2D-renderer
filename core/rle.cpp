// core/rle.cpp
#include "core/rle.h"

#include <cstring>

namespace pb {

namespace {

// 统计从 i 开始、与 src[i] 相同的连续字节数（上限 129，正好一个重复包的容量）
size_t runLength(const uint8_t* s, size_t n, size_t i) {
    size_t k = 1;
    while (i + k < n && k < 129 && s[i + k] == s[i]) k++;
    return k;
}

}  // namespace

size_t rleEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    const size_t start = out.size();
    size_t i = 0;
    while (i < n) {
        const size_t run = runLength(src, n, i);
        // 只有 >= 2 的游程才值得用重复包：单字节走字面包（tag 更小）
        if (run >= 2) {
            out.push_back((uint8_t)(0x80 | (run - 2)));
            out.push_back(src[i]);
            i += run;
            continue;
        }
        // 字面包：一直收，直到遇到"长度 >= 3 的游程"（2 个相同字节走字面更省：
        // 字面包里 2 字节只花 2 字节，而重复包要 2 字节 —— 打平，所以用 3 作为
        // 切分门槛，能少一次 tag）
        size_t j = i;
        while (j < n && j - i < 128) {
            if (runLength(src, n, j) >= 3) break;
            j++;
        }
        const size_t lit = j - i;
        out.push_back((uint8_t)(lit - 1));
        out.insert(out.end(), src + i, src + j);
        i = j;
    }
    return out.size() - start;
}

bool rleDecode(const std::vector<uint8_t>& src, size_t* pos, size_t rawBytes,
               std::vector<uint8_t>& out) {
    if (!pos) return false;
    size_t p = *pos;
    size_t produced = 0;
    while (produced < rawBytes) {
        if (p >= src.size()) return false;   // 截断
        const uint8_t tag = src[p++];
        if (tag < 0x80) {
            const size_t cnt = (size_t)tag + 1;
            if (p + cnt > src.size()) return false;
            if (produced + cnt > rawBytes) return false;   // 多出来的字节 = 损坏
            out.insert(out.end(), src.begin() + (std::ptrdiff_t)p,
                       src.begin() + (std::ptrdiff_t)(p + cnt));
            p += cnt;
            produced += cnt;
        } else {
            const size_t cnt = (size_t)(tag & 0x7F) + 2;
            if (p >= src.size()) return false;
            if (produced + cnt > rawBytes) return false;
            out.insert(out.end(), cnt, src[p]);
            p++;
            produced += cnt;
        }
    }
    *pos = p;
    return true;
}

}  // namespace pb
