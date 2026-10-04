# paintboard — 2D 像素风格画板（带光线追踪）

游戏开发子项目。由从 Paradox Node 挖出的 `magic-simulator` 模块改造而来：
**电路模拟部分已完全移除**，保留并重写了它的 GPU 光传输管线。

## 目标

一个像素风格的画板：画出像素，由 GPU 计算光照，得到实时泛光与可切换的逐像素光线追踪。
核心（文档/工具）与渲染库解耦，游戏侧可直接复用渲染库。

## 模块结构

```
core/    纯 C++，零 OpenGL / 零 GLFW —— 可 headless 测试，游戏侧复用
  types.h        RGBA8 / Rect / CoordI 等基础类型
  material.h     每像素光学属性：自发光 / 折射率 / 反射·实心标志
  image.{h,cpp}  RGBA8 图像缓冲 + PNG 导出（stb_image_write）
  canvas.{h,cpp} 画板文档：albedo + 材质 + 脏矩形 + 绘制原语（直线/矩形/椭圆/洪水填充）
  palette.{h,cpp} 调色板：内置标准色板 + 自定义色
  history.{h,cpp} 撤销/重做：脏矩形补丁栈
  tool.{h,cpp}   绘图工具：铅笔/直线/矩形/椭圆/油漆桶/抖动/橡皮/取色/加光/减光
  console.h      Windows 控制台 UTF-8 设置
gfx/     OpenGL，但 GLFW-agnostic —— 这就是游戏要复用的渲染库
  gl_common.h    统一 GLEW 包含（GLEW_STATIC 必须先行定义）
  gl_context.{h,cpp}  窗口/上下文（离线渲染用隐藏窗口）
  shader.{h,cpp}      着色器编译链接 + 错误上报
  gl_texture.{h,cpp}  纹理分配 + 脏矩形上传 + 回读
  light_sweep.{h,cpp} 方向扫描式全局光照（实时模式）
  viewport.{h,cpp}    相机、CROP 适配、显示、屏幕↔画布坐标换算
  hud.{h,cpp}         屏幕空间极简 HUD（纯色矩形：色板条、笔刷光标）
ui/      画板界面（P3 接入 Dear ImGui）
app/     GLFW 外壳：窗口 + 输入 + 主循环 —— 画板应用本体
  main.cpp       交互 + 演示/截图模式
tools/   每个 .cpp 编译成一个独立可执行文件
  render_cli.cpp 离线渲染器（出 PNG，渲染效果的验证回路 + 游戏侧素材烘焙）
  core_test.cpp  core 层的 headless 测试（190 项断言）
third_party/  Dear ImGui v1.92.9b + stb_image_write（源码内置，见其 README）
_attic/  改造前的原始代码与实验残骸，不参与编译（被 gitignore）
unifont-17.0.04.otf  UI 字体（P3 用 ImGui 加载中文）
```

**边界约束**：`core` 不 include GL，`gfx` 不 include GLFW（宿主提供上下文和尺寸）。
游戏侧只需 link `core` + `gfx`，自己驱动主循环。

## 构建

```powershell
.\build.ps1              # 构建全部（app + tools）
.\build.ps1 -Test        # 构建并运行 core 层测试
.\build.ps1 -App         # 只构建交互程序
.\build.ps1 -Cli         # 只构建 tools/
.\build.ps1 -Clean       # 清空重建
.\build.ps1 -Gxx <path>  # 指定 g++.exe
.\build.ps1 -Config debug
.\build.ps1 -Run         # 构建后启动 app
```

- 编译器：小熊猫 C++ 自带的 MinGW-w64 GCC 11.5.0
  （默认在 `D:\Program Files\RedPanda-Cpp\mingw64\bin\g++.exe`，可用 `-Gxx` 或 `REDPANDA_HOME` 覆盖）
- 产物：`build/paintboard.exe`、`build/render_cli.exe`、`build/core_test.exe`
- 依赖的 GLFW / GLEW 静态库位于 MinGW 默认搜索路径，无需额外 `-I` / `-L`

### ⚠️ 三个必须知道的坑（都踩过了）

1. **编译器的 bin 目录必须在 PATH 上。**
   `g++.exe` 只是个驱动，真正干活的是它从 `../libexec/gcc/...` 拉起的 `cc1plus.exe`，
   而 `cc1plus` 依赖同目录下的 MinGW 数学库 DLL（libmpc / libmpfr / libgmp / …）。
   若用绝对路径调用 `g++.exe` 而该目录不在 PATH，`cc1plus` 会在加载阶段就死掉
   （退出码 `0xC000007B`，STATUS_INVALID_IMAGE_FORMAT），而 g++ **只返回一个裸的
   exit code 1，不打印任何诊断信息**。`build.ps1` 会自动把该目录前置到 PATH。

2. **不要用 PowerShell 5.1 的 `Get-Content`/`Set-Content` 原地替换源码。**
   5.1 按 GBK 读取 UTF-8 文件，会破坏中文注释**并吞掉换行**（换行被并进 `//` 注释后，
   紧随其后的代码会静默消失）。本项目已经踩过一次，5 个文件被改坏。
   编辑源码请用支持 UTF-8 的工具。`build.ps1` 本身刻意只用 ASCII 也是同样的原因——
   PS 5.1 会把无 BOM 的脚本按 ANSI 解析。

3. **`GLEW_STATIC` 必须在包含 `<GL/glew.h>` 之前定义。**
   否则 glew.h 会把函数声明成 `dllimport`，生成 `__imp___glew*` 引用，
   静态链接时全部 undefined reference。统一走 `gfx/gl_common.h`。

## 交互操作

| 类别 | 按键 |
|---|---|
| 工具 | `P` 铅笔　`L` 直线　`R` 矩形　`E` 椭圆　`F` 油漆桶　`D` 抖动笔　`K` 取色　`X` 橡皮　`I` 加光　`O` 减光 |
| 笔刷 | `[` `]` 改大小（1..64）　`B` 方形/圆形切换　`G` 填充/描边切换 |
| 色板 | `1`-`9` `0` 选色　`,` `.` 切换色板　**也可以直接点画面底部的色条** |
| 视图 | 滚轮缩放（以光标为锚点）　中键 或 `空格`+左键 拖拽平移　`Ctrl+0` 复位 |
| 编辑 | 左键绘制　右键临时擦除（松手恢复原工具）　`Ctrl+Z` 撤销　`Ctrl+Y` / `Ctrl+Shift+Z` 重做 |
| 其它 | `Tab` 开关光照（关掉即显示干净原图）　`Ctrl+S` 存原始图　`Ctrl+Shift+S` 存受光图　`Ctrl+N` 清空　`ESC` 退出 |

窗口标题栏实时显示：工具 / 颜色 / alpha / 自发光 / 笔刷 / 色板 / 撤销重做步数 / 光照开关 / fps / 画布尺寸。

## 光照模型

`RGBA8` 的 **alpha 是"光路上的通透程度"**，这是整个模型的枢纽：

| alpha | 含义 | 渲染行为 |
|---|---|---|
| `0` | 空气 | 光直接穿过，显示环境光 + 沿途累积的光 |
| `0 < a < 1` | 半透明介质（霓虹管、玻璃） | 光穿过并按 `rgb*a*emit` 自发光，点亮周围 |
| `255` | 实心不透明（墙、实体） | 挡住光，画面直接显示自身 albedo |

所以构造一个"灯"就是画一根 `alpha≈110` 的亮色线条；构造"墙"就是 `alpha=255` 的色块。

**`Material` 通道**是对上面这个模型的扩展（数据模型已在 P1 落地，P2 的光追通道会真正读取它）：
自发光强度与遮挡程度是两个独立的量，这样才能表达原来表达不了的两种像素——
「不透明但发光」（自发光墙面/屏幕）和「半透明但不发光」（彩色玻璃）。

当前管线（`light_sweep`，移植自原 `renderScanline`）：

```
1. GPU 端清零 u32 定点累积 SSBO（省掉每帧 CPU->GPU 上传）
2. scan  : 一次 dispatch 覆盖所有方向（z 维 = 方向索引，曾是一方向一次共 728 次）
           每条射线直线步进，lt = lt*(1-a) + em，atomicAdd 写定点 SSBO
3. copy  : SSBO -> RGBA16F
4. blend : 与自发光合成，峰值归一，输出 RGBA8
```

## 离线渲染与自动化验证

**离线渲染器**（不开可见窗口，跑完光照直接出 PNG）：

```powershell
build\render_cli.exe --scene neon --size 320x180 --radius 128 --out out.png
```

| 参数 | 说明 |
|---|---|
| `--scene` | `neon` / `cornell` / `checker` / `empty`（默认 neon） |
| `--size` | 画布尺寸 `WxH`（默认 320x180） |
| `--radius` | 扫描方向表半径，越大光越平滑越慢（默认 128） |
| `--emit` / `--ambient` | 自发光倍率 / 环境光 `r,g,b` |
| `--out` | 输出 PNG |
| `--dump-canvas` | 额外导出未受光的画布原图，便于对照 |

**画板的演示 / 截图模式**（自动画一张演示图 → 渲染 N 帧 → 截下整个帧缓冲含 HUD）：

```powershell
build\paintboard.exe --demo --frames 4 --out out\p1_demo.png
```

这条路径是这个项目的**视觉回归工具**：不需要人操作就能看出渲染、HUD、绘图原语是否正常。
`--demo`/`--frames` 默认不开可见窗口，不会干扰使用者。

**core 层测试**（190 项断言，不需要 GPU）：

```powershell
build\core_test.exe      # 或 .\build.ps1 -Test
```

覆盖：直线（含端点规范化后的方向无关性、越界裁剪）、矩形、椭圆（实心面积、
四重对称、极扁椭圆的完整性）、洪水填充（含同色与越界）、撤销重做（逐像素+材质精确还原、
无变化不污染栈、补丁内存占用、多步与分支失效、步数裁剪、越带改动后重做链作废）、
工具（铅笔连续性、笔刷大小与形状、形状预览回滚、橡皮/取色、抖动覆盖率与确定性、
加光减光只作用于已有像素、油漆桶）、调色板（预设、夹取、循环、自定义色）、
材质通道、脏矩形、尺寸变更。

### 为什么要有这一层测试

交互式画板有大量**看不到的编辑语义**（撤销要精确还原到什么程度、形状预览会不会残留、
快速拖动的笔迹会不会断）。界面我看不到，但这些语义必须能被自动化证明，
否则每次改动都只能靠肉眼猜。`core` 刻意不依赖 GL，就是为了让这件事成为可能。

## 调色板

内置 6 套，色值来自权威来源（PICO-8 与 DB16 已逐字节核对：

- **PICO-8**（16 色）：Lexaloffle 官方系统调色板，标准索引序 0..15
- **DB16**（16 色）：DawnBringer 2011 年原始发布值
- **Game Boy**（4 色）：社区最广为人知的"经典四色"（Aseprite 等采用）。
  注意 DMG 是反射式 STN 液晶，物理上没有固定 RGB 输出，所有数值都是近似；
  另一个有据可查的版本是 BGB 模拟器默认值（`081820/346856/88C070/E0F8D0`），
  换用它只需改 `core/palette.cpp` 里的四行。
- **NES**（55 色）：Lospec / NES.css 一致的常用近似。**不存在硬件权威的十六进制值**
  （NES 输出 NTSC 复合视频，RGB 由电视机产生），各模拟器与实测版本每一格都可能不同。
  常说的"54 色"来自另一个变体（该变体 `$37=FBDFA6`、`$3D=D8D8D8`）。
- **灰阶 8**、**黑白**

自由 RGB：`Palette::addColor()` 会把自定义色追加到当前色板末尾（P3 的拾色器界面会用它）。

## 路线图

- [x] **P0** 清理与地基：git 版本控制、`build.ps1`、模块骨架、离线渲染 CLI 验证回路
- [x] **P1** `core`：动态 Canvas + 材质通道 + 调色板 + 全套绘图工具 + 撤销重做 + 相机缩放 + HUD
- [ ] **P2** `gfx`：光照与 albedo 解耦，材质纹理；扫描 GI 与**渐进式逐像素 2D 光追**双模式一键切换
- [ ] **P3** Dear ImGui 界面：调色板 / 工具栏 / 图层面板（用 `unifont` 加载中文）
- [ ] **P4** 图层、选区变换、导出打磨

### P2 要解决的核心问题

现在的扫描 GI 有两个已知局限，都在 P0/P1 的渲染结果里能看到：

1. **方向性条纹**：光是沿扫描方向累积的，单个亮像素会拖出可见的直线光带，
   而不是各向同性的光晕。
2. **无法反射/折射**：只有"穿过"和"挡住"两种交互，没有镜面反射、折射和软阴影。

P2 新增一条逐像素 ray march 通道（真·2D 光追），复用时间累积做渐进收敛，
与实时扫描 GI 并存、一键切换。同时把光照结果与 albedo 真正解耦——目前 `blend` 是把光
**叠死**在输出里的，`Tab` 只是切换显示哪张纹理；P2 要让光照成为可独立开关、可导出的图层，
并让 `Material` 的 emission / ior / 反射标志真正参与计算。

### 已知待办

- 画布最大尺寸目前仍受 `LightSweep` 一次性分配的限制（无运行时改尺寸）
- `app/main.cpp` 是 P1 的过渡外壳，P3 接 ImGui 后会重写为 UI 驱动
- 尚未做图层的节点管理、选区变换、工程文件（`.pxc`）读写
