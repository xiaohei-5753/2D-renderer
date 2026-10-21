# 架构（模块边界与库）

## 分层

```
core/     数据模型：Image / Layer / Document / Material / Project / History
          —— 纯 CPU，不碰 OpenGL，可单独测试
   |
   v
gfx/      渲染器：GL 上下文、纹理、着色器、扫描 GI、去噪、图层合成
          —— 只依赖 core/，不知道"画板"的存在
   |
   +--> scenes/   示例内容（bench 场景：按 Document 直接作图，确定性动画）
   |
   +--> ui/       ImGui 控件（字体、主题、小组件）
                     |
                     v
                  app/     画板前端（面板、输入、工程、导出）
```

**硬规则：`core/` 与 `gfx/` 不得包含 `app/`、`ui/` 的任何头文件。**
这条规则不靠自觉，靠库边界保证：`libpb_render.a` 只由 `core/` + `gfx/` 编出，
一旦渲染器里出现对前端的反向依赖，链接期就会缺符号。

## 三个静态库

| 库 | 来源 | 谁链接 |
|---|---|---|
| `libpb_render.a` | `core/` + `gfx/` | 所有人（渲染器本体） |
| `libpb_scenes.a` | `scenes/` | paintboard、bench_view、hash_accum |
| `libpb_ui.a` | `ui/` + 第三方 Dear ImGui | paintboard、bench_view 等带界面的工具 |

链接顺序：**前端对象 → ui → scenes → render**（被依赖的库放后面）。

## 公开入口

`gfx/renderer.h` 是渲染器库的门面（umbrella header）：包含它就能拿到
`Document/Layer/Material` 与 `LightSweep/Denoise/LayerComposite/Viewport`，
文件头里有最小用法示例。

## 画质档位（渲染器可调项）

`LightSweep::FieldQuality`：

- `Exact`（默认）：光场按画布全分辨率计算 = 原始路径，逐次渲染位级一致；
- `Coarse2 / Coarse3 / Coarse4`：只把**光场**降到 1/s 分辨率，合成时双线性放大；
  albedo / 自发光 / 最终合成仍是全分辨率，所以 1px 霓虹不会被糊掉。
  实测（4080 Laptop，5 层 R=64）：4.08 ms → 1.63 ms（2×）/ 0.53 ms（4×），
  平滑内容与精确档最大差 7 级；薄墙密集的内容会偏暗（已知原理性损失）。
- `Adaptive`：粗趟 + 细趟（未收敛，选中会回退到 `Coarse4` 并打印警告）。

## 目录约定

- `scenes/` 里只放**内容**（按 Document 画什么），不放交互/UI；
- `tools/` 每个 `.cpp` 是一个独立可执行文件，与 `app/` 平级，都是库的消费者；
- 新增第三方依赖放 `third_party/<name>/`，并在 `build.ps1` 里用 `-isystem` 引入。
