// app/light_settings_io.cpp
#include "app/light_settings_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/paths.h"
#include "gfx/light_sweep.h"

namespace pb {
namespace app {

namespace {

const char* kFileName = "paintboard_light.ini";

// 优先放 exe 同目录；拿不到 exe 目录（极罕见）就退回当前目录。
// 不写进 core/paths 的 findAsset —— 那个是"只读查找"，这里是"要写"，
// 语义不同（写的时候不能去上一级乱写）。
std::string configPath() {
    const std::string dir = executableDir();
    return (dir.empty() ? std::string() : dir) + kFileName;
}

// 去掉行尾的 \r\n 与首尾空白（Windows 上文件可能是 CRLF）
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

bool parseBool(const std::string& v) {
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

}  // namespace

void loadLightSettings(gfx::LightSettings& s) {
    const std::string path = configPath();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;   // 没有配置文件 = 用默认值，这是正常情况（首次运行）

    char line[512];
    int applied = 0;
    while (std::fgets(line, sizeof(line), f)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;

        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        const std::string val = trim(t.substr(eq + 1));

        // 认不出的键直接忽略 —— 这样旧配置文件不会因为新版本加了参数而失效
        if (key == "rayRadius") {
            const int v = std::atoi(val.c_str());
            if (v >= 4 && v <= 512) { s.rayRadius = v; applied++; }
        } else if (key == "emitScale") {
            s.emitScale = (float)std::atof(val.c_str()); applied++;
        } else if (key == "propagate") {
            s.propagate = (float)std::atof(val.c_str()); applied++;
        } else if (key == "ambient0") {
            s.ambient[0] = (float)std::atof(val.c_str()); applied++;
        } else if (key == "ambient1") {
            s.ambient[1] = (float)std::atof(val.c_str()); applied++;
        } else if (key == "ambient2") {
            s.ambient[2] = (float)std::atof(val.c_str()); applied++;
        } else if (key == "diffusionOn") {
            s.diffusionOn = parseBool(val); applied++;
        } else if (key == "diffusion") {
            const float v = (float)std::atof(val.c_str());
            if (v >= 0.0f && v <= 1.0f) { s.diffusion = v; applied++; }
        } else if (key == "diffusionStep") {
            const int v = std::atoi(val.c_str());
            if (v >= 1 && v <= 64) { s.diffusionStep = v; applied++; }
        }
    }
    std::fclose(f);

    if (applied > 0) {
        std::printf("[cfg] 光照参数已从 %s 恢复（%d 项）\n", kFileName, applied);
        std::printf("[cfg]   R=%d  扩散 %s (w=%.3f K=%d)\n", s.rayRadius,
                    s.diffusionOn ? "开" : "关", (double)s.diffusion, s.diffusionStep);
    }
}

void saveLightSettings(const gfx::LightSettings& s) {
    const std::string path = configPath();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;   // 写不进去就算了（只读目录等），不该让画板崩

    std::fprintf(f, "# paintboard 光照参数（自动生成，可手改）\n");
    std::fprintf(f, "# 方向扩散：w = 相邻射线的交换比例，diffusionStep = 每多少步交换一次\n");
    std::fprintf(f, "# 实测甜点 w=0.100 / K=8；w 过大会把细线光源抹糊\n");
    std::fprintf(f, "rayRadius=%d\n", s.rayRadius);
    std::fprintf(f, "emitScale=%.4f\n", (double)s.emitScale);
    std::fprintf(f, "propagate=%.4f\n", (double)s.propagate);
    std::fprintf(f, "ambient0=%.4f\n", (double)s.ambient[0]);
    std::fprintf(f, "ambient1=%.4f\n", (double)s.ambient[1]);
    std::fprintf(f, "ambient2=%.4f\n", (double)s.ambient[2]);
    std::fprintf(f, "diffusionOn=%d\n", s.diffusionOn ? 1 : 0);
    std::fprintf(f, "diffusion=%.4f\n", (double)s.diffusion);
    std::fprintf(f, "diffusionStep=%d\n", s.diffusionStep);
    std::fclose(f);
}

}  // namespace app
}  // namespace pb
