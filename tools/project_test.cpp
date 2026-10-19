// tools/project_test.cpp — 工程文件的往返与健壮性测试
//
// ## 为什么值得单独一个工具
//
// 工程文件是**用户劳动成果的唯一载体**。存坏了 = 作品没了，比渲染出错严重得多。
// 所以这里不只测"能存能读"，还要测：
//
//   1. **往返逐字节相同**：存了再读，每个图层的 albedo 与材质表必须完全一致。
//      注意不能用视觉对比 —— 那只能证明"看起来差不多"。
//   2. **压缩是无损的**：RLE 编解码对随机数据、长游程、单字节交替都必须还原。
//   3. **损坏的文件被拒绝而不是崩溃**：截断、magic 错、版本过新、字段越界。
//      工程文件是外部输入，宁可报错也不能崩。
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "core/console.h"
#include "core/layer.h"
#include "core/palette.h"
#include "core/paths.h"
#include "core/project.h"
#include "core/rle.h"
#include "core/tool.h"
#include "gfx/light_sweep.h"

using namespace pb;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* what) {
    if (ok) { g_pass++; }
    else { g_fail++; std::printf("  FAIL  %s\n", what); }
}

// 构造一个"什么都有"的文档：多层、不同可见性/不透明度、逐像素材质、
// 含自发光与折射率 —— 覆盖所有会被序列化的字段。
Document makeDoc() {
    Document d;
    d.resize(37, 23);   // 故意用非 2 的幂、非 16 倍数，容易暴露 stride 错误
    while (d.layerCount() < 3) d.addLayer("");
    const int W = d.width(), H = d.height();

    // 层 0：纯色 + 一个折射像素
    {
        Layer& L = d.layer(0);
        L.name = "底层";
        L.albedo.fill(RGBA8{10, 20, 30, 255});
        for (auto& m : L.material) m = Material{};
        Material m;
        m.setIorValue(2.4f);
        L.setPixelRaw(5, 7, RGBA8{200, 100, 50, 255}, m);
        L.setPixelRaw(W - 1, H - 1, RGBA8{1, 2, 3, 4}, m);
    }
    // 层 1：图案 + 自发光，且**不可见**
    {
        Layer& L = d.layer(1);
        L.name = "发光层 (测试中文名)";
        L.visible = false;
        L.opacity = 137;
        L.locked = true;
        L.albedo.fill(RGBA8{0, 0, 0, 0});
        for (auto& m : L.material) m = Material{};
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if ((x + y) % 3) continue;
                Material m;
                m.setEmissionFrom((uint8_t)(x * 7), (uint8_t)(y * 11), 200);
                L.setPixelRaw(x, y, RGBA8{(uint8_t)x, (uint8_t)y, 99, 255}, m);
            }
    }
    // 层 2：完全空层（考验零长游程）
    {
        Layer& L = d.layer(2);
        L.name = "";
        L.albedo.fill(RGBA8{0, 0, 0, 0});
        for (auto& m : L.material) m = Material{};
    }
    d.setActiveIndex(1);
    return d;
}

void compareDoc(const Document& a, const Document& b, const char* tag) {
    check(a.width() == b.width() && a.height() == b.height(), "尺寸一致");
    check(a.layerCount() == b.layerCount(), "图层数一致");
    if (a.layerCount() != b.layerCount()) return;
    check(a.activeIndex() == b.activeIndex(), "活动图层一致");
    for (int i = 0; i < a.layerCount(); i++) {
        const Layer& la = a.layer(i);
        const Layer& lb = b.layer(i);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s: 层%d 名字", tag, i);
        check(la.name == lb.name, buf);
        std::snprintf(buf, sizeof(buf), "%s: 层%d 可见性/不透明度/锁定", tag, i);
        check(la.visible == lb.visible && la.opacity == lb.opacity && la.locked == lb.locked, buf);

        std::snprintf(buf, sizeof(buf), "%s: 层%d albedo 逐像素", tag, i);
        bool same = true;
        if (la.albedo.pixelCount() != lb.albedo.pixelCount()) same = false;
        else same = std::memcmp(la.albedo.data(), lb.albedo.data(),
                                la.albedo.pixelCount() * sizeof(RGBA8)) == 0;
        check(same, buf);

        std::snprintf(buf, sizeof(buf), "%s: 层%d 材质表逐像素", tag, i);
        same = la.material.size() == lb.material.size() &&
               (la.material.empty() ||
                std::memcmp(la.material.data(), lb.material.data(),
                            la.material.size() * sizeof(Material)) == 0);
        check(same, buf);
    }
}

// ---------------------------------------------------------------------------

void testRle() {
    std::printf("[RLE 编解码]\n");
    struct Case { const char* name; std::vector<uint8_t> data; };
    std::vector<Case> cases;

    cases.push_back({"空", {}});
    cases.push_back({"单字节", {42}});
    cases.push_back({"两个相同", {7, 7}});
    cases.push_back({"三个相同", {7, 7, 7}});
    cases.push_back({"长游程 500", std::vector<uint8_t>(500, 0xAB)});
    cases.push_back({"恰好 129 长", std::vector<uint8_t>(129, 3)});
    cases.push_back({"恰好 130 长", std::vector<uint8_t>(130, 3)});
    // 交替字节：字面包的最坏情况
    {
        std::vector<uint8_t> v;
        for (int i = 0; i < 1000; i++) v.push_back((uint8_t)(i & 1));
        cases.push_back({"交替 1000", v});
    }
    // 伪随机：压缩不了的情况
    {
        std::vector<uint8_t> v;
        uint32_t s = 12345;
        for (int i = 0; i < 4096; i++) { s = s * 1103515245u + 12345u; v.push_back((uint8_t)(s >> 16)); }
        cases.push_back({"伪随机 4096", v});
    }
    // 混合：游程 + 噪声
    {
        std::vector<uint8_t> v;
        for (int i = 0; i < 50; i++) {
            v.insert(v.end(), 200, (uint8_t)i);
            for (int k = 0; k < 17; k++) v.push_back((uint8_t)(k * 13 + i));
        }
        cases.push_back({"混合", v});
    }

    for (const Case& c : cases) {
        std::vector<uint8_t> comp;
        rleEncode(c.data.data(), c.data.size(), comp);
        std::vector<uint8_t> back;
        size_t pos = 0;
        const bool ok = rleDecode(comp, &pos, c.data.size(), back);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "RLE %s: 解码成功", c.name);
        check(ok, buf);
        std::snprintf(buf, sizeof(buf), "RLE %s: 内容一致 (%zu -> %zu -> %zu)",
                      c.name, c.data.size(), comp.size(), back.size());
        check(back == c.data, buf);
        std::snprintf(buf, sizeof(buf), "RLE %s: 恰好消耗完压缩流", c.name);
        check(pos == comp.size(), buf);
    }
}

void testRoundTrip() {
    std::printf("[工程文件往返]\n");
    Document src = makeDoc();
    Palette pal;
    pal.loadPreset(0);
    pal.addColor(RGBA8{11, 22, 33, 44});
    pal.addColor(RGBA8{55, 66, 77, 88});
    Palette::Mat7 pm;
    pm.eR = 9; pm.eG = 8; pm.eB = 7; pm.a = 200;
    pal.setMat(1, pm);
    pal.setCurrent(2);

    ToolOptions tool;
    tool.tool = ToolId::Ellipse;
    tool.color = RGBA8{1, 2, 3, 4};
    tool.material.setEmissionFrom(5, 6, 7);
    tool.material.setIorValue(1.7f);
    tool.brushSize = 17;
    tool.brushRound = true;
    tool.shapeFilled = true;
    tool.ditherLevel = 63;
    tool.shadeAmount = 111;

    ViewState view;
    view.zoom = 3.25f;
    view.camX = -12.5f;
    view.camY = 44.75f;

    UiState ui;
    ui.leftWidth = 199.5f;
    ui.rightWidth = 301.25f;
    ui.leftOpen = false;
    ui.rightOpen = true;
    ui.showPixelGrid = true;

    gfx::LightSettings light;
    light.rayRadius = 32;
    light.ambient[0] = 0.125f; light.ambient[1] = 0.25f; light.ambient[2] = 0.5f;
    light.emitScale = 1.75f;
    light.propagate = 1.25f;
    light.diffusionOn = true;
    light.diffusion = 0.125f;
    light.diffusionStep = 4;

    ProjectState st;
    st.doc = &src; st.palette = &pal; st.tool = &tool;
    st.view = &view; st.ui = &ui; st.light = &light;
    st.lightMode = 1; st.fieldStrength = 1.375f;   // 1 = 旧路径追踪值，读取时应被忽略

    std::vector<uint8_t> bytes;
    check(projectToBytes(st, bytes), "序列化成功");
    std::printf("  未压缩像素约 %zu KB，序列化后 %zu KB\n",
                (size_t)(37 * 23 * 8 * 3 / 1024), bytes.size() / 1024);

    // 反序列化到一套全新的对象里
    Document dst;
    dst.resize(37, 23);
    Palette pal2;
    ToolOptions tool2;
    ViewState view2;
    UiState ui2;
    gfx::LightSettings light2;
    ProjectState st2;
    st2.doc = &dst; st2.palette = &pal2; st2.tool = &tool2;
    st2.view = &view2; st2.ui = &ui2; st2.light = &light2;

    check(projectFromBytes(bytes, st2), "反序列化成功");
    compareDoc(src, dst, "往返");

    check(pal2.size() == pal.size(), "调色板项数");
    check(pal2.currentIndex() == pal.currentIndex(), "调色板当前项");
    bool palSame = pal2.size() == pal.size();
    for (int i = 0; palSame && i < pal.size(); i++) {
        const RGBA8 a = pal.color(i), b = pal2.color(i);
        const Palette::Mat7 ma = pal.mat(i), mb = pal2.mat(i);
        const bool cOk = std::memcmp(&a, &b, 4) == 0;
        const bool mOk = std::memcmp(&ma, &mb, sizeof(ma)) == 0;
        if (!cOk || !mOk) {
            std::printf("  调色板项 %d 不同：\n", i);
            std::printf("    原 color=(%d,%d,%d,%d)  mat=(%d,%d,%d|e%d,%d,%d|a%d)\n",
                        a.r, a.g, a.b, a.a, ma.r, ma.g, ma.b, ma.eR, ma.eG, ma.eB, ma.a);
            std::printf("    新 color=(%d,%d,%d,%d)  mat=(%d,%d,%d|e%d,%d,%d|a%d)\n",
                        b.r, b.g, b.b, b.a, mb.r, mb.g, mb.b, mb.eR, mb.eG, mb.eB, mb.a);
            palSame = false;
        }
    }
    check(palSame, "调色板颜色+材质逐项一致");

    check(tool2.tool == tool.tool, "工具 id");
    check(std::memcmp(&tool2.color, &tool.color, 4) == 0, "工具颜色");
    check(tool2.material == tool.material, "工具材质");
    check(tool2.brushSize == tool.brushSize, "笔刷大小");
    check(tool2.brushRound == tool.brushRound, "笔刷形状");
    check(tool2.shapeFilled == tool.shapeFilled, "图形填充");
    check(tool2.ditherLevel == tool.ditherLevel, "抖动强度");
    check(tool2.shadeAmount == tool.shadeAmount, "加减光强度");

    check(view2.zoom == view.zoom && view2.camX == view.camX && view2.camY == view.camY, "视图状态");
    check(ui2.leftWidth == ui.leftWidth && ui2.rightWidth == ui.rightWidth, "栏宽");
    check(ui2.leftOpen == ui.leftOpen && ui2.rightOpen == ui.rightOpen &&
          ui2.showPixelGrid == ui.showPixelGrid, "栏开合与网格");

    check(light2.rayRadius == light.rayRadius, "光照半径");
    check(light2.ambient[0] == light.ambient[0] && light2.ambient[1] == light.ambient[1] &&
          light2.ambient[2] == light.ambient[2], "环境光");
    check(light2.emitScale == light.emitScale, "自发光倍率");
    check(light2.propagate == light.propagate, "沿程增益");
    check(light2.diffusionOn == light.diffusionOn, "扩散开关");
    check(light2.diffusion == light.diffusion, "扩散 w");
    check(light2.diffusionStep == light.diffusionStep, "扩散 K");
    // lightMode 只保留给文件格式兼容：保存写 0、读取忽略（路径追踪已移除）。
    check(st2.lightMode == 0, "lightMode 旧字段被忽略（不回填 1）");
    check(st2.fieldStrength == st.fieldStrength, "光场强度");
}

void testCorrupt() {
    std::printf("[损坏文件的处理]\n");
    Document src = makeDoc();
    Palette pal; pal.loadPreset(0);
    ToolOptions tool; ViewState view; UiState ui; gfx::LightSettings light;
    ProjectState st;
    st.doc = &src; st.palette = &pal; st.tool = &tool;
    st.view = &view; st.ui = &ui; st.light = &light;

    std::vector<uint8_t> good;
    projectToBytes(st, good);

    auto expectReject = [&](const std::vector<uint8_t>& bad, const char* what) {
        Document d; d.resize(37, 23);
        ProjectState st2; st2.doc = &d;
        const bool ok = projectFromBytes(bad, st2);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "拒绝：%s", what);
        check(!ok, buf);
    };

    expectReject({}, "空文件");
    { std::vector<uint8_t> b = good; b[0] = 'X'; expectReject(b, "magic 错误"); }
    { std::vector<uint8_t> b(good.begin(), good.begin() + 5); expectReject(b, "截断在头部"); }
    { std::vector<uint8_t> b(good.begin(), good.begin() + good.size() / 2); expectReject(b, "截断在中段"); }
    { std::vector<uint8_t> b = good; b[7] = 99; expectReject(b, "版本号过新"); }
    { std::vector<uint8_t> b = good; b[8] = 0; b[9] = 0; b[10] = 0; b[11] = 0; expectReject(b, "画布宽为 0"); }
    // 只改一个像素数据字节：长度字段仍自洽，这里**允许**被接受（数据本身合法），
    // 但要确认不会崩 —— 所以只要求"不崩"，不要求拒绝。
    {
        std::vector<uint8_t> b = good;
        if (b.size() > 60) b[60] ^= 0xFF;
        Document d; d.resize(37, 23);
        ProjectState st2; st2.doc = &d;
        projectFromBytes(b, st2);   // 不检查返回值：只要不崩就算过
        check(true, "单字节篡改不会崩");
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 保存的安全性：失败**不能**毁掉已有的工程
// ---------------------------------------------------------------------------
//
// 这里针对的是一个真实存在过的写法（core/project.cpp 的 saveProject）：
// 为了"探测目标能不能写"，先用 "wb" 打开目标文件 —— 那会当场把已有工程
// 截断成 0 字节 —— 然后才写 .tmp、最后改名。于是"临时文件保护"从第一步就
// 失效了：只要 .tmp 那一步失败（磁盘满、同名目录占位…），用户的工程已经没了。
//
// 测法：用一个**同名目录**占住 `<path>.tmp`，让 fopen(tmp,"wb") 必然失败，
// 然后逐字节确认目标文件还是原来那份。
namespace {

std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
        out.insert(out.end(), chunk, chunk + n);
    std::fclose(f);
    return out;
}

void testSaveGuard() {
    std::printf("[保存的安全性（失败不毁旧文件）]\n");

    // 写在 exe 同目录（build\），不污染仓库
    const std::string path = pb::executableDir() + "_projguard.pbproj";
    const std::string tmpDir = path + ".tmp";

    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);
    std::remove(path.c_str());

    // --- 先存一份正常的工程 ---
    Document a = makeDoc();
    Palette pal; pal.loadPreset(0);
    ToolOptions tool; ViewState view; UiState ui; gfx::LightSettings light;
    ProjectState stA;
    stA.doc = &a; stA.palette = &pal; stA.tool = &tool;
    stA.view = &view; stA.ui = &ui; stA.light = &light;

    std::vector<uint8_t> bytesA;
    check(projectToBytes(stA, bytesA), "基线工程可序列化");
    check(saveProject(path, stA), "保存基线工程");

    const std::vector<uint8_t> onDisk = readFileBytes(path);
    check(onDisk == bytesA, "落盘内容与序列化结果一致");

    // --- 占住 .tmp：这次保存必须失败 ---
    std::filesystem::create_directory(tmpDir, ec);
    check(!ec, "已用同名目录占住 .tmp（用来强制写失败）");

    Document b = makeDoc();
    b.resize(11, 5);           // 与基线明显不同，一旦被写进去一眼能看出来
    ProjectState stB;
    stB.doc = &b; stB.palette = &pal; stB.tool = &tool;
    stB.view = &view; stB.ui = &ui; stB.light = &light;

    check(!saveProject(path, stB), "写入失败时 saveProject 返回 false");

    // --- 关键断言：旧文件一个字节都没动 ---
    const std::vector<uint8_t> afterFail = readFileBytes(path);
    check(!afterFail.empty(), "失败后旧文件仍然存在");
    check(afterFail == bytesA, "失败后旧文件**逐字节**未被改动");

    // 还能正常读回来，内容与基线完全一致
    Document reloaded;
    ProjectState stReload;
    stReload.doc = &reloaded;
    check(loadProject(path, stReload), "失败后旧文件仍可读取");
    compareDoc(a, reloaded, "失败后旧文件内容");

    // --- 清掉占位目录后，覆盖保存必须正常工作 ---
    std::filesystem::remove_all(tmpDir, ec);
    check(saveProject(path, stB), "清掉障碍后可覆盖保存");
    Document reloaded2;
    ProjectState stReload2;
    stReload2.doc = &reloaded2;
    check(loadProject(path, stReload2), "覆盖后可读取");
    check(reloaded2.width() == 11 && reloaded2.height() == 5, "覆盖后内容是新工程");

    std::remove(path.c_str());
    std::filesystem::remove_all(tmpDir, ec);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 不缓冲：卡在哪一步要能立刻看到
    std::printf("=== 工程文件测试 ===\n");
    setupConsoleUtf8();
    testRle();
    testRoundTrip();
    testCorrupt();
    testSaveGuard();
    std::printf("\n%s  通过 %d，失败 %d\n", g_fail ? "有失败！" : "全部通过。", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
