#include "easy_renderer.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <algorithm>

namespace easy_renderer {

// ============================================================================
// Canvas Implementation
// ============================================================================

Canvas::Canvas(int width, int height) : width_(width), height_(height) {
    pixels_.resize(width * height, Pixel{0, 0, 0, 0, 0, 0, 0});
}

Canvas::~Canvas() {
}

void Canvas::setPixel(int x, int y, const Pixel& p) {
    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        pixels_[y * width_ + x] = p;
        dirty_ = true;
    }
}

void Canvas::clear(float r, float g, float b, float a) {
    for (auto& p : pixels_) {
        p = Pixel{r, g, b, a, 0, 0, 0};
    }
    dirty_ = true;
}

// ============================================================================
// Renderer Implementation
// ============================================================================

// Shader source codes (from original project)
static const char* rayCompSrc = R"(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
uniform sampler2D u_cc; uniform sampler2D u_cl; uniform sampler2D u_occ;
uniform vec2 u_sd; uniform float u_ss;
uniform vec3 u_sl; uniform vec3 u_wl;
uniform int u_cr; uniform ivec2 u_cs; uniform float u_ep;
layout(rgba8, binding = 0) writeonly uniform image2D u_out;

// Occupancy check: 255 = has content, 0 = empty
bool isEmptyRegion(ivec2 p) {
    float occ = texelFetch(u_occ, p, 0).r;
    return occ < 0.5;
}

vec3 cR(ivec2 o, ivec2 d) {
    vec3 light = vec3(0.0);
    float trans = 1.0;
    int dx = abs(d.x), dy = abs(d.y);
    int sx = (d.x >= 0) ? 1 : -1, sy = (d.y >= 0) ? 1 : -1;
    int er = dx - dy, x = o.x, y = o.y;
    int e2 = 2 * er;
    if (e2 > -dy) { er -= dy; x += sx; }
    if (e2 < dx) { er += dx; y += sy; }
    while (true) {
        if (x < 0 || x >= u_cs.x || y < 0 || y >= u_cs.y) {
            vec2 dn = normalize(vec2(d));
            if (1.0 - dot(dn, u_sd) <= u_ss) light += trans * u_sl; else light += trans * u_wl;
            return light;
        }
        
        // Skip empty regions: advance 4 steps at once
        if (isEmptyRegion(ivec2(x, y))) {
            for (int s = 0; s < 4; s++) {
                e2 = 2 * er;
                if (e2 > -dy) { er -= dy; x += sx; }
                if (e2 < dx) { er += dx; y += sy; }
                if (x < 0 || x >= u_cs.x || y < 0 || y >= u_cs.y) break;
                if (!isEmptyRegion(ivec2(x, y))) break;
            }
            continue;
        }
        
        vec4 c = texelFetch(u_cc, ivec2(x, y), 0);
        vec3 ref = texelFetch(u_cl, ivec2(x, y), 0).rgb;
        if (c.a >= 1.0) {
            light += trans * ref;
            return light;
        }
        if (c.a > 0.0) {
            light += trans * ref * c.a;
            trans *= (1.0 - c.a);
            if (trans < u_ep) return light;
        }
        e2 = 2 * er;
        if (e2 > -dy) { er -= dy; x += sx; }
        if (e2 < dx) { er += dx; y += sy; }
    }
}

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= u_cs.x || p.y >= u_cs.y) return;
    vec4 c = texelFetch(u_cc, p, 0);
    if (c.a >= 1.0) { imageStore(u_out, p, c); return; }
    vec3 a = vec3(0.0); int cnt = 0, r = u_cr, m = 1 - r, x = 0, y = r;
    while (x < y) {
        a += cR(p, ivec2(x, y)); cnt++; a += cR(p, ivec2(-x, y)); cnt++;
        a += cR(p, ivec2(x, -y)); cnt++; a += cR(p, ivec2(-x, -y)); cnt++;
        a += cR(p, ivec2(y, x)); cnt++; a += cR(p, ivec2(-y, x)); cnt++;
        a += cR(p, ivec2(y, -x)); cnt++; a += cR(p, ivec2(-y, -x)); cnt++;
        x++; if (m < 0) m += 2 * x + 1; else { y--; m += 2 * (x - y) + 1; }
    }
    a /= float(cnt);
    // Self light (pixel's own emission)
    vec3 selfLight = texelFetch(u_cl, p, 0).rgb;
    a += selfLight;
    a = max(a, vec3(0.0));
    float ov = max(max(a.r, a.g), a.b);
    if (ov > 1.0) a /= ov;
    // Mix accumulated light with pixel's own color by alpha
    imageStore(u_out, p, vec4(mix(a, c.rgb, c.a), 1.0));
}
)";

// Scanline light propagation shader — replaces per-pixel ray tracing
// Each work item = one Bresenham ray, carrying ambient light across the canvas
static const char* scanCompSrc = R"(#version 430 core
layout(local_size_x = 256) in;
uniform sampler2D u_cc; uniform sampler2D u_cl;
uniform ivec2 u_dir;        // direction vector (dx, dy) for this ray family
uniform ivec2 u_cs;         // canvas size
uniform vec3 u_ambient;     // ambient/wall light (entry brightness)
uniform int u_rayCount;     // number of parallel rays in this family
uniform int u_ndirs;        // total number of direction families (for normalization)
layout(rgba16f, binding = 0) uniform image2D u_scan;  // light accumulation (read+write)

void main() {
    int rayId = int(gl_GlobalInvocationID.x);
    if (rayId >= u_rayCount) return;
    
    int dx = abs(u_dir.x), dy = abs(u_dir.y);
    int sx = (u_dir.x >= 0) ? 1 : -1, sy = (u_dir.y >= 0) ? 1 : -1;
    
    // Compute ray entry point on the canvas boundary
    // Rays cover canvas: some enter from left/top, others from right/bottom
    int W = u_cs.x, H = u_cs.y;
    int x = 0, y = 0;
    int r = rayId;
    
    // Axial: entry from left/right (x edge) or top/bottom (y edge)
    if (dx == 0) {
        // Vertical: entry from top or bottom
        if (sy > 0) { x = r; y = 0; }
        else        { x = r; y = H-1; }
    } else if (dy == 0) {
        // Horizontal: entry from left or right
        if (sx > 0) { x = 0; y = r; }
        else        { x = W-1; y = r; }
    } else if (dx >= dy) {
        // Shallow diagonal: entry from left (x edge) + top (y edge)
        if (r < H) { x = sx > 0 ? 0 : W-1; y = r; }
        else       { x = r-H; y = sy > 0 ? 0 : H-1; }
    } else {
        // Steep diagonal: entry from top (y edge) + left (x edge)
        if (r < W) { x = r; y = sy > 0 ? 0 : H-1; }
        else       { x = sx > 0 ? 0 : W-1; y = r-W; }
    }
    
    // Initial light: ambient enters from the edge
    vec3 light = u_ambient;
    
    // Bresenham march
    int er = dx - dy;
    int e2 = 2 * er;
    if (e2 > -dy) { er -= dy; x += sx; }
    if (e2 < dx) { er += dx; y += sy; }
    
    while (x >= 0 && x < W && y >= 0 && y < H) {
        vec4 c = texelFetch(u_cc, ivec2(x, y), 0);
        vec3 e = texelFetch(u_cl, ivec2(x, y), 0).rgb;
        
        // Forward light propagation: absorb color, add emission
        light = light * (1.0 - c.a) + e;
        
        // Accumulate into scan texture (normalize per-direction)
        vec4 prev = imageLoad(u_scan, ivec2(x, y));
        float w = 1.0 / float(u_ndirs);
        imageStore(u_scan, ivec2(x, y), prev + vec4(light * w, 0.0));
        
        // Advance one Bresenham step
        e2 = 2 * er;
        if (e2 > -dy) { er -= dy; x += sx; }
        if (e2 < dx) { er += dx; y += sy; }
    }
}
)";

// Blend shader: copy scan light to renderTex, mixing with color + emission
static const char* blendCompSrc = R"(#version 430 core
layout(local_size_x = 8, local_size_y = 8) in;
uniform sampler2D u_cc;   // color
uniform sampler2D u_cl;   // emission
uniform sampler2D u_scan; // accumulated scan light
uniform ivec2 u_cs;
layout(rgba8, binding = 0) writeonly uniform image2D u_out;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= u_cs.x || p.y >= u_cs.y) return;
    vec4 c = texelFetch(u_cc, p, 0);
    if (c.a >= 1.0) { imageStore(u_out, p, c); return; }
    vec3 e   = texelFetch(u_cl, p, 0).rgb;
    vec3 scan = texelFetch(u_scan, p, 0).rgb;
    vec3 result = mix(scan, c.rgb, c.a) + e;
    float ov = max(max(result.r, result.g), result.b);
    if (ov > 1.0) result /= ov;
    imageStore(u_out, p, vec4(result, 1.0));
}
)";

static const char* dispVertSrc = R"(#version 330 core
layout(location = 0) in vec2 aP; layout(location = 1) in vec2 aU;
out vec2 vU;
void main() { gl_Position = vec4(aP, 0.0, 1.0); vU = aU; }
)";

static const char* dispFragSrc = R"(#version 330 core
uniform sampler2D u_t; uniform vec2 u_cp; uniform float u_cs; uniform vec2 u_cv;
in vec2 vU; out vec4 oC;
void main() {
    vec2 ab = u_cs * (vU - 0.5) + u_cp;
    vec2 mc = ab * u_cv;
    vec2 uv = mc / u_cv;
    oC = texture(u_t, uv);
}
)";

Renderer::Renderer(int canvasWidth, int canvasHeight, int windowWidth, int windowHeight)
    : canvasWidth_(canvasWidth), canvasHeight_(canvasHeight),
      windowWidth_(windowWidth), windowHeight_(windowHeight) {
}

Renderer::~Renderer() {
    if (cvsColorTex_) glDeleteTextures(1, &cvsColorTex_);
    if (cvsLightTex_) glDeleteTextures(1, &cvsLightTex_);
    if (cvsOccuTex_) glDeleteTextures(1, &cvsOccuTex_);
    if (cvsScanTex_) glDeleteTextures(1, &cvsScanTex_);
    if (renderTex_)   glDeleteTextures(1, &renderTex_);
    if (scanProg_) glDeleteProgram(scanProg_);
    if (blendProg_) glDeleteProgram(blendProg_);
    if (rayProg_) glDeleteProgram(rayProg_);
    if (dispProg_) glDeleteProgram(dispProg_);
    if (fullVAO_) glDeleteVertexArrays(1, &fullVAO_);

    if (window_) {
        glfwDestroyWindow(window_);
        glfwTerminate();
    }

    delete canvas_;
}

bool Renderer::init() {
    if (!initGL()) return false;
    if (!initShaders()) return false;
    if (!initScanShaders()) return false;
    if (!initTextures()) return false;
    return true;
}

bool Renderer::initGL() {
    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window_ = glfwCreateWindow(windowWidth_, windowHeight_, "Easy Renderer", nullptr, nullptr);
    if (!window_) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(0);  // No VSync for performance testing

    // Set user pointer for callbacks
    glfwSetWindowUserPointer(window_, this);

    // Setup callbacks
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
    glfwSetCursorPosCallback(window_, cursorPosCallback);
    glfwSetKeyCallback(window_, keyCallback);

    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        fprintf(stderr, "glewInit: %s\n", glewGetErrorString(err));
        return false;
    }

    return true;
}

bool Renderer::initShaders() {
    // Ray tracing compute shader
    GLuint cs = compileShader(rayCompSrc, GL_COMPUTE_SHADER);
    rayProg_ = glCreateProgram();
    glAttachShader(rayProg_, cs);
    glLinkProgram(rayProg_);
    {
        GLint ok = 0;
        glGetProgramiv(rayProg_, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            GLsizei len = 0;
            glGetProgramInfoLog(rayProg_, sizeof(log), &len, log);
            fprintf(stderr, "Compute shader link error:\n%s\n", log);
        }
    }
    glDeleteShader(cs);

    // Display shaders
    GLuint dvs = compileShader(dispVertSrc, GL_VERTEX_SHADER);
    GLuint dfs = compileShader(dispFragSrc, GL_FRAGMENT_SHADER);
    dispProg_ = linkProgram(dvs, dfs);
    glDeleteShader(dvs);
    glDeleteShader(dfs);

    return true;
}

bool Renderer::initScanShaders() {
    GLuint cs = compileShader(scanCompSrc, GL_COMPUTE_SHADER);
    scanProg_ = glCreateProgram();
    glAttachShader(scanProg_, cs);
    glLinkProgram(scanProg_);
    GLint ok = 0;
    glGetProgramiv(scanProg_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei len = 0;
        glGetProgramInfoLog(scanProg_, sizeof(log), &len, log);
        fprintf(stderr, "Scan shader link error:\n%s\n", log);
    }
    glDeleteShader(cs);
    // Blend shader
    cs = compileShader(blendCompSrc, GL_COMPUTE_SHADER);
    blendProg_ = glCreateProgram();
    glAttachShader(blendProg_, cs);
    glLinkProgram(blendProg_);
    ok = 0;
    glGetProgramiv(blendProg_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei len = 0;
        glGetProgramInfoLog(blendProg_, sizeof(log), &len, log);
        fprintf(stderr, "Blend shader link error:\n%s\n", log);
    }
    glDeleteShader(cs);
    return true;
}

bool Renderer::initTextures() {
    // Color texture
    glGenTextures(1, &cvsColorTex_);
    glBindTexture(GL_TEXTURE_2D, cvsColorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvasWidth_, canvasHeight_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Light texture
    glGenTextures(1, &cvsLightTex_);
    glBindTexture(GL_TEXTURE_2D, cvsLightTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, canvasWidth_, canvasHeight_, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Occupancy texture (for empty-region skip optimization)
    glGenTextures(1, &cvsOccuTex_);
    glBindTexture(GL_TEXTURE_2D, cvsOccuTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, canvasWidth_, canvasHeight_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Scanline light accumulation texture (float, for additive blending)
    glGenTextures(1, &cvsScanTex_);
    glBindTexture(GL_TEXTURE_2D, cvsScanTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, canvasWidth_, canvasHeight_, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Render texture - initialize to black
    glGenTextures(1, &renderTex_);
    glBindTexture(GL_TEXTURE_2D, renderTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvasWidth_, canvasHeight_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Clear render texture to black (prevents random garbage on startup)
    std::vector<unsigned char> black((size_t)canvasWidth_ * canvasHeight_ * 4, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasWidth_, canvasHeight_, GL_RGBA, GL_UNSIGNED_BYTE, black.data());

    // Fullscreen VAO
    float verts[] = { -1, -1, 0, 0, 3, -1, 2, 0, -1, 3, 0, 2 };
    glGenVertexArrays(1, &fullVAO_);
    glBindVertexArray(fullVAO_);
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
    glBindVertexArray(0);

    return true;
}

void Renderer::setCanvas(Canvas* canvas) {
    if (canvas_ && canvas_ != canvas) {
        delete canvas_;
    }
    canvas_ = canvas;
}

void Renderer::render() {
    if (!canvas_ || !window_) return;
    if (canvas_->isDirty()) {
        uploadCanvasTexture();
        renderScanline();
        canvas_->markClean();
    }
    renderDisplay();
}

bool Renderer::shouldClose() const {
    return window_ && glfwWindowShouldClose(window_);
}

void Renderer::pollEvents() {
    glfwPollEvents();
}

void Renderer::uploadCanvasTexture() {
    if (!canvas_) return;

    const Pixel* data = canvas_->data();

    // Upload color texture (clamp [0,1] to prevent negative wrap-around)
    std::vector<unsigned char> col((size_t)canvasWidth_ * canvasHeight_ * 4);
    for (int i = 0; i < canvasWidth_ * canvasHeight_; i++) {
        col[i*4+0] = (unsigned char)(std::max(0.0f, std::min(data[i].r, 1.0f)) * 255.0f);
        col[i*4+1] = (unsigned char)(std::max(0.0f, std::min(data[i].g, 1.0f)) * 255.0f);
        col[i*4+2] = (unsigned char)(std::max(0.0f, std::min(data[i].b, 1.0f)) * 255.0f);
        col[i*4+3] = (unsigned char)(std::max(0.0f, std::min(data[i].a, 1.0f)) * 255.0f);
    }
    glBindTexture(GL_TEXTURE_2D, cvsColorTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasWidth_, canvasHeight_, GL_RGBA, GL_UNSIGNED_BYTE, col.data());

    // Upload light texture
    std::vector<unsigned char> lig((size_t)canvasWidth_ * canvasHeight_ * 3);
    for (int i = 0; i < canvasWidth_ * canvasHeight_; i++) {
        lig[i*3+0] = (unsigned char)(std::max(0.0f, std::min(data[i].lr, 1.0f)) * 255.0f);
        lig[i*3+1] = (unsigned char)(std::max(0.0f, std::min(data[i].lg, 1.0f)) * 255.0f);
        lig[i*3+2] = (unsigned char)(std::max(0.0f, std::min(data[i].lb, 1.0f)) * 255.0f);
    }
    glBindTexture(GL_TEXTURE_2D, cvsLightTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasWidth_, canvasHeight_, GL_RGB, GL_UNSIGNED_BYTE, lig.data());

    // Build and upload occupancy texture (for empty-region skip optimization)
    std::vector<unsigned char> occ((size_t)canvasWidth_ * canvasHeight_);
    for (int i = 0; i < canvasWidth_ * canvasHeight_; i++) {
        occ[i] = (data[i].a > 0.01f) ? 255 : 0;  // 255 = has content, 0 = empty
    }
    glBindTexture(GL_TEXTURE_2D, cvsOccuTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, canvasWidth_, canvasHeight_, GL_RED, GL_UNSIGNED_BYTE, occ.data());
}

void Renderer::renderRayTrace() {
    double sunDirX = std::sqrt(0.5);
    double sunDirY = std::sqrt(0.5);
    float sunSc = 5e-3f;
    float eps_l = 1e-3f;

    // Use config-based light colors
    float sunClr[3] = {sunColor_[0], sunColor_[1], sunColor_[2]};
    float wallClr[3] = {wallColor_[0], wallColor_[1], wallColor_[2]};

    glBindImageTexture(0, renderTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glUseProgram(rayProg_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cvsColorTex_);
    glUniform1i(glGetUniformLocation(rayProg_, "u_cc"), 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, cvsLightTex_);
    glUniform1i(glGetUniformLocation(rayProg_, "u_cl"), 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, cvsOccuTex_);
    glUniform1i(glGetUniformLocation(rayProg_, "u_occ"), 2);

    glUniform2f(glGetUniformLocation(rayProg_, "u_sd"), (float)sunDirX, (float)sunDirY);
    glUniform1f(glGetUniformLocation(rayProg_, "u_ss"), sunSc);
    glUniform3fv(glGetUniformLocation(rayProg_, "u_sl"), 1, sunClr);
    glUniform3fv(glGetUniformLocation(rayProg_, "u_wl"), 1, wallClr);
    glUniform1i(glGetUniformLocation(rayProg_, "u_cr"), circleRadius_);
    glUniform2i(glGetUniformLocation(rayProg_, "u_cs"), canvasWidth_, canvasHeight_);
    glUniform1f(glGetUniformLocation(rayProg_, "u_ep"), eps_l);

    glDispatchCompute((canvasWidth_ + 7) / 8, (canvasHeight_ + 7) / 8, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
}

void Renderer::renderScanline() {
    int W = canvasWidth_, H = canvasHeight_;
    int R = circleRadius_;
    float wc[3] = {wallColor_[0], wallColor_[1], wallColor_[2]};
    
    // Clear light accumulation texture
    int N = W * H;
    std::vector<unsigned short> zero(N * 4, 0);  // half-float 0 = 0x0000
    glBindTexture(GL_TEXTURE_2D, cvsScanTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_HALF_FLOAT, zero.data());
    
    // Generate direction families from Bresenham circle
    struct { int x, y; } dirs[64];
    int m = 1 - R, x = 0, y = R;
    int nDirs = 0;
    int stride = std::max(1, R / 4);  // fewer directions for speed
    int cnt = 0;
    while (x < y && nDirs < 56) {
        if (cnt % stride == 0) {
            dirs[nDirs++] = { x,  y}; dirs[nDirs++] = {-x,  y};
            dirs[nDirs++] = { x, -y}; dirs[nDirs++] = {-x, -y};
            dirs[nDirs++] = { y,  x}; dirs[nDirs++] = {-y,  x};
            dirs[nDirs++] = { y, -x}; dirs[nDirs++] = {-y, -x};
        }
        cnt++; x++;
        if (m < 0) m += 2*x + 1; else { y--; m += 2*(x-y) + 1; }
    }
    
    glUseProgram(scanProg_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, cvsColorTex_);
    glUniform1i(glGetUniformLocation(scanProg_, "u_cc"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, cvsLightTex_);
    glUniform1i(glGetUniformLocation(scanProg_, "u_cl"), 1);
    glUniform2i(glGetUniformLocation(scanProg_, "u_cs"), W, H);
    glBindImageTexture(0, cvsScanTex_, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);
    
    for (int d = 0; d < nDirs; d++) {
        int dx = dirs[d].x, dy = dirs[d].y;
        if (dx == 0 && dy == 0) continue;
        
        glUniform2i(glGetUniformLocation(scanProg_, "u_dir"), dx, dy);
        glUniform3fv(glGetUniformLocation(scanProg_, "u_ambient"), 1, wc);
        int nRays = W + H;
        glUniform1i(glGetUniformLocation(scanProg_, "u_rayCount"), nRays);
        glUniform1i(glGetUniformLocation(scanProg_, "u_ndirs"), nDirs);
        glDispatchCompute((nRays + 255) / 256, 1, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    
    // Final blend: scan light → renderTex with color + emission mix
    glUseProgram(blendProg_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, cvsColorTex_);
    glUniform1i(glGetUniformLocation(blendProg_, "u_cc"), 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, cvsLightTex_);
    glUniform1i(glGetUniformLocation(blendProg_, "u_cl"), 1);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, cvsScanTex_);
    glUniform1i(glGetUniformLocation(blendProg_, "u_scan"), 2);
    glUniform2i(glGetUniformLocation(blendProg_, "u_cs"), W, H);
    glBindImageTexture(0, renderTex_, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute((W + 7) / 8, (H + 7) / 8, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
}

void Renderer::renderDisplay() {
    glViewport(0, 0, windowWidth_, windowHeight_);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(dispProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, renderTex_);
    glUniform1i(glGetUniformLocation(dispProg_, "u_t"), 0);
    glUniform2f(glGetUniformLocation(dispProg_, "u_cp"), (float)camera_.x, (float)camera_.y);
    glUniform1f(glGetUniformLocation(dispProg_, "u_cs"), (float)camera_.scale);
    glUniform2f(glGetUniformLocation(dispProg_, "u_cv"), (float)canvasWidth_, (float)canvasHeight_);

    glBindVertexArray(fullVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

GLuint Renderer::compileShader(const char* src, GLenum type) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei len = 0;
        glGetShaderInfoLog(s, sizeof(log), &len, log);
        fprintf(stderr, "Shader compile error:\n%s\n", log);
    }
    return s;
}

GLuint Renderer::linkProgram(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei len = 0;
        glGetProgramInfoLog(p, sizeof(log), &len, log);
        fprintf(stderr, "Program link error:\n%s\n", log);
    }
    return p;
}

// ============================================================================
// Static Callbacks
// ============================================================================

void Renderer::mouseButtonCallback(GLFWwindow* w, int button, int action, int mods) {
    Renderer* r = (Renderer*)glfwGetWindowUserPointer(w);
    if (r && r->onMouseButton) {
        r->onMouseButton(button, action, mods);
    }
}

void Renderer::cursorPosCallback(GLFWwindow* w, double x, double y) {
    Renderer* r = (Renderer*)glfwGetWindowUserPointer(w);
    if (r) {
        r->mouseX_ = x;
        r->mouseY_ = y;
        if (r->onMouseMove) {
            r->onMouseMove(x, y);
        }
    }
}

void Renderer::keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods) {
    Renderer* r = (Renderer*)glfwGetWindowUserPointer(w);
    if (r && r->onKey) {
        r->onKey(key, scancode, action, mods);
    }
}

// ============================================================================
// Configuration API
// ============================================================================

void Renderer::setCircleRadius(int r) {
    if (r > 0) circleRadius_ = r;
}

void Renderer::setSunColor(float r, float g, float b) {
    sunColor_[0] = r; sunColor_[1] = g; sunColor_[2] = b;
}

void Renderer::setWallColor(float r, float g, float b) {
    wallColor_[0] = r; wallColor_[1] = g; wallColor_[2] = b;
}

} // namespace easy_renderer
