// app/light_settings_io.h — 光照参数的持久化
//
// ## 为什么需要它
//
// 画板原来只靠 ImGui 的 .ini 存**界面折叠状态**，光照参数（半径、扩散、
// 环境光…）重启就丢。方向扩散是个需要反复手调的参数，每次开程序都要重调
// 一遍是不可接受的。
//
// ## 为什么不塞进 ImGui 的 .ini
//
// 要做到那点得用 `ImGui::AddSettingsHandler`，而它声明在 **imgui_internal.h**
// —— 用了就等于依赖 ImGui 内部 API，升级版本时容易断。为一个滑块付这个代价
// 不值得，所以这里用一个独立的小配置文件。
//
// ## 文件位置与格式
//
// 位置：`build\paintboard_light.ini`（**跟 exe 同目录**，和字体一个道理 ——
// 整个 build\ 拷走设置也跟着走）。格式是极简的 `键=值`，一行一个：
//
//     rayRadius=32
//     diffusionOn=1
//     diffusion=0.100
//     diffusionStep=8
//
// 解析故意做得**很宽容**：认不出的键忽略、缺的键保留默认值、文件不存在就当
// 全默认。这样以后加新参数不会让旧配置文件失效。
#pragma once

namespace pb {
namespace gfx {
struct LightSettings;
}

namespace app {

// 从 exe 同目录的 paintboard_light.ini 读取光照参数。
// 文件不存在 / 读不动 = 保持当前值（即默认值），不算错误。
void loadLightSettings(gfx::LightSettings& s);

// 写回同一个文件。用于"改了就存"与退出时保存。
void saveLightSettings(const gfx::LightSettings& s);

}  // namespace app
}  // namespace pb
