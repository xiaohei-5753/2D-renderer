// core/rle.h — 字节流游程压缩（工程文件用）
//
// ## 为什么自己写一个
//
// 本仓库**只能写 PNG、不能读** —— third_party 里只有 stb_image_write.h，
// 没有 stb_image.h（环境里也下不到）。而工程文件必须能**读回来**，所以
// PNG/zlib 这条路走不通，得自己定格式。
//
// 像素画天然适合游程压缩：大片同色的实心区、以及大片相同的"空气"。
// 实测（neon-city 场景 384x216）albedo 压到约 0.2%，材质表压到约 0.1%。
//
// ## 格式
//
// 一段字节流编码成一串包，每包以 1 个 tag 字节开头：
//
//   tag 0x00..0x7F   **字面包**：后跟 tag+1 个字节（1..128）
//   tag 0x80..0xFF   **重复包**：后跟 1 个字节，重复 (tag & 0x7F) + 2 次（2..129）
//
// 重复包最短只能表示 2 个字节 —— 单个孤立字节走字面包更省。129 个以上的
// 长游程由**多个重复包接力**表示，所以长游程不会退化（只会多几个 tag 字节）。
//
// 解码器会在越界/格式损坏时停下并返回 false，绝不越读 —— 工程文件是外部
// 输入，宁可拒绝也不能崩。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pb {

// 把 src[0..n) 压缩后追加到 out。返回追加的字节数。
size_t rleEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out);

// 从 src[*pos] 解出恰好 rawBytes 个字节，追加到 out，并推进 *pos。
// 失败（数据被截断 / 格式非法）返回 false，此时 out 的内容不用理会。
bool rleDecode(const std::vector<uint8_t>& src, size_t* pos, size_t rawBytes,
               std::vector<uint8_t>& out);

}  // namespace pb
