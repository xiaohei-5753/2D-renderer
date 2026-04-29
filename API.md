# Easy Renderer API Reference / API 参考手册

Bilingual reference for the `easy_renderer` library (`easy_renderer.h` / `easy_renderer.cpp`).
`easy_renderer` 库的中英双语 API 参考。

---

## Namespace / 命名空间

All types and classes are in `namespace easy_renderer`.
所有类型和类均在 `namespace easy_renderer` 中。

---

## `struct Pixel`

```cpp
struct Pixel {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;  // color / 颜色
    float lr = 0.0f, lg = 0.0f, lb = 0.0f;            // self-emission / 自发光
};
```

A single pixel on the canvas. Color channels are in `[0, 1]`. Alpha `a` controls opacity (0 = transparent, 1 = opaque). Light channels `lr/lg/lb` are the pixel's self-emission color.
画布上的单个像素。颜色通道在 `[0, 1]` 范围内。Alpha `a` 控制不透明度（0 = 透明，1 = 不透明）。光通道 `lr/lg/lb` 表示像素的自发光颜色。

---

## `struct Camera`

```cpp
struct Camera {
    double x = 0.5, y = 0.5, scale = 1.0;
};
```

Orthographic camera. `x, y` is the center in normalized canvas coordinates `[0, 1]`. `scale` controls zoom (1.0 = fit canvas to window).
正交相机。`x, y` 是归一化画布坐标 `[0, 1]` 下的中心点。`scale` 控制缩放（1.0 = 画布适配窗口）。

---

## `class Canvas`

Pixel buffer with dirty tracking. 带脏标记追踪的像素缓冲区。

| Method / 方法 | Description / 说明 |
|---|---|
| `Canvas(int w, int h)` | Create canvas of `w × h` pixels / 创建 `w × h` 像素的画布 |
| `~Canvas()` | Destructor / 析构函数 |
| `void setPixel(int x, int y, const Pixel& p)` | Set pixel at `(x, y)` / 设置 `(x, y)` 处的像素 |
| `void clear(float r=0, float g=0, float b=0, float a=0)` | Fill entire canvas with color / 用颜色填充整个画布 |
| `const Pixel* data() const` | Raw pixel array (`width × height`) / 原始像素数组 |
| `int width() const` | Canvas width / 画布宽度 |
| `int height() const` | Canvas height / 画布高度 |
| `bool isDirty() const` | Has content changed since last `markClean()`? / 自上次 `markClean()` 后内容是否改变？ |
| `void markClean()` | Reset dirty flag / 重置脏标记 |

**Usage note / 使用说明**: `setPixel()` sets `dirty_ = true` automatically. The Renderer checks `isDirty()` and only re-renders when needed.
`setPixel()` 自动设置 `dirty_ = true`。Renderer 检查 `isDirty()` 仅在需要时重新渲染。

---

## `class Renderer`

### Lifecycle / 生命周期

```cpp
Renderer(int canvasW, int canvasH, int windowW = 1280, int windowH = 1280);
~Renderer();

bool init();              // Init GLFW / GLEW / shaders / textures
void setCanvas(Canvas* c); // Attach canvas / 绑定画布
bool shouldClose() const;  // Window close requested? / 窗口是否请求关闭？
void pollEvents();         // Poll GLFW events / 轮询 GLFW 事件
void render();             // One frame: upload → scanline → blend → display / 一帧：上传→扫描线→混合→显示
```

**Typical usage / 典型用法:**

```cpp
Renderer rdr(512, 512);
Canvas cvs(512, 512);
rdr.setCanvas(&cvs);
rdr.setCircleRadius(64);
rdr.setSunColor(10, 10, 10);
rdr.setWallColor(0.125f, 0.125f, 0.125f);

if (!rdr.init()) return;

while (!rdr.shouldClose()) {
    rdr.pollEvents();
    // ... modify canvas via cvs.setPixel() ...
    rdr.render();               // scanline propagation + blend + display
    // (no manual swap needed; render() does it internally)
    glfwSwapBuffers(glfwGetCurrentContext());
}
```

### Configuration API / 配置 API

```cpp
void setCircleRadius(int r);   // Ray count: ~8r per pixel / 光线数：每像素约 8r
void setSunColor(float r, float g, float b);   // Sun light color / 阳光颜色
void setWallColor(float r, float g, float b);  // Wall ambient color / 环境光颜色
int  getCircleRadius() const;                   // Current radius / 当前半径
```

**Parameter notes / 参数说明:**

| API | Range / 范围 | Default / 默认 | Effect / 效果 |
|-----|-------------|----------------|---------------|
| `circleRadius` | 1–128+ | 64 | Larger = more rays = better quality, slower / 越大光线越多 = 质量越高、越慢 |
| `sunColor` | any ≥ 0 | (10, 9, 7) | Brightness of directional sun light / 方向阳光的亮度 |
| `wallColor` | any ≥ 0 | (0.15, 0.2, 0.25) | Brightness of ambient wall light / 环境光的亮度 |

### Input Callbacks / 输入回调

```cpp
std::function<void(double x, double y)> onMouseMove;
std::function<void(int button, int action, int mods)> onMouseButton;
std::function<void(int key, int scancode, int action, int mods)> onKey;
```

Set these before entering the render loop. The library dispatches GLFW events to these hooks.
在进入渲染循环前设置。库将 GLFW 事件分发到这些钩子。

| Parameter | Meaning / 含义 |
|-----------|----------------|
| `x, y` | Screen coordinates / 屏幕坐标 (0–windowW, 0–windowH) |
| `button` | `GLFW_MOUSE_BUTTON_LEFT` / `_RIGHT` / `_MIDDLE` |
| `action` | `GLFW_PRESS` / `GLFW_RELEASE` |
| `key` | GLFW key code / GLFW 键码 |
| `mods` | `GLFW_MOD_CONTROL` / `_SHIFT` / etc. |

### Window Info / 窗口信息

```cpp
const Camera& getCamera() const;  // Read-only camera / 只读相机
int getWindowWidth()  const;      // Window width in pixels / 窗口像素宽度
int getWindowHeight() const;      // Window height in pixels / 窗口像素高度
```

### Internal Pipeline (private) / 内部管线（私有）

| Method / 方法 | Role / 作用 |
|---|---|
| `initGL()` | glfwInit, glewInit, create window / 创建窗口 |
| `initShaders()` | Compile compute + display shaders / 编译计算+显示着色器 |
| `initTextures()` | Create GL textures (color, light, occupancy, output) / 创建 GL 纹理 |
| `uploadCanvasTexture()` | CPU → GPU: upload color, light, occupancy / CPU→GPU：上传颜色、光、占空 |
| `renderScanline()` | Scanline propagation per direction family / 每个方向族的扫描线传播 |
| `blendCompSrc` | Blend scan light + color + emission → output / 混合扫描光+颜色+发光→输出 |
| `renderDisplay()` | Fullscreen quad with camera transform / 带相机变换的全屏四边形 |

---

## Compute Shader / 计算着色器

Built-in, compiled at runtime. No external `.glsl` files needed.
内建，运行时编译。无需外部 `.glsl` 文件。

| Property / 属性 | Value / 值 |
|---|---|
| Version / 版本 | `#version 430 core` |
| Workgroup / 工作组 | `layout(local_size_x = 8, local_size_y = 8)` |
| Inputs / 输入 | `u_cc` (color), `u_cl` (emission), `u_occ` (occupancy) |
| Output / 输出 | `u_out` (RGBA8 image, binding = 0) |

### Shader Uniforms / 着色器 Uniform

| Name / 名称 | Type / 类型 | Set by / 设置者 | Description / 说明 |
|---|---|---|---|
| `u_sd` | `vec2` | internal | Sun direction / 阳光方向 |
| `u_ss` | `float` | internal | Sun spread threshold / 阳光扩散阈值 |
| `u_sl` | `vec3` | `setSunColor()` | Sun light color / 阳光颜色 |
| `u_wl` | `vec3` | `setWallColor()` | Wall ambient color / 环境光颜色 |
| `u_cr` | `int` | `setCircleRadius()` | Sampling circle radius / 采样圆半径 |
| `u_cs` | `ivec2` | internal | Canvas size / 画布尺寸 |
| `u_ep` | `float` | internal | Transmittance epsilon / 透射率阈值 |

---

## Data Flow / 数据流

```
User modifies Canvas pixels via setPixel()
  → Canvas marks dirty
  → Renderer::render() checks isDirty()
  → uploadCanvasTexture() uploads to GPU:
      ├─ cvsColorTex_ (RGBA8) — pixel colors / 像素颜色
      ├─ cvsLightTex_  (RGB8)  — self-emission / 自发光
      └─ cvsOccuTex_   (R8)    — occupancy (alpha > 0.01) / 占空
  → renderScanline() processes direction families:
      ├─ Clears cvsScanTex_ (RGBA16F) to 0 / 清空扫描纹理
      ├─ For each direction family (from Bresenham circle):
      │    └─ Dispatch W+H parallel Bresenham rays with ambient light
      │       Each ray carries light forward, accumulates to cvsScanTex_
      └─ Memory barrier after each family / 每族后内存屏障
  → blend shader: reads cvsScanTex_ + cvsColorTex_ + cvsLightTex_
      → writes renderTex_ (final output)
  → renderDisplay() samples renderTex_ with camera transform
```

---

## Error Handling / 错误处理

```cpp
bool init();  // Returns false on failure (prints details to stderr)
```

`init()` will print GLFW/GLEW/shaders errors to `stderr` and return `false` on any failure.
`init()` 会在失败时将 GLFW/GLEW/着色器错误打印到 `stderr` 并返回 `false`。

### Dependencies / 依赖

- OpenGL 4.3+ (compute shader support / 计算着色器支持)
- GLFW 3.x (window + input / 窗口+输入)
- GLEW (OpenGL extensions / OpenGL 扩展)
- Windows GDI (text overlay, in app layer only / 文字叠加，仅在应用层)

---

## Thread Safety / 线程安全

**Not thread-safe.** All calls must be made from the same thread that created the GL context (typically the main thread).
**非线程安全。** 所有调用必须在创建 GL 上下文（通常是主线程）的同一线程中进行。

---

## App Layer vs Library Layer / 应用层 vs 库层

| Responsibility / 职责 | `easy_renderer` (library) | `paint_app.cpp` (app) |
|---|---|---|
| Canvas pixel buffer | ✅ | |
| GPU rendering pipeline | ✅ | |
| Config API | ✅ (setters) | |
| GLFW/GLEW init | ✅ | |
| `cfg.txt` parsing | | ✅ (→ calls API setters) |
| Drawing primitives | | ✅ (`drawLine`, `drawRect`) |
| Input handling | ✅ (hooks) | ✅ (callbacks) |
| Camera control | | ✅ (modifies `Camera` directly) |
| Text overlay (GDI) | | ✅ |
| Screenshot | | ✅ |
| Color palette | | ✅ |
