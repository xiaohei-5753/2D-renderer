# Easy Renderer — 2D GPU Ray-Traced Renderer / 二维 GPU 光线追踪渲染器

A lightweight, GPU-accelerated 2D ray tracer built with OpenGL compute shaders.
一个基于 OpenGL Compute Shader 的轻量级 GPU 二维光线追踪渲染器。

---

## Features / 特性

- **Scanline Light Propagation** — GPU forward propagation along parallel Bresenham lines, ~100× fewer texelFetch than per-pixel ray tracing
  **扫描线光线传播** — 沿平行 Bresenham 线在 GPU 上正向传播光，texelFetch 比逐像素光线追踪少 ~100×
- **Bresenham Direction Families** — full-circle direction families (configurable via circleRadius), each processed as independent parallel scanlines
  **Bresenham 方向族** — 全圆方向族（通过 circleRadius 配置），每个方向族作为独立平行扫描线处理
- **Occupancy Skip** — empty region stride (×4) via real-time occupancy texture
  **占空跳过** — 通过实时占空纹理实现空区域 4 像素步进跳过
- **Semi-Transparent Materials** — alpha-based transmission with energy conservation
  **半透明材质** — 基于 Alpha 的光线透射，能量守恒
- **Directional Sun + Ambient Wall Light** — configurable
  **方向光（阳光）+ 环境光（墙壁）** — 可配置
- **Pixel Self-Emission** — each pixel emits colored light independently
  **像素自发光** — 每个像素独立发射彩色光
- **API-driven Configuration** — library exposes setter API; `cfg.txt` parsing lives in app layer
  **基于 API 的配置** — 库暴露 setter 接口；`cfg.txt` 解析在应用层
- **Interactive Paint Demo** — Brush / Line / Rect / Eraser, 8 colors
  **交互式画板** — 画笔/直线/矩形/橡皮擦，8 种颜色

---

## Quick Start / 快速开始

### Dependencies / 依赖

- OpenGL 4.3+ (compute shader)
- GLFW 3.x
- GLEW
- MinGW-w64

### Build / 构建

```bash
build.bat
```

### Run / 运行

```bash
paint_app.exe
```

### Controls / 操作

| Key | Action / 功能 |
|-----|--------------|
| Left Mouse / 左键 | Draw / 画 |
| Right Mouse / 右键 | Erase / 擦除 |
| `1`–`8` | Select color / 选色 |
| `B` / `L` / `R` / `E` | Brush / Line / Rect / Eraser |
| `C` | Clear canvas / 清空 |
| `Ctrl+S` | Screenshot / 截图 |
| `Ctrl+N` | Reset camera / 重置相机 |
| Arrow / 方向键 | Pan / 平移 |
| `+` / `-` | Zoom / 缩放 |

---

## Configuration / 配置

Configuration is **app-layer**: `paint_app.cpp` parses `cfg.txt` and applies values via the library API.
配置属于 **应用层**：`paint_app.cpp` 解析 `cfg.txt`，通过库 API 设置参数。

```
# Canvas / 画布
512 512 64 1280 1280   # width height circleRadius windowW windowH

# Wire alpha / 线条透明度  
0.5

# Colors / 颜色 (RGB 0–255)
255 255 255 255          # text / 文字 (RGBA)
255 255 255              # sun / 阳光 (×10 intensity)
64 64 64                 # wall / 环境光 (×0.5 intensity)

# Reserved / 预留
100 2 50 8

# Show FPS / 显示帧率
1
```

**`circleRadius`** — sampling circle radius / 采样圆半径
- Larger → more rays → higher quality, slower / 大→光线多→质量高、慢
- Default: 64 (≈500 rays/px)
- Low-end: try 16–32

---

## Algorithm Details / 算法详解

### 1. Bresenham Circle Sampling / 布雷森汉姆圆采样

```
For pixel P with radius R:
  m = 1 - R, x = 0, y = R
  while (x < y):
    cast ray from P in direction (x, y)       → cR()
    cast ray from P in direction (-x, y)      → cR()
    ... (8 symmetric directions per step)
    x += 1
    update m (midpoint circle decision)
```

Each (x, y) pair generates **8 symmetric directions** (the 8 octants of a circle). For radius R, the total ray count per pixel is ≈ 8R.

每个 `(x, y)` 对生成 **8 个对称方向**（圆的八个卦限）。半径 R 时，每像素约 8R 条光线。

### 2. Bresenham Ray Marching / 布雷森汉姆光线步进

```glsl
vec3 cR(ivec2 origin, ivec2 dir) {
    light = 0, transmittance = 1
    x, y = origin
    e2 = 2 * (|dx| - |dy|)
    
    // First Bresenham step
    if (e2 > -|dy|) adjust error, advance x
    if (e2 < |dx|)  adjust error, advance y
    
    while (true):
        if out of bounds:
            add sun or wall light (weighted by transmittance)
            return light
        
        if region is EMPTY (occupancy check):
            advance 4 pixels at once (continue loop)
        
        sample pixel color (c) and emission (ref)
        
        if c.a ≥ 1 (opaque):
            light += transmittance × ref
            return light
        
        if c.a > 0 (semi-transparent):
            light += transmittance × ref × c.a
            transmittance ×= (1 - c.a)
            if transmittance < epsilon: return light
        
        advance 1 Bresenham step
}
```

### 3. Occupancy Optimization / 占空优化

```
Occupancy texture (R8, per frame):
  alpha > 0.01 → occ = 255 (has content)
  alpha ≤ 0.01 → occ = 0   (empty)

In shader:
  if (texelFetch(u_occ, pos).r < 0.5):
    → skip 4 pixels at once using Bresenham loop
```

The occupancy texture is rebuilt every frame in `uploadCanvasTexture()`, based on pixel alpha. This is independent of the CPU-side quadtree (which has been removed).

占空纹理每帧在 `uploadCanvasTexture()` 中根据像素 Alpha 重建，与 CPU 端已移除的四叉树无关。

### 4. Scanline Light Propagation / 扫描线光线传播

```
For each direction (dx, dy) from Bresenham circle:
  // Parallel scanlines, 1 pixel apart, covering entire canvas
  For each entry point on canvas boundary (W + H rays):
    light = ambient
    March along Bresenham line through canvas:
      light = light × (1 - α_pixel) + emission_pixel
      scanTex[pixel] += light × (1 / nDirs)    // accumulate
```

每个方向族一次 compute dispatch，完全并行。所有方向族累积到 float16 扫描纹理，最终 blend 到输出。
Each direction family = one compute dispatch, fully parallel. All families accumulate to a float16 scan texture, then blended to output.

**Complexity / 复杂度**:
- Per-pixel ray tracing (old): O(pixels × rays × steps) ≈ **15B** texelFetch
- Scanline (new): O(directions × (W+H) × steps) ≈ **80M** texelFetch
- **~200× fewer** texture reads

### 5. Lighting Model / 光照模型

```
For each ray direction:
  light_accumulated += Σ(transmittance × pixel_emission × alpha)

At canvas boundary:
  if ray_dir aligns with sun: light += transmittance × sun_color
  else:                       light += transmittance × wall_color

After all rays:
  averaged_light = Σ(light_per_ray) / ray_count
  self_emission  = texelFetch(u_cl, pixel_pos).rgb  (not scaled by alpha)
  final_color    = mix(averaged_light + self_emission, pixel_color, alpha)

Output: imageStore(u_out, pixel_pos, vec4(final_color, 1.0))
```

**Key fix**: self-emission is NOT multiplied by `(1-alpha)` to avoid double-scaling through the subsequent `mix()`.

**关键修复**：自发光不乘以 `(1-alpha)`，避免通过后续 `mix()` 产生双重缩放。

### 5. Two-Layer Architecture / 双层架构

```
┌──────────────────────────────────────────────────┐
│  paint_app.cpp (Application Layer / 应用层)       │
│  - Mouse state / 鼠标状态                         │
│  - Drawing: drawLine, drawRect, scr2cvs           │
│  - GDI TextOverlay / GDI 文字叠加                  │
│  - Screenshot / 截图                              │
│  - Camera control / 相机控制                       │
│  - FPS / 帧率                                     │
├──────────────────────────────────────────────────┤
│  easy_renderer (Library Layer / 库层)             │
│  - Canvas: pixel buffer, dirty tracking            │
│  - Renderer: GL init, shaders, textures            │
│  - uploadCanvasTexture(): CPU→GPU data transfer    │
│  - renderRayTrace(): compute shader dispatch       │
│  - renderDisplay(): camera transform → screen      │
│  - Config API: setCircleRadius / setSunColor / …   │
│  - Input hooks: onMouse*, onKey                    │
│  - Core structs: Pixel, Camera                    │
└──────────────────────────────────────────────────┘
```

The library (`easy_renderer`) has zero GDI dependency and is theoretically portable. The application layer (`paint_app.cpp`) handles all Win32/GDI-specific functionality.

库层零 GDI 依赖，理论上可跨平台。应用层处理所有 Win32/GDI 特有功能。

---

## Pipeline / 渲染管线

```
User Draws / 用户绘制
  ↓
uploadCanvasTexture()  → upload color + light + occupancy → GPU textures
  ↓
renderScanline()       → dispatch scanline propagation per direction family
  ↓  For each direction:
     ├─ Clear accumulation texture / 清空累积纹理
     ├─ Dispatch W+H parallel rays / 分发 W+H 条平行射线
     │  └─ Each ray: march Bresenham, carry ambient, absorb+emit, accumulate
     │    每条射线：Bresenham 步进，携带环境光，吸收+发光，累积
     └─ Memory barrier / 内存屏障
  ↓
blend shader           → mix scan light + color + emission → renderTex
  ↓
renderDisplay()        → camera transform → screen / 相机变换→屏幕
```

---

## Performance / 性能

| Scenario / 场景 | FPS (circleRadius=64) |
|----------------|----------------------|
| Empty canvas / 空白 | 5000+ |
| Sparse / 稀疏内容 | 200–500 |
| Dense / 密集内容 | 30–60 |

---

## File Structure / 文件结构

```
E:\programming\CPP\2D-renderer\
├── easy_renderer.h       # Library header / 库头文件
├── easy_renderer.cpp     # Library impl / 库实现
├── paint_app.cpp         # Application / 应用
├── build.bat             # Build script / 构建脚本
├── cfg.txt               # Config / 配置
├── API.md                # API reference / API 手册
├── .gitignore
├── README.md
├── include/              # GLFW + GLEW
└── lib/                  # Libraries / 库文件
```

## API Reference / API 参考

See [`API.md`](API.md) for the complete library API documentation (bilingual CN/EN).
完整的库 API 文档见 [`API.md`](API.md)（中英双语）。

## Technical Notes / 技术备忘

| Item | Detail |
|------|--------|
| Shader version / 着色器版本 | `#version 430 core` |
| Workgroup / 工作组 | 8×8 |
| Output format / 输出格式 | RGBA8 image |
| Blending / 混合 | None (shader writes final result) |
| Camera / 相机 | Orthographic, pan + zoom |
| VSync / 垂直同步 | Disabled (`glfwSwapInterval(0)`) |
| Text overlay / 文字 | GDI → GL texture → fragment shader (in app) |
| Build / 构建 | MinGW-w64, `-O3 -static` |

---

## License

GPL v3
