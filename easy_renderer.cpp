#define GLFW_EXPOSE_NATIVE_WIN32
#include "easy_renderer.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <windows.h>
#include <GLFW/glfw3native.h>

namespace easy_renderer {

// ============================================================================
// Quadtree Implementation  
// ============================================================================

Quadtree::Quadtree(int x1, int y1, int x2, int y2, int depth)
    : bounds_{x1, y1, x2, y2}, depth_(depth) {
}

Quadtree::~Quadtree() {
    clear();
}

void Quadtree::clear() {
    for (int i = 0; i < 4; i++) {
        delete children_[i];
        children_[i] = nullptr;
    }
    occupancy_ = 0;
}

bool Quadtree::subdivide(const Pixel* pixels, int w, int h) {
    int mx = (bounds_.x1 + bounds_.x2) / 2;
    int my = (bounds_.y1 + bounds_.y2) / 2;
    
    // Create 4 children
    children_[0] = new Quadtree(bounds_.x1, bounds_.y1, mx, my, depth_ + 1);      // top-left
    children_[1] = new Quadtree(mx, bounds_.y1, bounds_.x2, my, depth_ + 1);     // top-right
    children_[2] = new Quadtree(bounds_.x1, my, mx, bounds_.y2, depth_ + 1);     // bottom-left
    children_[3] = new Quadtree(mx, my, bounds_.x2, bounds_.y2, depth_ + 1);    // bottom-right
    
    bool anyContent = false;
    for (int i = 0; i < 4; i++) {
        anyContent |= children_[i]->build(pixels, w, h);
    }
    return anyContent;
}

bool Quadtree::build(const Pixel* pixels, int w, int h) {
    clear();
    
    // Check if this region has any content
    bool hasContent = false;
    int contentCount = 0;
    
    for (int y = bounds_.y1; y < bounds_.y2 && y < h; y++) {
        for (int x = bounds_.x1; x < bounds_.x2 && x < w; x++) {
            const Pixel& p = pixels[y * w + x];
            if (p.a > 0.01f) {  // Has some opacity
                contentCount++;
                hasContent = true;
            }
        }
    }
    
    if (!hasContent) {
        occupancy_ = 0;  // Empty
        return false;
    }
    
    int totalPixels = (bounds_.x2 - bounds_.x1) * (bounds_.y2 - bounds_.y1);
    if (contentCount == totalPixels) {
        occupancy_ = 2;  // Full
        return true;
    }
    
    // Partial - check if should subdivide
    if (bounds_.width() > MIN_SIZE && bounds_.height() > MIN_SIZE) {
        if (subdivide(pixels, w, h)) {
            occupancy_ = 1;  // Partial
            return true;
        }
    }
    
    occupancy_ = 2;  // Treat as full (has content but too small to subdivide)
    return true;
}

// Ray trace using quadtree - returns true to continue tracing (no hit)
bool Quadtree::rayTrace(int startX, int startY, int dirX, int dirY, int maxDist) const {
    // Bresenham line marching with quadtree skip
    int x = startX, y = startY;
    int dx = abs(dirX), dy = abs(dirY);
    int sx = (dirX >= 0) ? 1 : -1;
    int sy = (dirY >= 0) ? 1 : -1;
    int err = dx - dy;
    
    while (true) {
        // Skip if outside bounds
        if (!bounds_.contains(x, y)) {
            // Moved outside - don't check further
            return true;
        }
        
        // Check this quadtree node
        if (occupancy_ == 0) {
            // Empty region - skip ahead
        } else if (occupancy_ == 2) {
            // Full (has content) - check specific pixel
            // Caller should check the actual pixel
            return true;
        } else {
            // Partial - check children
            bool hit = false;
            for (int i = 0; i < 4; i++) {
                if (children_[i] && children_[i]->bounds_.contains(x, y)) {
                    // This child contains the point, continue
                    break;
                }
            }
        }
        
        // Move to next pixel
        if (x == startX + dirX * maxDist && y == startY + dirY * maxDist) break;
        
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
    
    return true;
}

// ============================================================================
// Canvas Implementation
// ============================================================================

Canvas::Canvas(int width, int height) : width_(width), height_(height) {
    pixels_.resize(width * height, Pixel{0, 0, 0, 0, 0, 0, 0});
}

Canvas::~Canvas() {
    delete quadtree_;
}

void Canvas::setPixel(int x, int y, const Pixel& p) {
    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        pixels_[y * width_ + x] = p;
        dirty_ = true;
    }
}

void Canvas::setPixel(int x, int y, float r, float g, float b, float a) {
    setPixel(x, y, Pixel{r, g, b, a, 0, 0, 0});
}

void Canvas::getPixel(int x, int y, Pixel& p) const {
    if (x >= 0 && x < width_ && y >= 0 && y < height_) {
        p = pixels_[y * width_ + x];
    } else {
        p = Pixel{0, 0, 0, 0, 0, 0, 0};
    }
}

void Canvas::clear(float r, float g, float b, float a) {
    for (auto& p : pixels_) {
        p = Pixel{r, g, b, a, 0, 0, 0};
    }
    dirty_ = true;
}

void Canvas::drawLine(int x1, int y1, int x2, int y2, const Pixel& p) {
    // Bresenham line algorithm
    int dx = abs(x2 - x1);
    int dy = -abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx + dy;
    
    while (true) {
        setPixel(x1, y1, p);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            if (x1 == x2) break;
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            if (y1 == y2) break;
            err += dx;
            y1 += sy;
        }
    }
}

void Canvas::drawRect(int x1, int y1, int x2, int y2, const Pixel& p) {
    int cx1 = std::max(0, std::min(x1, x2));
    int cy1 = std::max(0, std::min(y1, y2));
    int cx2 = std::min(width_ - 1, std::max(x1, x2));
    int cy2 = std::min(height_ - 1, std::max(y1, y2));
    
    // Top and bottom edges
    for (int x = cx1; x <= cx2; x++) {
        setPixel(x, cy1, p);
        setPixel(x, cy2, p);
    }
    // Left and right edges
    for (int y = cy1; y <= cy2; y++) {
        setPixel(cx1, y, p);
        setPixel(cx2, y, p);
    }
}

void Canvas::fillRect(int x1, int y1, int x2, int y2, const Pixel& p) {
    int cx1 = std::max(0, std::min(x1, x2));
    int cy1 = std::max(0, std::min(y1, y2));
    int cx2 = std::min(width_ - 1, std::max(x1, x2));
    int cy2 = std::min(height_ - 1, std::max(y1, y2));
    
    for (int y = cy1; y <= cy2; y++) {
        for (int x = cx1; x <= cx2; x++) {
            setPixel(x, y, p);
        }
    }
}

void Canvas::screenToCanvas(double sx, double sy, int& cx, int& cy, const Camera& cam, int winW, int winH) const {
    double u = sx / winW;
    double v = 1.0 - sy / winH;
    double abx = cam.scale * (u - 0.5) + cam.x;
    double aby = cam.scale * (v - 0.5) + cam.y;
    cx = (int)(abx * width_);
    cy = (int)(aby * height_);
    if (cx < 0) cx = 0;
    if (cx >= width_) cx = width_ - 1;
    if (cy < 0) cy = 0;
    if (cy >= height_) cy = height_ - 1;
}

void Canvas::rebuildQuadtree() {
    delete quadtree_;
    quadtree_ = new Quadtree(0, 0, width_, height_, 0);
    quadtree_->build(pixels_.data(), width_, height_);
}

bool Canvas::rayTrace(int startX, int startY, int dirX, int dirY, int maxDist) const {
    if (!quadtree_) return true;
    return quadtree_->rayTrace(startX, startY, dirX, dirY, maxDist);
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

static const char* overlayFragSrc = R"(#version 330 core
uniform sampler2D u_t;
uniform vec2 u_tl;
uniform vec2 u_size;
uniform vec2 u_win;
uniform vec3 u_color;
in vec2 vU;
out vec4 oC;
void main() {
    vec2 px = vU * u_win;
    vec2 pos = px - u_tl;
    if (pos.x >= 0.0 && pos.x < u_size.x && pos.y >= 0.0 && pos.y < u_size.y) {
        vec2 tc = vec2(pos.x / u_size.x, 1.0 - pos.y / u_size.y);
        float a = texture(u_t, tc).a;
        if (a < 0.005) discard;
        oC = vec4(u_color, a);
    } else {
        discard;
    }
}
)";

Renderer::Renderer(int canvasWidth, int canvasHeight, int windowWidth, int windowHeight)
    : canvasWidth_(canvasWidth), canvasHeight_(canvasHeight),
      windowWidth_(windowWidth), windowHeight_(windowHeight) {
    canvas_ = new Canvas(canvasWidth, canvasHeight);
}

Renderer::~Renderer() {
    // Cleanup OpenGL resources
    if (cvsColorTex_) glDeleteTextures(1, &cvsColorTex_);
    if (cvsLightTex_) glDeleteTextures(1, &cvsLightTex_);
    if (renderTex_) glDeleteTextures(1, &renderTex_);
    if (rayProg_) glDeleteProgram(rayProg_);
    if (dispProg_) glDeleteProgram(dispProg_);
    if (fullVAO_) glDeleteVertexArrays(1, &fullVAO_);
    if (overlayTex_) glDeleteTextures(1, &overlayTex_);
    if (overlayProg_) glDeleteProgram(overlayProg_);
    
    if (window_) {
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
    
    delete canvas_;
}

bool Renderer::init() {
    if (!initGL()) return false;
    if (!initShaders()) return false;
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
    
    // Occupancy texture (for quadtree optimization)
    glGenTextures(1, &cvsOccuTex_);
    glBindTexture(GL_TEXTURE_2D, cvsOccuTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, canvasWidth_, canvasHeight_, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
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

bool Renderer::isValid() const {
    return window_ != nullptr;
}

void Renderer::setCanvas(Canvas* canvas) {
    if (canvas_ && canvas_ != canvas) {
        delete canvas_;
    }
    canvas_ = canvas;
}

void Renderer::render() {
    if (!canvas_ || !window_) return;
    
    // FPS calculation
    double currentTime = glfwGetTime();
    if (lastFrameTime_ > 0) {
        float delta = currentTime - lastFrameTime_;
        if (delta > 0) fps_ = 0.9f * fps_ + 0.1f * (1.0f / delta);
    }
    lastFrameTime_ = currentTime;
    
    // Update FPS overlay
    if (showFPS_) {
        char fpsText[64];
        snprintf(fpsText, sizeof(fpsText), "FPS: %.1f", fps_);
        setOverlayText(fpsText);
    }
    
    if (canvas_->isDirty()) {
        uploadCanvasTexture();
        renderRayTrace();
        canvas_->markClean();
    }
    
    renderDisplay();
    
    if (!overlayText_.empty() && showFPS_) {
        renderTextOverlay();
    }
    
    glfwSwapBuffers(window_);
}

void Renderer::swapBuffers() {
    if (window_) {
        glfwSwapBuffers(window_);
    }
}

bool Renderer::shouldClose() const {
    return window_ && glfwWindowShouldClose(window_);
}

void Renderer::pollEvents() {
    glfwPollEvents();
}

void Renderer::pan(double dx, double dy) {
    camera_.x += dx * camera_.scale;
    camera_.y += dy * camera_.scale;
}

void Renderer::zoom(double factor) {
    camera_.scale *= factor;
}

void Renderer::resetCamera() {
    camera_ = Camera{0.5, 0.5, 1.0};
}

void Renderer::screenshot(const std::string& filename) {
    static int scrCnt = 0;
    std::string fn = filename.empty() ? std::to_string(++scrCnt) + "_screenshot.tga" : filename;
    
    std::vector<unsigned char> buf(windowWidth_ * windowHeight_ * 3);
    glReadPixels(0, 0, windowWidth_, windowHeight_, GL_BGR, GL_UNSIGNED_BYTE, buf.data());
    
    FILE* f = fopen(fn.c_str(), "wb");
    if (f) {
        unsigned char hdr[18] = {0};
        hdr[2] = 2;
        hdr[12] = windowWidth_ & 255;
        hdr[13] = windowWidth_ >> 8;
        hdr[14] = windowHeight_ & 255;
        hdr[15] = windowHeight_ >> 8;
        hdr[16] = 24;
        hdr[17] = 0x20;
        fwrite(hdr, 1, 18, f);
        fwrite(buf.data(), 1, buf.size(), f);
        fclose(f);
        printf("Saved: %s\n", fn.c_str());
    }
}

void Renderer::setOverlayText(const std::string& text) {
    overlayText_ = text;
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
    
    // Build and upload occupancy texture (for quadtree optimization)
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

void Renderer::renderTextOverlay() {
    // Lazy init shader
    if (overlayProg_ == 0) {
        GLuint vs = compileShader(dispVertSrc, GL_VERTEX_SHADER);
        GLuint fs = compileShader(overlayFragSrc, GL_FRAGMENT_SHADER);
        overlayProg_ = linkProgram(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);
    }
    
    // Rebuild texture only if text changed
    if (overlayText_ != overlayTextCache_) {
        overlayTextCache_ = overlayText_;
        
        // Create GDI memory bitmap with text
        HDC memDC = CreateCompatibleDC(nullptr);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 255));
        HFONT hFont = CreateFontA(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FIXED_PITCH, "Consolas");
        HGDIOBJ hOldFont = SelectObject(memDC, hFont);
        
        // Measure text extent
        SIZE sz = {};
        GetTextExtentPoint32A(memDC, overlayText_.c_str(), (int)overlayText_.size(), &sz);
        int tw = ((sz.cx + 15) / 16 * 16) + 8;
        int th = sz.cy + 6;
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        
        HBITMAP hBmp = CreateCompatibleBitmap(memDC, tw, th);
        HGDIOBJ hOldBmp = SelectObject(memDC, hBmp);
        
        RECT rc = {0, 0, tw, th};
        HBRUSH hBr = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &rc, hBr);
        DeleteObject(hBr);
        DrawTextA(memDC, overlayText_.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
        
        // Extract bitmap pixels
        std::vector<unsigned char> pixels((size_t)tw * th * 4, 0);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = tw;
        bmi.bmiHeader.biHeight = -th;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(memDC, hBmp, 0, th, pixels.data(), &bmi, DIB_RGB_COLORS);
        
        SelectObject(memDC, hOldBmp);
        SelectObject(memDC, hOldFont);
        DeleteObject(hBmp);
        DeleteObject(hFont);
        DeleteDC(memDC);
        
        // Convert to alpha mask
        for (int i = 0; i < tw * th; i++) {
            unsigned char r = pixels[i*4+2];
            unsigned char g = pixels[i*4+1];
            unsigned char b = pixels[i*4+0];
            unsigned char a = (unsigned char)(0.299f * r + 0.587f * g + 0.114f * b);
            pixels[i*4+0] = 255;
            pixels[i*4+1] = 255;
            pixels[i*4+2] = 255;
            pixels[i*4+3] = a;
        }
        
        // Upload texture
        if (overlayTex_ == 0) glGenTextures(1, &overlayTex_);
        glBindTexture(GL_TEXTURE_2D, overlayTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        overlayTexW_ = tw;
        overlayTexH_ = th;
    }
    
    if (overlayTex_ == 0 || overlayText_.empty()) return;
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    glUseProgram(overlayProg_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, overlayTex_);
    glUniform1i(glGetUniformLocation(overlayProg_, "u_t"), 0);
    glUniform2f(glGetUniformLocation(overlayProg_, "u_tl"), 10.0f, 10.0f);
    glUniform2f(glGetUniformLocation(overlayProg_, "u_size"), (float)overlayTexW_, (float)overlayTexH_);
    glUniform2f(glGetUniformLocation(overlayProg_, "u_win"), (float)windowWidth_, (float)windowHeight_);
    glUniform3f(glGetUniformLocation(overlayProg_, "u_color"), textColor_[0], textColor_[1], textColor_[2]);
    glBindVertexArray(fullVAO_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    
    glDisable(GL_BLEND);
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
        r->prevMouseX_ = r->mouseX_;
        r->prevMouseY_ = r->mouseY_;
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

} // namespace easy_renderer

// ============================================================================
// Configuration Functions (outside namespace for implementation)
// ============================================================================

void easy_renderer::Renderer::loadConfig(const std::string& configPath) {
    std::ifstream fin(configPath);
    if (!fin) {
        fprintf(stderr, "Failed to open config file: %s, using defaults\n", configPath.c_str());
        return;
    }
    
    // Helper: read next non-comment line (skip lines starting with #)
    auto skipComments = [&]() {
        int ch;
        while ((ch = fin.peek()) != EOF) {
            if (ch == '#') {
                fin.ignore(1024, '\n');  // skip comment line
            } else if (ch == '\n' || ch == '\r') {
                fin.get();  // skip empty lines
            } else {
                break;
            }
        }
    };
    
    // Line 1: canvas and window settings
    skipComments();
    fin >> canvasWidth_ >> canvasHeight_ >> circleRadius_ >> windowWidth_ >> windowHeight_;
    
    // Validate
    if (canvasWidth_ < 1) canvasWidth_ = 512;
    if (canvasHeight_ < 1) canvasHeight_ = 512;
    if (circleRadius_ < 1) circleRadius_ = 64;
    if (windowWidth_ < 100) windowWidth_ = 1280;
    if (windowHeight_ < 100) windowHeight_ = 1280;
    
    std::string line;
    std::getline(fin, line);  // consume rest of line
    
    // Wire alpha (no font name in our format)
    skipComments();
    fin >> wireAlpha_;
    
    // Text color
    skipComments();
    float tr, tg, tb, ta;
    fin >> tr >> tg >> tb >> ta;
    textColor_[0] = tr / 255.f; textColor_[1] = tg / 255.f;
    textColor_[2] = tb / 255.f; textColor_[3] = ta / 255.f;
    
    // Sun color
    skipComments();
    float sr, sg, sb;
    fin >> sr >> sg >> sb;
    sunColor_[0] = sr / 255.f * 10.f;
    sunColor_[1] = sg / 255.f * 10.f;
    sunColor_[2] = sb / 255.f * 10.f;
    
    // Wall color
    skipComments();
    float wr, wg, wb;
    fin >> wr >> wg >> wb;
    wallColor_[0] = wr / 255.f * 0.5f;
    wallColor_[1] = wg / 255.f * 0.5f;
    wallColor_[2] = wb / 255.f * 0.5f;
    
    // Adaptive sampling
    skipComments();
    int nr = 100, ns = 2, fr = 50, fst = 8, nt = 128;
    if (fin >> nr >> ns >> fr >> fst >> nt) {
        adNearRays_ = nr;
        adNearStep_ = ns;
        adFarRays_ = fr;
        adFarStep_ = fst;
    }
    
    // Show FPS
    skipComments();
    int showFPS = 1;
    fin >> showFPS;
    showFPS_ = (showFPS != 0);
    
    fin.close();
    printf("Loaded config: %s (wireAlpha=%.2f, sun=(%.1f,%.1f,%.1f), wall=(%.3f,%.3f,%.3f), r=%d)\n",
        configPath.c_str(), wireAlpha_,
        sunColor_[0], sunColor_[1], sunColor_[2],
        wallColor_[0], wallColor_[1], wallColor_[2],
        circleRadius_);
}

void easy_renderer::Renderer::saveConfig(const std::string& configPath) const {
    std::ofstream fout(configPath);
    if (!fout) {
        fprintf(stderr, "Failed to write config file: %s\n", configPath.c_str());
        return;
    }
    
    fout << "# Easy Renderer Configuration File\n";
    fout << "# Format: each line is a parameter or parameter group\n\n";
    
    // Line 1: canvas and window settings
    fout << canvasWidth_ << " " << canvasHeight_ << " " << circleRadius_ 
         << " " << windowWidth_ << " " << windowHeight_ << "\n\n";
    
    // Line 2: rendering settings
    fout << "# [wireAlpha] - opacity of wire cells (0.0 - 1.0)\n";
    fout << wireAlpha_ << "\n\n";
    
    // Line 3: text overlay color
    fout << "# Text overlay color (RGBA)\n";
    fout << (int)(textColor_[0] * 255) << " " << (int)(textColor_[1] * 255) << " " 
         << (int)(textColor_[2] * 255) << " " << (int)(textColor_[3] * 255) << "\n\n";
    
    // Line 4: sun color
    fout << "# Sun/light color (RGB)\n";
    fout << (int)(sunColor_[0] * 255 / 10) << " " << (int)(sunColor_[1] * 255 / 10) << " " 
         << (int)(sunColor_[2] * 255 / 10) << "\n\n";
    
    // Line 5: wall color
    fout << "# Wall/ambient color (RGB)\n";
    fout << (int)(wallColor_[0] * 255 * 2) << " " << (int)(wallColor_[1] * 255 * 2) << " " 
         << (int)(wallColor_[2] * 255 * 2) << "\n\n";
    
    // Line 6: adaptive sampling (optional)
    fout << "# Adaptive sampling: nearRays nearStepSize farRays farStepSize nearThreshold\n";
    // Assuming default values - would need actual adaptive config storage
    fout << "100 2 50 8 128\n\n";
    
    // Line 7: show FPS
    fout << "# Show FPS (0 or 1)\n";
    fout << (showFPS_ ? 1 : 0) << "\n";
    
    fout.close();
    printf("Saved config: %s\n", configPath.c_str());
}