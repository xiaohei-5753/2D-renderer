# paintboard — 2D 像素风格画板（带光线追踪）

游戏开发子项目。由从 Paradox Node 挖出的 `magic-simulator` 模块改造而来：
**电路模拟部分已完全移除**，保留并重写了它的 GPU 光传输管线。

## 目标

一个像素风格的画板：画出像素，由 GPU 计算光照，得到实时泛光与可切换的逐像素光线追踪。
核心（文档/工具）与渲染库解耦，游戏侧可直接复用渲染库。

## 模块结构

```
core/    纯 C++，零 OpenGL / 零 GLFW —— 可 headless 测试，游戏侧复用
  types.h        RGBA8 / Rect / 基础类型
  image.{h,cpp}  RGBA8 图像缓冲 + PNG 导出（stb_image_write）
  canvas.{h,cpp} 画板文档：像素 + 脏矩形 + 绘制原语
  console.h      Windows 控制台 UTF-8 设置
gfx/     OpenGL，但 GLFW-agnostic —— 这就是游戏要复用的渲染库
  gl_common.h    统一 GLEW 包含（GLEW_STATIC 必须先行定义）
  gl_context.{h,cpp}  窗口/上下文（离线渲染用隐藏窗口）
  shader.{h,cpp}      着色器编译链接 + 错误上报
  gl_texture.{h,cpp}  纹理分配 + 脏矩形上传 + 回读
  light_sweep.{h,cpp} 方向扫描式全局光照（实时模式）
  viewport.{h,cpp}    相机、CROP 适配、显示、屏幕↔画布坐标
ui/      画板界面（P3 接入 Dear ImGui）
app/     GLFW 外壳：窗口 + 输入 + 主循环 —— 画板应用本体
tools/   离线渲染 CLI（同时是渲染效果的验证回路与素材烘焙工具）
third_party/  Dear ImGui v1.92.9b + stb_image_write（源码内置，见其 README）
_attic/  改造前的原始代码与实验残骸，不参与编译（被 gitignore）
unifont-17.0.04.otf  UI 字体（ImGui 会用它加载中文）
```

**边界约束**：`core` 不 include GL，`gfx` 不 include GLFW（宿主提供上下文和尺寸）。
游戏侧只需 link `core` + `gfx`，自己驱动主循环。

## 构建

```powershell
.\build.ps1              # 构建全部（app + 离线渲染器）
.\build.ps1 -Cli         # 只构建离线渲染器
.\build.ps1 -App         # 只构建交互程序
.\build.ps1 -Clean       # 清空重建
.\build.ps1 -Gxx <path>  # 指定 g++.exe
.\build.ps1 -Config debug
.\build.ps1 -Run         # 构建后启动 app
```

- 编译器：小熊猫 C++ 自带的 MinGW-w64 GCC 11.5.0
  （默认在 `D:\Program Files\RedPanda-Cpp\mingw64\bin\g++.exe`，可用 `-Gxx` 或 `REDPANDA_HOME` 覆盖）
- 产物：`build/paintboard.exe`、`build/paintboard_render.exe`
- 依赖的 GLFW / GLEW 静态库位于 MinGW 默认搜索路径，无需额外 `-I` / `-L`

### ⚠️ 两个必须知道的构建陷阱

1. **编译器的 bin 目录必须在 PATH 上。**
   `g++.exe` 只是个驱动，真正干活的是它从 `../libexec/gcc/...` 拉起的 `cc1plus.exe`，
   而 `cc1plus` 依赖同目录下的 MinGW 数学库 DLL（libmpc / libmpfr / libgmp / …）。
   若用绝对路径调用 `g++.exe` 而该目录不在 PATH，`cc1plus` 会在加载阶段就死掉
   （退出码 `0xC000007B`，STATUS_INVALID_IMAGE_FORMAT），而 g++ **只返回一个裸的
   exit code 1，不打印任何诊断信息**。`build.ps1` 会自动把该目录前置到 PATH。

2. **`build.ps1` 刻意只用 ASCII。**
   Windows PowerShell 5.1 会把无 BOM 的脚本按 ANSI(GBK) 解析，脚本内的中文会被破坏。

3. 同理：**不要用 PowerShell 的 `Get-Content`/`Set-Content` 原地替换源码**。
   5.1 版按 GBK 读取 UTF-8 文件，会破坏中文注释并吞掉换行（本项目已经踩过一次）。
   编辑源码请用支持 UTF-8 的工具。

## 离线渲染器

不开可见窗口，跑完光照直接出 PNG。这是渲染效果的验证回路（无需人眼盯屏幕），
也是游戏侧的素材烘焙工具。

```powershell
build\paintboard_render.exe --scene neon --size 320x180 --radius 128 --out out.png
```

| 参数 | 说明 |
|---|---|
| `--scene` | `neon` / `cornell` / `checker` / `empty`（默认 neon） |
| `--size` | 画布尺寸 `WxH`（默认 320x180） |
| `--radius` | 扫描方向表半径，越大光越平滑越慢（默认 128） |
| `--emit` | 自发光倍率 |
| `--ambient` | 环境光 `r,g,b`（0..1） |
| `--out` | 输出 PNG |
| `--dump-canvas` | 额外导出未受光的画布原图，便于对照 |

实测（RTX 4080 Laptop，320x180，radius 128）：`728 个方向，4.6 ms`。

## 光照模型

`RGBA8` 的 **alpha 同时是"光路上的通透程度"**，这是整个模型的枢纽：

| alpha | 含义 | 渲染行为 |
|---|---|---|
| `0` | 空气 | 光直接穿过，显示环境光 + 沿途累积的光 |
| `0 < a < 1` | 半透明介质（霓虹管、玻璃） | 光穿过并按 `rgb*a*emit` 自发光，点亮周围 |
| `255` | 实心不透明（墙、实体） | 挡住光，画面直接显示自身 albedo |

所以构造一个"灯"就是画一根 `alpha≈90` 的亮色线条；构造"墙"就是 `alpha=255` 的色块。

当前管线（`light_sweep`，移植自原 `renderScanline`）：

```
1. GPU 端清零 u32 定点累积 SSBO（省掉每帧 CPU->GPU 上传）
2. scan  : 一次 dispatch 覆盖所有方向（z 维 = 方向索引，曾是一方向一次共 728 次）
           每条射线直线步进，lt = lt*(1-a) + em，atomicAdd 写定点 SSBO
3. copy  : SSBO -> RGBA16F
4. blend : 与自发光合成，峰值归一，输出 RGBA8
```

## 路线图

- [x] **P0** 清理与地基：git 版本控制、`build.ps1`、模块骨架、离线渲染 CLI 验证回路
- [ ] **P1** `core`：动态 Canvas + 材质通道 + 调色板 + 全套绘图工具 + 撤销重做 + 相机缩放
- [ ] **P2** `gfx`：光照与 albedo 解耦，材质纹理；扫描 GI 与**渐进式逐像素 2D 光追**双模式一键切换
- [ ] **P3** Dear ImGui 界面：调色板 / 工具栏 / 图层面板（用 `unifont` 加载中文）
- [ ] **P4** 图层、选区变换、导出打磨

### P2 要解决的核心问题

现在的扫描 GI 有两个已知局限，都在上面 `neon` 场景的渲染结果里能看到：

1. **方向性条纹**：光是沿扫描方向累积的，单个亮像素会拖出可见的直线光带，
   而不是各向同性的光晕。
2. **无法反射/折射**：只有"穿过"和"挡住"两种交互，没有镜面反射、折射和软阴影。

P2 的做法是新增一条逐像素 ray march 通道（真·2D 光追），复用时间累积做渐进收敛，
与实时扫描 GI 并存、一键切换。同时把光照结果与 albedo 解耦——目前 `blend` 是把光
**叠死**在输出里的，画板需要能关闭光照看到干净的原图（也就无法取色和正确导出）。
