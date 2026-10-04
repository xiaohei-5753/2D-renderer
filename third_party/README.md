# `third_party/` — 第三方依赖

本目录的依赖全部以**源码形式**内置，无需外部构建系统或包管理器。

## Dear ImGui `v1.92.9b`

- 来源：`https://codeload.github.com/ocornut/imgui/zip/refs/tags/v1.92.9b`
- 许可：MIT（见 `imgui/LICENSE.txt`）
- 用途：画板 UI（调色板 / 工具栏 / 图层面板 / 设置）
- 字体：ImGui 内置 `imstb_truetype.h`，可直接加载项目根目录的 `unifont-17.0.04.otf`
  （Unifont 是 16×16 点阵风格且覆盖中文，正好契合像素风画板）

内置的文件：

| 文件 | 说明 |
|---|---|
| `imgui/*.cpp/.h` | ImGui 核心（`imgui` / `imgui_draw` / `imgui_tables` / `imgui_widgets`） |
| `imgui/imstb_*.h` | 内置的 stb 单头文件（矩形打包 / 文本编辑 / TrueType 字体光栅化） |
| `imgui/imgui_demo.cpp` | 官方示例窗口。**仅 Debug 构建编译**，用于查阅控件用法 |
| `imgui/backends/imgui_impl_glfw.*` | GLFW 平台后端 |
| `imgui/backends/imgui_impl_opengl3.*` | OpenGL 3/4 渲染后端 |

## stb_image_write

- 来源：`https://cdn.jsdelivr.net/gh/nothings/stb/stb_image_write.h`
- 许可：public domain / MIT 双许可
- 用途：PNG 导出。画板的导出功能，以及离线渲染 CLI 的输出
- 单头文件，在 `core/image.cpp` 中定义 `STB_IMAGE_WRITE_IMPLEMENTATION`

> 注意：`raw.githubusercontent.com` 在本机被阻断，依赖改用
> `codeload.github.com`（ImGui）与 `cdn.jsdelivr.net`（stb）获取。
