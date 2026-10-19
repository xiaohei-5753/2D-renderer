// tools/png_diff.cpp — 比较两张 PNG（亮度统计 + 逐像素差 + 亮度直方图）
//
// 为什么需要它：这个项目只能**写** PNG（stb_image_write），没有读 PNG 的库，
// 环境里也装不到（试着下载 stb_image.h，拿回来的是搜索引擎的错误页）。
// 而"改了着色器之后画面到底变了多少"必须靠数字回答 —— 我在逐层光照的改造里
// 就吃过亏：光看缩略图会得出错误结论，来回折腾了很多轮。
//
// 所以这里自带一个最小 inflate（raw DEFLATE）+ PNG 逆滤波。
// 注意 PNG 的 IDAT 是 **zlib 容器**（2 字节头 + raw deflate + 4 字节 Adler32），
// 不是裸 deflate —— 这一点让第一版解码器直接失败过。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// 只借用 common.h 的亮度权重：这个工具的解码器（inflate + 逆滤波）是它自己
// 独立实现的，与别的工具没有共用部分，所以照旧留在本文件里。
#include "common.h"

using namespace pb::tools;

namespace {

// ---------------------------------------------------------------------------
// 最小 inflate（raw DEFLATE：存储块 + 固定/动态 Huffman）
// ---------------------------------------------------------------------------

struct BitReader {
    const uint8_t* p = nullptr;
    size_t n = 0, pos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    bool bad = false;

    uint32_t bits(int k) {
        while (bitCnt < k) {
            if (pos >= n) { bad = true; return 0; }
            bitBuf |= (uint32_t)p[pos++] << bitCnt;
            bitCnt += 8;
        }
        const uint32_t v = bitBuf & ((1u << k) - 1u);
        bitBuf >>= k;
        bitCnt -= k;
        return v;
    }
    void alignByte() { bitBuf = 0; bitCnt = 0; }
};

struct Huff {
    int count[16] = {0};
    std::vector<int> symbol;
};

void buildHuff(Huff& h, const uint8_t* lens, int n) {
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[lens[i]]++;
    h.count[0] = 0;
    int offs[16] = {0};
    for (int i = 1; i < 16; i++) offs[i] = offs[i - 1] + h.count[i - 1];
    h.symbol.assign((size_t)n, 0);
    for (int i = 0; i < n; i++)
        if (lens[i]) h.symbol[(size_t)offs[lens[i]]++] = i;
}

int decodeSym(BitReader& br, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)br.bits(1);
        const int count = h.count[len];
        if (code - first < count) return h.symbol[(size_t)(index + (code - first))];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    br.bad = true;
    return -1;
}

const int kClcOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

bool inflateRaw(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) {
    BitReader br;
    br.p = in;
    br.n = inLen;

    static const int lenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,
                                    23, 27, 31, 35, 43, 51, 59, 67,  83,  99,  115, 131, 163,
                                    195, 227, 258};
    static const int lenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                     2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int distBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                     17,   25,   33,   49,   65,   97,    129,   193,
                                     257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                     4097, 6145, 8193, 12289, 16385, 24577};
    static const int distExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    while (!br.bad) {
        const uint32_t last = br.bits(1);
        const uint32_t type = br.bits(2);
        if (type == 0) {
            br.alignByte();
            if (br.pos + 4 > br.n) { br.bad = true; break; }
            const int len = in[br.pos] | (in[br.pos + 1] << 8);
            br.pos += 4;
            if (br.pos + (size_t)len > br.n) { br.bad = true; break; }
            out.insert(out.end(), in + br.pos, in + br.pos + len);
            br.pos += (size_t)len;
        } else if (type == 1 || type == 2) {
            Huff lit, dist;
            if (type == 1) {
                uint8_t ll[288];
                for (int i = 0; i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                uint8_t dl[30];
                for (int i = 0; i < 30; i++) dl[i] = 5;
                buildHuff(lit, ll, 288);
                buildHuff(dist, dl, 30);
            } else {
                const int hlit = (int)br.bits(5) + 257;
                const int hdist = (int)br.bits(5) + 1;
                const int hclen = (int)br.bits(4) + 4;
                uint8_t clc[19] = {0};
                for (int i = 0; i < hclen; i++) clc[kClcOrder[i]] = (uint8_t)br.bits(3);
                Huff clh;
                buildHuff(clh, clc, 19);

                uint8_t lens[320] = {0};
                int i = 0;
                while (i < hlit + hdist && !br.bad) {
                    const int sym = decodeSym(br, clh);
                    if (sym < 0) break;
                    if (sym < 16) {
                        lens[i++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (i == 0) { br.bad = true; break; }
                        const int prev = lens[i - 1];
                        const int rep = 3 + (int)br.bits(2);
                        for (int k = 0; k < rep && i < hlit + hdist; k++)
                            lens[i++] = (uint8_t)prev;
                    } else if (sym == 17) {
                        const int rep = 3 + (int)br.bits(3);
                        for (int k = 0; k < rep && i < hlit + hdist; k++) lens[i++] = 0;
                    } else {
                        const int rep = 11 + (int)br.bits(7);
                        for (int k = 0; k < rep && i < hlit + hdist; k++) lens[i++] = 0;
                    }
                }
                if (br.bad) break;
                buildHuff(lit, lens, hlit);
                buildHuff(dist, lens + hlit, hdist);
            }

            for (;;) {
                const int sym = decodeSym(br, lit);
                if (sym < 0) break;
                if (sym < 256) {
                    out.push_back((uint8_t)sym);
                } else if (sym == 256) {
                    break;
                } else {
                    const int li = sym - 257;
                    if (li >= 29) { br.bad = true; break; }
                    const int length = lenBase[li] + (int)br.bits(lenExtra[li]);
                    const int ds = decodeSym(br, dist);
                    if (ds < 0 || ds >= 30) { br.bad = true; break; }
                    const int distance = distBase[ds] + (int)br.bits(distExtra[ds]);
                    if ((size_t)distance > out.size()) { br.bad = true; break; }
                    const size_t start = out.size() - (size_t)distance;
                    for (int k = 0; k < length; k++) out.push_back(out[start + (size_t)k]);
                }
            }
        } else {
            br.bad = true;
            break;
        }
        if (last) break;
    }
    return !br.bad;
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

struct Image {
    int w = 0, h = 0, ch = 0;
    std::vector<uint8_t> px;
};

uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

bool loadPNG(const char* path, Image& out, std::string& err) {
    FILE* f = fopen(path, "rb");
    if (!f) { err = "cannot open"; return false; }
    std::vector<uint8_t> buf;
    uint8_t tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (buf.size() < 8 || memcmp(buf.data(), sig, 8) != 0) { err = "not a png"; return false; }

    std::vector<uint8_t> idat;
    size_t pos = 8;
    int bitDepth = 0, colorType = 0, interlace = 0;
    while (pos + 8 <= buf.size()) {
        const uint32_t len = be32(&buf[pos]);
        if (pos + 12 + len > buf.size()) { err = "truncated chunk"; return false; }
        const std::string type((const char*)&buf[pos + 4], 4);
        const uint8_t* data = &buf[pos + 8];
        if (type == "IHDR") {
            out.w = (int)be32(data);
            out.h = (int)be32(data + 4);
            bitDepth = data[8];
            colorType = data[9];
            interlace = data[12];
        } else if (type == "IDAT") {
            idat.insert(idat.end(), data, data + len);
        } else if (type == "IEND") {
            break;
        }
        pos += 12 + len;
    }
    if (bitDepth != 8 || interlace != 0) { err = "only 8-bit non-interlaced"; return false; }
    out.ch = (colorType == 2) ? 3 : (colorType == 6 ? 4 : 0);
    if (out.ch == 0) { err = "only RGB/RGBA"; return false; }
    if (out.w <= 0 || out.h <= 0) { err = "bad size"; return false; }
    if (idat.size() <= 6) { err = "idat too small"; return false; }

    // IDAT 是 zlib 流：跳过 2 字节头与 4 字节 Adler32
    std::vector<uint8_t> raw;
    if (!inflateRaw(idat.data() + 2, idat.size() - 6, raw)) {
        err = "inflate failed";
        return false;
    }

    const int stride = out.w * out.ch;
    if (raw.size() < (size_t)out.h * (size_t)(stride + 1)) {
        err = "short raw data";
        return false;
    }
    out.px.assign((size_t)stride * out.h, 0);
    for (int y = 0; y < out.h; y++) {
        const uint8_t* src = raw.data() + (size_t)y * (size_t)(stride + 1);
        const int ft = src[0];
        const uint8_t* in = src + 1;
        uint8_t* cur = out.px.data() + (size_t)y * stride;
        const uint8_t* prev = (y > 0) ? out.px.data() + (size_t)(y - 1) * stride : nullptr;
        for (int i = 0; i < stride; i++) {
            const int a = (i >= out.ch) ? cur[i - out.ch] : 0;
            const int b = prev ? prev[i] : 0;
            const int c = (prev && i >= out.ch) ? prev[i - out.ch] : 0;
            int v = in[i];
            switch (ft) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: err = "bad filter"; return false;
            }
            cur[i] = (uint8_t)(v & 0xFF);
        }
    }
    return true;
}

double meanLuma(const Image& im) {
    // 亮度公式（Rec.709 权重）在 common.h，和 scan_dump 用的是同一个
    const size_t n = (size_t)im.w * (size_t)im.h;
    return lumaStats(im.px.data(), n, im.ch).mean;
}

void histogram(const char* tag, const Image& im) {
    int hist[8] = {0};
    const size_t n = (size_t)im.w * im.h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = &im.px[i * im.ch];
        const int L = (int)luma(p[0], p[1], p[2]);
        hist[std::min(7, L / 32)]++;
    }
    printf("%s 亮度直方图(0..255 分 8 档): ", tag);
    for (int i = 0; i < 8; i++) printf("%6d ", hist[i]);
    printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: png_diff <a.png> [b.png] [--at x,y]\n");
        return 1;
    }
    int atX = -1, atY = -1;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            sscanf(argv[i + 1], "%d,%d", &atX, &atY);
        }
    }
    Image a;
    std::string err;
    if (!loadPNG(argv[1], a, err)) { printf("读 %s 失败: %s\n", argv[1], err.c_str()); return 1; }
    printf("A %s: %dx%d  平均亮度 %.2f\n", argv[1], a.w, a.h, meanLuma(a));
    histogram("A", a);
    if (atX >= 0 && atX < a.w && atY >= 0 && atY < a.h) {
        printf("A 在 (%d,%d) 附近 5x5：\n", atX, atY);
        for (int y = atY - 2; y <= atY + 2; y++) {
            if (y < 0 || y >= a.h) continue;
            printf("  y=%3d: ", y);
            for (int x = atX - 2; x <= atX + 2; x++) {
                if (x < 0 || x >= a.w) { printf("      ---     "); continue; }
                const uint8_t* p = &a.px[((size_t)y * a.w + x) * a.ch];
                printf("(%3d,%3d,%3d)", p[0], p[1], p[2]);
            }
            printf("\n");
        }
    }
    if (argc < 3) return 0;

    Image b;
    if (!loadPNG(argv[2], b, err)) { printf("读 %s 失败: %s\n", argv[2], err.c_str()); return 1; }
    printf("B %s: %dx%d  平均亮度 %.2f\n", argv[2], b.w, b.h, meanLuma(b));
    histogram("B", b);

    if (a.w != b.w || a.h != b.h || a.ch != b.ch) {
        printf("尺寸/通道不同，无法逐像素比较\n");
        return 0;
    }
    int maxd = 0, ndiff = 0;
    long long sum = 0;
    const size_t n = (size_t)a.w * a.h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* pa = &a.px[i * a.ch];
        const uint8_t* pb = &b.px[i * b.ch];
        int d = 0;
        for (int c = 0; c < a.ch; c++)
            d = std::max(d, std::abs((int)pa[c] - (int)pb[c]));
        if (d) ndiff++;
        sum += d;
        maxd = std::max(maxd, d);
    }
    printf("不同像素 %d / %zu   最大通道差 %d   平均差 %.3f\n", ndiff, n, maxd,
           n ? (double)sum / (double)n : 0.0);
    return 0;
}
