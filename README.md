# Easy Renderer — 2D GPU Ray-Traced Renderer

A lightweight, GPU-accelerated 2D ray tracer built with OpenGL compute shaders. Features real-time light simulation with Bresenham-based ray marching, occupancy-optimized performance, and an interactive paint demo.

## Features

- **GPU Ray Tracing** — Compute shader-based per-pixel light simulation with ~5000 FPS on empty canvas
- **Bresenham Circle Sampling** — 8-symmetry ray casting for efficient omnidirectional light accumulation
- **Occupancy Optimization** — Empty region skip (4-pixel stride) via real-time occupancy texture
- **Semi-Transparent Materials** — Alpha-based light transmission with correct energy conservation
- **Directional Sun Light** + **Ambient Wall Light** — Configurable direction, intensity, and color
- **Pixel Self-Emission** — Each pixel can emit colored light independently
- **Configurable** — All rendering parameters via `cfg.txt`
- **Interactive Paint Demo** — Brush, Line, Rectangle tools with 8 colors

## Screenshot

```
A paint application where you draw colored pixels on a canvas,
and the renderer simulates how light bounces and accumulates
in real time using GPU ray tracing.
```

## Quick Start

### Dependencies

- OpenGL 4.3+ (compute shader support required)
- GLFW 3.x
- GLEW
- MinGW-w64 (for building)

### Build

```bash
build.bat
```

### Run

```bash
paint_app.exe
```

## Controls

| Key | Action |
|-----|--------|
| Left Mouse | Draw / Paint |
| Right Mouse | Erase (set transparent) |
| `1`–`8` | Select color |
| `B` | Brush tool |
| `L` | Line tool |
| `R` | Rectangle tool |
| `E` | Eraser mode |
| `C` | Clear canvas |
| `Ctrl+S` | Screenshot (TGA) |
| `Ctrl+N` | Reset camera |
| Arrow Keys | Pan camera |
| `+` / `-` | Zoom in / out |
| `ESC` | Exit |

## Configuration (`cfg.txt`)

```
# Canvas and Window
512 512 64 1280 1280   # width height circleRadius windowW windowH

# Rendering
0.5                      # wireAlpha (opacity of drawn cells)

# Colors (RGB 0–255)
255 255 255 255          # text overlay color (RGBA)
255 255 255              # sun color (x10 intensity)
64 64 64                 # wall/ambient color (x0.5 intensity, ~32)

# Adaptive (reserved)
100 2 50 8

# Performance
1                        # show FPS (0 or 1)
```

**Key parameter: `circleRadius`** — Controls the ray sampling circle radius.
- Larger = more rays = better quality but slower
- Default: 64 (≈500 rays per pixel)
- For low-end: try 16–32

## Architecture

### Rendering Pipeline

```
User Draws → Canvas (CPU pixels)
  → uploadCanvasTexture() uploads color + light + occupancy to GPU
  → renderRayTrace() dispatches compute shader
    → For each pixel (8×8 workgroups):
      → If opaque (alpha ≥ 1): write directly
      → Else: cast ~64 Bresenham rays around circle radius
        → Each ray marches using Bresenham line algorithm
        → Empty pixels: skip 4 at once (occupancy optimization)
        → Semi-transparent pixels: absorb/transmit light
        → Opaque pixels: reflect + emit light
        → Canvas edge: sun or wall light
      → Average all rays → mix with pixel color → output
  → renderDisplay() samples result via camera transform
  → [app] overlay.draw() renders GDI text overlay
  → glfwSwapBuffers() presents frame
```

### Key Algorithms

| Algorithm | Where | What |
|-----------|-------|------|
| Bresenham Circle | Shader `main()` | Generates 8R sample directions per pixel |
| Bresenham Line | Shader `cR()` | Integer ray marching per direction |
| Occupancy Skip | Shader `cR()` | Skips 4 empty pixels at once using occupancy texture |

### Performance

| Scenario | FPS (circleRadius=64) |
|----------|----------------------|
| Empty canvas | 5000+ |
| Sparse content | 200–500 |
| Dense content | 30–60 |

## File Structure

```
E:\programming\CPP\2D-renderer\
├── easy_renderer.h       # Core library header
├── easy_renderer.cpp     # Core library implementation
├── paint_app.cpp         # Interactive paint application
├── build.bat             # Build script (MinGW)
├── cfg.txt               # Configuration file
├── .gitignore
├── README.md
├── include/              # GLFW + GLEW headers
│   ├── GL/
│   └── GLFW/
└── lib/                  # GLFW + GLEW libraries
    ├── glfw3.dll
    ├── libglfw3.a
    └── glew32s.lib
```

## Technical Notes

- **Compute Shader**: `#version 430 core`, workgroup size 8×8, renders into RGBA8 image
- **No Blending**: Output is written directly to `u_out` image, then sampled by display shader
- **Camera Model**: Orthographic with pan (arrow keys) and zoom (+/−)
- **Two-Layer Architecture**: `easy_renderer` is a standalone library; `paint_app.cpp` is the application layer (drawing, overlay, input)
- **Text Overlay**: GDI rasterization → OpenGL texture → custom fragment shader (in paint_app.cpp)
- **VSync**: Disabled for performance testing (`glfwSwapInterval(0)`)

## License

MIT
