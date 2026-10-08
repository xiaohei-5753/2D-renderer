# 交接文档

> 写给接手的人（也可能是几周后的自己）。
> 先看「跑起来 / 怎么验」，再看「重构计划」。

---

## 1. 项目是什么

像素风格画板。**画布（core/）与渲染库（gfx/）解耦**，游戏侧可以直接复用 `gfx/`。
光照分两套，一键切换：

| 模式 | 实现 | 特点 |
|---|---|---|
| **扫描 GI** | `gfx/light_sweep` | 方向表扫描，**实时**（728 方向 182~243 fps），无噪声 |
| **路径追踪** | `gfx/path_trace` | 逐像素光线追踪，渐进累积收敛 |

规模：55 个文件、约 8,800 行。

```
core/   2000 行 19 文件   纯 C++，零 OpenGL —— 可 headless 测试，游戏侧复用
gfx/    1998 行 18 文件   OpenGL 但 GLFW-agnostic —— 这就是游戏要复用的渲染库
ui/      376 行  4 文件   Dear ImGui 的薄封装
app/    2568 行  9 文件   画板应用（工具、输入、面板、光照编排）
tools/  1838 行  5 文件   headless 测试与离线出图
```

## 2. 跑起来 / 怎么验

```powershell
.\build.ps1                 # 全建（app + tools）
.\build.ps1 -Test           # 只建 tools（app 开着也能建，不占用 exe）
.\build\paintboard.exe      # 开画板
```

**⚠️ 画板开着时 `build.ps1` 会链接失败**（`Permission denied`）——先关窗口，
或者用 `-Test` 绕开（它只建 tools）。

### 回归（改完必跑）

```powershell
.\build\core_test.exe       # 285 个断言，必须"全部通过"
.\build\comp_test.exe       # 合成路径
```

### 离线出图（不用开窗口）

```powershell
.\build\render_cli.exe --scene neon --size 320x180 --out out\reg.png
.\build\paintboard.exe --demo --frames 40 --out out\layers.png
```

### 逐层光照的诊断回路（改了光照/合成之后先跑它）

```powershell
.\build\scan_dump.exe ...   # 把"扫描 / 逐层受光 / 最终结果"三阶段分别 dump 并报平均亮度
```

**这个工具救过命**：当初正是它报出"扫描 74.03、受光 73.35、最终 0.47"，
一眼定位到问题出在合成那一步，而不是扫描。

### 有窗口的自动化

```powershell
# 跑 N 帧、截图、可以点开/关某个面板分区
.\build\paintboard.exe --demo --frames 40 --out out\x.png --open 图层 --close 材质
```

---

## 3. 数据契约（改代码前先看这里）

### `core/layer.h` —— Document / Layer

* `Layer` = **可绘制表面**：`albedo`(RGBA8) + `material`(逐像素) + 脏矩形 + 绘制原语
* `Document` = **图层栈**，`layers()[0]` 是最底层
* `Document::resize()` 会清空一切、只留一个**空**层
  （纯白底纸是 **app 层**加的：启动时与「新建文档」时 `image().fill(白)`

  > ⚠️ 别把白底塞回 `Document::resize()`：那里会让 8 个 history 断言挂掉
  > （`core_test.cpp:282~296` 的 `commit/canUndo/undo/redo`）。

### `core/material.h` —— 逐像素材质

```cpp
struct Material {
    uint8_t emR, emG, emB;   // 自发光，独立 RGB（红表面可以发蓝光）
    uint8_t ior;             // 折射率（只在路径追踪下生效）
    uint8_t flags;           // kSolid 等
    uint8_t reserved;        // 目前没用
};
```

**遮挡 alpha 不在材质里**，它存在 albedo 图像的 alpha 通道
（0 = 空气，255 = 完全挡光，中间 = 半透明介质）。

### `core/palette.h` —— 色板项 = 7 维材质

```cpp
struct Mat7 {
    uint8_t r, g, b;         // 颜色
    uint8_t eR, eG, eB;      // 自发光（"亮度"，带颜色）
    uint8_t a;               // 透明度
};
const Mat7& mat(int i) const;   // 越界返回默认值，所以 mats_ 可以短于 colors_
void setMat(int i, const Mat7& m);
```

**核心约定：选色板项 = 同时选定这一笔的颜色、自发光、透明度。**
没有第二套并行的笔刷属性。反向也成立：改「材质」面板、用取色器（K），
都写回当前色板项。改这块时**别破坏这个约定**。

---

## 4. 光照模型（当前，务必先读懂再动）

### 4.1 逐层光场 + 乘法合成

```cpp
// gfx/layer_composite.cpp
L_i = 第 i 层的**空气像素**颜色 − 环境光    // 纯光，不含原始像素
F_i = Σ_{k>=i} L_k                          // 本层 + 上方所有层的光场
C_i = A_i × (F_i × strength + 环境光)       // 彩色光打在彩色表面上
acc = mix(acc, C_i, 覆盖率)                  // 覆盖 / 半覆盖
```

四个**不能忘**的点：

1. **是乘法，不是加法。** 只有环境光时白纸是**暗的**（`白 × 20/255`）。
   曾经有 3 次把"偏暗"误判成 bug —— 那是**正确行为**。
2. **光场取自空气像素**：那里 `alpha=0` 但 `rgb` 有值，正是这一层洒出的光晕。
   不需要另建纹理、不需要额外拷贝通道。（曾经为此折腾过一条 `glCopyImageSubData`
   的路，实测恒为空，白费。）
3. **环境光每层减掉、最后加一次** —— 等价于"每层 1/n"，但不会过曝。
4. **必须关掉旧的层间耦合**（`u_cross = 0`，见 `light_sweep.cpp`）：
   旧的 `emitFor` 会把"其他层透过来的光"塞进每层受光结果，
   于是**空图层也有值，层数一多画面越来越亮**。这是已修过的 bug。

面板「扫描 GI → **光场照亮下层**」= `strength`，默认 1；拉到 0 退回旧的混合路径。

### 4.2 曾经踩过的两个合成坑

* **整层不透明度会擦掉下层**：`acc = mix(acc, c.rgb, op)` 里 `op` 是整层标量，
  空的上层以 100% 盖住下面的内容（图层缩略图看得到、画面看不到）。
  必须用**逐像素覆盖率** `c.a`。
* **光晕要加法叠加**：空气像素的 rgb 应该 `+=` 上去，而不是覆盖掉下面的层。

### 4.3 已归档的体素路线

为了让上层给下层**投影**，试过把画布当 `X×Y×Z` 体素做真 3D 光追
（VCT 与 Radiance Cascades，CPU + GPU 都写了）。

**结论：放弃。** 换来的只是一处局部遮挡，整体光效却不如平面内光追。
实测与踩坑记录全在 **`archive/README.md`**，动手前值得一读。

---

## 5. 重构计划

按**价值 / 风险**排序。每条都是具体动作，不是方向。

### 5.1 低风险、马上能做

1. **删掉空的 `render/` 目录**
   体素实现已归档，目录空了。顺手确认 `build.ps1` 的源码 glob 里有没有它。

2. **README 里过时处**
   * 第 20 行 `composite.{h,cpp}` 仍写"层间光照通道（occlAbove / emitFor，模型 B 的核心）"
   * 第 425 行「参与光照的图层数上限 16」需要复核是否还成立
   * 「光照模型」整节已经改写过（现在是逐层光场），但**测试清单那一节**（379 行附近）
     还在提 `occlAbove / emitFor`

3. **`archive/` 里的 `dz25*` 与 `voxel/` 分两组**
   `archive/README.md` 已说明；如果嫌乱，可再分子目录，纯整理。

### 5.2 真正能砍代码的（推荐做）

4. **删掉旧的 `u_cross` 路径** —— 这是最大的一块冗余。

   现在 `strength` 默认 1，`u_cross` 恒为 0，也就是说：

   | 可以删掉 | 位置 |
   |---|---|
   | `occlAbove` / `emitFor` 的构建与上传 | `core/composite.{h,cpp}`、`gfx/light_sweep.{h,cpp}` |
   | 两个 `GL_TEXTURE_2D_ARRAY`（occl / emit） | `light_sweep.cpp` |
   | `u_cross` 分支（两个着色器各一处） | `light_sweep.cpp` 的 `kScanComp` / `kBlendComp` |
   | `buildOcclAbove` / `buildEmitFor` 的调用与脏区逻辑 | `app/lighting.cpp` |

   **预估能砍掉 300~400 行**，而且消掉一整类"空图层也会亮"的隐患。

   ⚠️ 删之前先确认：`strength` 是不是真的可以固定为 >0（现在面板能拉到 0）。
   如果还想保留"退回旧路径"的能力，就别删，改为**明确标注为 legacy**。

5. **`app/panels.cpp` 太大了**（约 1400 行，占 app 的一半）。
   里面是 6 个互不相干的 section，按函数边界拆成文件几乎零风险：

   ```
   panels_palette.cpp     drawPaletteSection
   panels_material.cpp    drawMaterialSection
   panels_lighting.cpp    drawLightingSection
   panels_layers.cpp      drawLayerSection
   panels_menu.cpp        drawMainMenuBar / drawStatusBar
   panels.cpp             只留 drawUi / drawToolbarPanel / drawPropertiesPanel
   ```

### 5.3 需要动脑的（想清楚再做）

6. **`tools/` 去重**：`comp_test` / `core_test` / `scan_dump` / `render_cli` / `png_stat`
   里应有一些共用的场景构造与 PNG 比较代码，可以抽一个 `tools/common.h`。

7. **`gfx/` 与 `core/` 的边界复核**：`gfx/layer_composite` 做的是"合成"，
   听起来像 `core` 的事；它留在 GPU 是因为逐层结果有 N 张，回读 CPU 太贵。
   这个理由需要写进注释，否则下一个人会想把它挪回 `core`。

### 5.4 不要做的事

* **别把白底放回 `Document::resize()`**（挂测试，见 §3）
* **别重新引入层间耦合**（`occlAbove` / `emitFor`）——它和逐层光场模型冲突
* **别把光场做成"加法叠加"** —— 必须是乘法（见 §4.1）
* **别在 PowerShell 里用 `Set-Content` / 字符串替换改仓库里的 C++ 源文件** ——
  中文会变乱码、换行会被吃掉、大括号会失配。用编辑工具，或 `[System.IO.File]::WriteAllText`。

---

## 6. 已知问题 / 遗留

| 项 | 说明 |
|---|---|
| 环境光累加 | **是特性**，使用者自己调「环境光」控制，不额外处理 |
| 图层数上限 16 | 每层一整个光照 pass。超出仍显示但不参与光照；图层列表**应当给出提示**（还没做） |
| 色板项没有名字/预览 | 7 维材质能用，但预设加载后 `mats_` 的初值策略比较朴素 |
| 体素路线的产物 | 全在 `archive/`，不参与编译 |

---

## 7. 这个项目教会我的（省你重复踩）

1. **先量再改。** 这一路最有效的动作都是"造一个能量出数字的场景"：
   等距圆环的不匀度、与反向光追参照物的差值、三阶段 dump 的平均亮度。
   凭肉眼看截图判断，我错过至少三次。
2. **参照物要先自证。** 用反向光追当基准时，它自己有个 bug（大块光源下
   `maxT=r-1` 会让射线撞进光源自身），害我一度以为是新实现错了。
3. **同名遮蔽**：局部变量遮住同名的 SSBO 数组，会报成"找不到函数重载"。犯过两次。
4. **测试场景本身也会骗人**：俯视下"正上方的遮挡物自己就是柱顶"。
   我当时拿着同一列去量遮挡，量到的"漏光"其实是板子顶面被正常照亮。
