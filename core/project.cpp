// core/project.cpp
#include "core/project.h"

#include <cstdio>
#include <cstring>

#include "core/layer.h"
#include "core/palette.h"
#include "core/rle.h"
#include "core/tool.h"
#include "gfx/light_sweep.h"

namespace pb {

namespace {

// ---------------------------------------------------------------------------
// 小工具：读写原语。全部小端、逐字段检查边界。
// ---------------------------------------------------------------------------

struct Writer {
    std::vector<uint8_t>& b;
    void u8v(uint8_t v) { b.push_back(v); }
    void u16v(uint16_t v) {
        b.push_back((uint8_t)(v & 0xFF));
        b.push_back((uint8_t)(v >> 8));
    }
    void u32v(uint32_t v) {
        for (int i = 0; i < 4; i++) b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
    }
    void f32v(float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        u32v(u);
    }
    void bytes(const void* p, size_t n) {
        const uint8_t* q = (const uint8_t*)p;
        b.insert(b.end(), q, q + n);
    }
    void str(const std::string& s) {
        const uint16_t n = (uint16_t)(s.size() > 0xFFFF ? 0xFFFF : s.size());
        u16v(n);
        b.insert(b.end(), s.begin(), s.begin() + n);
    }
};

struct Reader {
    const std::vector<uint8_t>& b;
    size_t p = 0;
    bool ok = true;
    const char* err = nullptr;

    bool need(size_t n) {
        if (!ok) return false;
        if (p + n > b.size()) { ok = false; err = "数据被截断"; return false; }
        return true;
    }
    uint8_t u8v() { if (!need(1)) return 0; return b[p++]; }
    uint16_t u16v() {
        if (!need(2)) return 0;
        const uint16_t v = (uint16_t)(b[p] | (b[p + 1] << 8));
        p += 2;
        return v;
    }
    uint32_t u32v() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t)b[p + (size_t)i] << (8 * i);
        p += 4;
        return v;
    }
    float f32v() {
        const uint32_t u = u32v();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    void bytes(void* dst, size_t n) {
        if (!need(n)) return;
        std::memcpy(dst, b.data() + p, n);
        p += n;
    }
    std::string str() {
        const uint16_t n = u16v();
        if (!need(n)) return std::string();
        std::string s((const char*)b.data() + p, n);
        p += n;
        return s;
    }
};

// 压缩一段字节并写进流：u32 rawLen, u32 compLen, comp[]
void writeBlob(Writer& w, const uint8_t* src, size_t n) {
    std::vector<uint8_t> comp;
    rleEncode(src, n, comp);
    w.u32v((uint32_t)n);
    w.u32v((uint32_t)comp.size());
    w.bytes(comp.data(), comp.size());
}

// 读回一段字节。expectedRaw > 0 时校验长度（防止损坏的头导致巨量分配）。
bool readBlob(Reader& r, size_t expectedRaw, std::vector<uint8_t>& out) {
    const uint32_t raw = r.u32v();
    const uint32_t comp = r.u32v();
    if (!r.ok) return false;
    if (raw != expectedRaw) { r.ok = false; r.err = "像素数据长度与画布尺寸不符"; return false; }
    // 压缩数据不可能比原数据大太多（最坏情况是每 128 字节多 1 个 tag）
    if (comp > raw + raw / 64 + 64) { r.ok = false; r.err = "压缩长度异常"; return false; }
    if (!r.need(comp)) return false;

    out.clear();
    out.reserve(raw);
    size_t pos = r.p;
    if (!rleDecode(r.b, &pos, raw, out)) { r.ok = false; r.err = "游程数据损坏"; return false; }
    if (out.size() != raw) { r.ok = false; r.err = "解压长度不符"; return false; }
    if (pos != r.p + comp) { r.ok = false; r.err = "压缩长度字段与实际不符"; return false; }
    r.p = pos;
    return true;
}

// 逐页眉：u32(4) 比较麻烦，magic 直接按字节写
const uint8_t kMagic[4] = {'P', 'B', 'P', 'J'};

}  // namespace

bool projectToBytes(const ProjectState& st, std::vector<uint8_t>& out) {
    if (!st.doc) return false;
    Document& d = *st.doc;
    if (d.width() <= 0 || d.height() <= 0) return false;

    out.clear();
    Writer w{out};

    // ---- 头 ----
    w.bytes(kMagic, 4);
    w.u32v(kProjectVersion);
    w.u32v((uint32_t)d.width());
    w.u32v((uint32_t)d.height());
    w.u32v((uint32_t)d.layerCount());
    w.u32v((uint32_t)(d.activeIndex() < 0 ? 0 : d.activeIndex()));

    const size_t px = (size_t)d.width() * (size_t)d.height();

    // ---- 图层 ----
    for (int i = 0; i < d.layerCount(); i++) {
        const Layer& L = d.layer(i);
        w.str(L.name);
        uint8_t flags = 0;
        if (L.visible) flags |= 1;
        if (L.locked) flags |= 2;
        w.u8v(flags);
        w.u8v(L.opacity);

        // albedo：w*h*4 字节，RGBA8 连续
        if (L.albedo.pixelCount() == px) {
            writeBlob(w, (const uint8_t*)L.albedo.data(), px * 4);
        } else {
            writeBlob(w, nullptr, 0);   // 尺寸不符时写空块，读回会当成"全空"
        }
        // 材质：w*h*4 字节（sizeof(Material) == 4，已静态断言）
        if (L.material.size() == px) {
            writeBlob(w, (const uint8_t*)L.material.data(), px * 4);
        } else {
            writeBlob(w, nullptr, 0);
        }
    }

    // ---- 调色板 ----
    if (st.palette) {
        const Palette& P = *st.palette;
        w.u32v((uint32_t)P.size());
        for (int i = 0; i < P.size(); i++) {
            const RGBA8 c = P.color(i);
            const Palette::Mat7 m = P.mat(i);
            // ⚠️ color.a 与 m.a 是**两个独立字段**，必须都存。
            // colors_ 与 mats_ 是平行数组：colors_[i].a 属于色板条目本身，
            // mats_[i].a 是"这一笔的遮挡程度"（见 Mat7 的注释）。
            // 早先把 color.a 写成 m.a，往返就丢了一份 —— 测试直接抓到了。
            w.u8v(c.r); w.u8v(c.g); w.u8v(c.b); w.u8v(c.a);
            w.u8v(m.eR); w.u8v(m.eG); w.u8v(m.eB); w.u8v(m.a);
        }
        w.u32v((uint32_t)(P.currentIndex() < 0 ? 0 : P.currentIndex()));
    } else {
        w.u32v(0);
        w.u32v(0);
    }

    // ---- 工具 ----
    if (st.tool) {
        const ToolOptions& t = *st.tool;
        w.u32v((uint32_t)t.tool);
        w.u8v(t.color.r); w.u8v(t.color.g); w.u8v(t.color.b); w.u8v(t.color.a);
        w.u8v(t.material.emR); w.u8v(t.material.emG); w.u8v(t.material.emB);
        w.u8v(t.material.ior);
        w.u32v((uint32_t)(t.brushSize < 1 ? 1 : t.brushSize));
        w.u8v(t.brushRound ? 1 : 0);
        w.u8v(t.shapeFilled ? 1 : 0);
        w.u32v((uint32_t)t.ditherLevel);
        w.u32v((uint32_t)t.shadeAmount);
    } else {
        w.u32v(0);   // tool id
        for (int i = 0; i < 8; i++) w.u8v(0);
        w.u32v(1); w.u8v(0); w.u8v(0); w.u32v(50); w.u32v(40);
    }

    // ---- 界面 + 视图 ----
    if (st.ui) {
        w.f32v(st.ui->leftWidth);
        w.f32v(st.ui->rightWidth);
        w.u8v(st.ui->leftOpen ? 1 : 0);
        w.u8v(st.ui->rightOpen ? 1 : 0);
        w.u8v(st.ui->showPixelGrid ? 1 : 0);
    } else {
        w.f32v(236.0f); w.f32v(322.0f);
        w.u8v(1); w.u8v(1); w.u8v(0);
    }
    if (st.view) {
        w.f32v(st.view->zoom);
        w.f32v(st.view->camX);
        w.f32v(st.view->camY);
    } else {
        w.f32v(1.0f); w.f32v(0.0f); w.f32v(0.0f);
    }

    // ---- 光照 ----
    if (st.light) {
        const gfx::LightSettings& s = *st.light;
        w.u32v((uint32_t)(s.rayRadius < 4 ? 4 : s.rayRadius));
        w.f32v(s.ambient[0]); w.f32v(s.ambient[1]); w.f32v(s.ambient[2]);
        w.f32v(s.emitScale);
        w.f32v(s.propagate);
        w.u8v(s.diffusionOn ? 1 : 0);
        w.f32v(s.diffusion);
        w.u32v((uint32_t)(s.diffusionStep < 1 ? 1 : s.diffusionStep));
    } else {
        w.u32v(128);
        w.f32v(0.25f); w.f32v(0.25f); w.f32v(0.25f);
        w.f32v(1.0f); w.f32v(1.0f);
        w.u8v(0); w.f32v(0.0f); w.u32v(8);
    }
    w.f32v(st.fieldStrength);
    // lightMode 保留给文件格式兼容：路径追踪已移除，一律写 0（见 core/project.h）。
    w.u32v(0);

    return true;
}

bool projectFromBytes(const std::vector<uint8_t>& in, ProjectState& st) {
    Reader r{in};
    if (!r.need(4)) { std::fprintf(stderr, "[proj] 文件为空\n"); return false; }
    if (std::memcmp(r.b.data(), kMagic, 4) != 0) {
        std::fprintf(stderr, "[proj] 不是工程文件（magic 不符）\n");
        return false;
    }
    r.p = 4;
    const uint32_t ver = r.u32v();
    if (ver > kProjectVersion) {
        std::fprintf(stderr, "[proj] 文件版本 %u 比本程序支持的 %u 更新，"
                             "请升级程序再打开\n", ver, kProjectVersion);
        return false;
    }
    const uint32_t w = r.u32v();
    const uint32_t h = r.u32v();
    const uint32_t nLayers = r.u32v();
    const uint32_t active = r.u32v();
    if (!r.ok) { std::fprintf(stderr, "[proj] 头部损坏\n"); return false; }
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        std::fprintf(stderr, "[proj] 画布尺寸非法 (%ux%u)\n", w, h);
        return false;
    }
    if (nLayers == 0 || nLayers > 4096) {
        std::fprintf(stderr, "[proj] 图层数非法 (%u)\n", nLayers);
        return false;
    }
    if (!st.doc) { std::fprintf(stderr, "[proj] 内部错误：没有文档\n"); return false; }
    if (!st.doc->layers().empty() && (st.doc->width() != (int)w || st.doc->height() != (int)h)) {
        std::fprintf(stderr, "[proj] 画布尺寸与当前文档不符：文件 %ux%u，当前 %dx%d\n",
                     w, h, st.doc->width(), st.doc->height());
        return false;
    }

    Document& d = *st.doc;
    d.resize((int)w, (int)h);
    // 图层数对不上就增减（resize 不会动图层，得自己来）
    while (d.layerCount() < (int)nLayers) d.addLayer("");
    while (d.layerCount() > (int)nLayers) d.removeLayer(d.layerCount() - 1);

    const size_t px = (size_t)w * (size_t)h;

    // ---- 图层 ----
    for (uint32_t i = 0; i < nLayers; i++) {
        Layer& L = d.layer((int)i);
        L.name = r.str();
        const uint8_t flags = r.u8v();
        L.opacity = r.u8v();
        L.visible = (flags & 1) != 0;
        L.locked = (flags & 2) != 0;

        std::vector<uint8_t> albedo, mat;
        if (!readBlob(r, px * 4, albedo)) { std::fprintf(stderr, "[proj] 第 %u 层 albedo 读取失败：%s\n", i, r.err ? r.err : "?"); return false; }
        if (!readBlob(r, px * 4, mat)) { std::fprintf(stderr, "[proj] 第 %u 层材质读取失败：%s\n", i, r.err ? r.err : "?"); return false; }
        if (albedo.size() == px * 4) std::memcpy(L.albedo.data(), albedo.data(), px * 4);
        if (mat.size() == px * 4) std::memcpy(L.material.data(), mat.data(), px * 4);
        L.markAllDirty();
    }
    d.setActiveIndex((int)active);

    // ---- 调色板 ----
    if (st.palette) {
        Palette& P = *st.palette;
        const uint32_t cnt = r.u32v();
        if (cnt > 65536) { std::fprintf(stderr, "[proj] 调色板项数异常 (%u)\n", cnt); return false; }
        std::vector<RGBA8> cols;
        std::vector<Palette::Mat7> mats;
        cols.reserve(cnt); mats.reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t cr = r.u8v(), cg = r.u8v(), cb = r.u8v(), ca = r.u8v();
            Palette::Mat7 m;
            m.eR = r.u8v(); m.eG = r.u8v(); m.eB = r.u8v(); m.a = r.u8v();
            m.r = cr; m.g = cg; m.b = cb;
            // 用文件里的 ca，不是 m.a —— 两者是不同的东西
            cols.push_back(RGBA8{cr, cg, cb, ca});
            mats.push_back(m);
        }
        const uint32_t cur = r.u32v();
        if (!r.ok) { std::fprintf(stderr, "[proj] 调色板损坏\n"); return false; }
        // 整表替换。**不能**用 removeColor 逐个清 —— 它有"至少保留 1 个"的
        // 保护，逐个删会在只剩 1 个时死循环（踩过）。而且"从磁盘恢复整份
        // 色板"本来就不归那条规则管。
        P.setAll(cols, mats, (int)cur);
    } else {
        (void)r.u32v(); (void)r.u32v();
    }

    // ---- 工具 ----
    {
        const uint32_t toolId = r.u32v();
        RGBA8 c;
        c.r = r.u8v(); c.g = r.u8v(); c.b = r.u8v(); c.a = r.u8v();
        Material m;
        m.emR = r.u8v(); m.emG = r.u8v(); m.emB = r.u8v(); m.ior = r.u8v();
        const uint32_t bs = r.u32v();
        const uint8_t round = r.u8v();
        const uint8_t filled = r.u8v();
        const uint32_t dither = r.u32v();
        const uint32_t shade = r.u32v();
        if (!r.ok) { std::fprintf(stderr, "[proj] 工具段损坏\n"); return false; }
        if (st.tool) {
            st.tool->tool = (ToolId)(toolId < (uint32_t)ToolId::Count ? toolId : 0);
            st.tool->color = c;
            st.tool->material = m;
            st.tool->brushSize = (int)(bs < 1 ? 1 : (bs > 64 ? 64 : bs));
            st.tool->brushRound = round != 0;
            st.tool->shapeFilled = filled != 0;
            st.tool->ditherLevel = (int)(dither > 100 ? 100 : dither);
            st.tool->shadeAmount = (int)(shade > 255 ? 255 : shade);
        }
    }

    // ---- 界面 + 视图 ----
    {
        const float lw = r.f32v(), rw = r.f32v();
        const uint8_t lo = r.u8v(), ro = r.u8v(), grid = r.u8v();
        const float zoom = r.f32v(), cx = r.f32v(), cy = r.f32v();
        if (!r.ok) { std::fprintf(stderr, "[proj] 界面段损坏\n"); return false; }
        if (st.ui) {
            st.ui->leftWidth = lw;
            st.ui->rightWidth = rw;
            st.ui->leftOpen = lo != 0;
            st.ui->rightOpen = ro != 0;
            st.ui->showPixelGrid = grid != 0;
        }
        if (st.view) {
            st.view->zoom = (zoom > 0.01f && zoom < 64.0f) ? zoom : 1.0f;
            st.view->camX = cx;
            st.view->camY = cy;
        }
    }

    // ---- 光照 ----
    {
        const uint32_t rr = r.u32v();
        float amb[3];
        for (int i = 0; i < 3; i++) amb[i] = r.f32v();
        const float em = r.f32v();
        const float cp = r.f32v();
        const uint8_t don = r.u8v();
        const float dw = r.f32v();
        const uint32_t dk = r.u32v();
        const float fs = r.f32v();
        const uint32_t mode = r.u32v();
        if (!r.ok) { std::fprintf(stderr, "[proj] 光照段损坏\n"); return false; }
        if (st.light) {
            st.light->rayRadius = (int)(rr < 4 ? 128 : (rr > 512 ? 512 : rr));
            for (int i = 0; i < 3; i++) st.light->ambient[i] = amb[i];
            st.light->emitScale = em;
            st.light->propagate = cp;
            st.light->diffusionOn = don != 0;
            st.light->diffusion = dw;
            st.light->diffusionStep = (int)(dk < 1 ? 8 : (dk > 64 ? 64 : dk));
        }
        st.fieldStrength = fs;
        // mode 是历史字段：读出来就丢掉（旧工程可能是 1 = 路径追踪，
        // 但那条路已于 2026-10-19 移除，现在一律按扫描 GI 打开）。
        (void)mode;
    }

    return true;
}

// ---------------------------------------------------------------------------
// 文件系统包装
// ---------------------------------------------------------------------------

bool saveProject(const std::string& path, const ProjectState& st) {
    std::vector<uint8_t> buf;
    if (!projectToBytes(st, buf)) {
        std::fprintf(stderr, "[proj] 序列化失败（文档为空？）\n");
        return false;
    }

    // 先写临时文件，成功后再替换目标文件 —— 工程文件是用户的劳动成果，
    // 覆盖写一半是最不能接受的失败方式。
    //
    // ⚠️ 这里**不能**为了"探测目标能不能写"而先用 "wb" 打开 path：
    // "wb" 会当场把已有的工程截断成 0 字节。原来正是这么写的，于是"临时文件
    // 保护"从第一步就失效了 —— 只要后面 tmp 建不出来（比如同名目录占位）
    // 或者写一半失败，用户的工程已经没了。现在只碰 tmp，目标文件一个字节不动。
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[proj] 无法写入 %s（原文件未改动）\n", tmp.c_str());
        return false;
    }
    const size_t wrote = std::fwrite(buf.data(), 1, buf.size(), f);
    // fflush 让"磁盘满"在这里暴露，而不是拖到 fclose 里被 silent 掉
    const bool flushed = std::fflush(f) == 0;
    std::fclose(f);
    if (wrote != buf.size() || !flushed) {
        std::fprintf(stderr, "[proj] 写入不完整（磁盘满？），原文件未改动\n");
        std::remove(tmp.c_str());
        return false;
    }

    // Windows 的 std::rename 不能覆盖已存在的文件，所以先删旧文件再改名。
    // 剩余风险只有一个极小的窗口（删掉旧文件后 rename 失败）：此时 tmp 是
    // **完整**的，所以刻意**不删**它，并在报错里指明 —— 用户手里至少还有
    // 一份能救回来的 .tmp，而不是两份都丢了。
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr,
                     "[proj] 改名失败；新内容已完整写入 %s，请手动改名\n", tmp.c_str());
        return false;
    }
    std::printf("[proj] 已保存 %s（%.1f KB）\n", path.c_str(), (double)buf.size() / 1024.0);
    return true;
}

bool loadProject(const std::string& path, ProjectState& st) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "[proj] 打不开 %s\n", path.c_str()); return false; }
    std::vector<uint8_t> buf;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) buf.insert(buf.end(), chunk, chunk + n);
    const bool readErr = std::ferror(f) != 0;
    std::fclose(f);
    if (readErr) { std::fprintf(stderr, "[proj] 读取 %s 出错\n", path.c_str()); return false; }
    if (!projectFromBytes(buf, st)) return false;
    std::printf("[proj] 已读取 %s（%.1f KB，%d 层 %dx%d）\n", path.c_str(),
                (double)buf.size() / 1024.0, st.doc ? st.doc->layerCount() : 0,
                st.doc ? st.doc->width() : 0, st.doc ? st.doc->height() : 0);
    return true;
}

}  // namespace pb
