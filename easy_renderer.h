#pragma once
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <vector>
#include <string>
#include <functional>

namespace easy_renderer {

// Forward declaration
struct Pixel;

// AABB (Axis-Aligned Bounding Box)
struct AABB {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool isEmpty() const { return x1 >= x2 || y1 >= y2; }
    int width() const { return x2 - x1; }
    int height() const { return y2 - y1; }
    bool contains(int x, int y) const { return x >= x1 && x < x2 && y >= y1 && y < y2; }
};

// Quadtree for spatial optimization
class Quadtree {
public:
    Quadtree(int x1, int y1, int x2, int y2, int depth = 0);
    ~Quadtree();
    
    // Build from canvas - returns true if this region has any content
    bool build(const Pixel* pixels, int w, int h);
    
    // Query: check if ray intersects any content
    // Returns true if ray should continue tracing (no hit in this region)
    bool rayTrace(int startX, int startY, int dirX, int dirY, int maxDist) const;
    
    // Get occupancy (0 = empty, 1 = partial, 2 = full)
    int getOccupancy() const { return occupancy_; }
    
private:
    AABB bounds_;
    Quadtree* children_[4] = {nullptr, nullptr, nullptr, nullptr};
    int occupancy_ = 0;  // 0: empty, 1: partial, 2: full
    int depth_;
    
    static constexpr int MIN_SIZE = 16;  // Minimum quadtree cell size
    
    void clear();
    bool subdivide(const Pixel* pixels, int w, int h);
};

struct Pixel {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;  // color
    float lr = 0.0f, lg = 0.0f, lb = 0.0f;              // light/emission
};

struct Camera {
    double x = 0.5, y = 0.5, scale = 1.0;
};

class Canvas {
public:
    Canvas(int width, int height);
    ~Canvas();
    
    // Pixel access
    void setPixel(int x, int y, const Pixel& p);
    void setPixel(int x, int y, float r, float g, float b, float a);
    void getPixel(int x, int y, Pixel& p) const;
    void clear(float r = 0, float g = 0, float b = 0, float a = 0);
    
    // Drawing primitives (Bresenham line algorithm)
    void drawLine(int x1, int y1, int x2, int y2, const Pixel& p);
    void drawRect(int x1, int y1, int x2, int y2, const Pixel& p);
    void fillRect(int x1, int y1, int x2, int y2, const Pixel& p);
    
    // Query
    int width() const { return width_; }
    int height() const { return height_; }
    const Pixel* data() const { return pixels_.data(); }
    bool isDirty() const { return dirty_; }
    void markClean() { dirty_ = false; }
    
    // Convert screen coordinates to canvas coordinates
    void screenToCanvas(double sx, double sy, int& cx, int& cy, const Camera& cam, int winW, int winH) const;
    
    // Rebuild quadtree (call after modifying canvas content)
    void rebuildQuadtree();
    
    // Query quadtree for ray tracing
    bool rayTrace(int startX, int startY, int dirX, int dirY, int maxDist) const;
    
private:
    int width_;
    int height_;
    std::vector<Pixel> pixels_;
    bool dirty_ = true;
    Quadtree* quadtree_ = nullptr;
};

struct AdaptiveConfig {
    // Near pixels: precise, fewer rays, smaller steps
    int nearRays = 100;     // rays per pixel for nearby regions
    int nearStepSize = 2;     // step size near pixel
    
    // Far pixels: sparse, more rays, larger steps  
    int farRays = 50;       // rays per pixel for far regions
    int farStepSize = 8;     // step size far from pixel
    
    // Threshold (in pixels) for near/far distinction
    int nearThreshold = 128;
};

class Renderer {
public:
    Renderer(int canvasWidth, int canvasHeight, int windowWidth = 1280, int windowHeight = 1280);
    ~Renderer();
    
    // Initialization
    bool init();
    bool isValid() const;
    
    // Canvas management
    void setCanvas(Canvas* canvas);
    Canvas* getCanvas() const { return canvas_; }
    
    // Main render loop
    void render();
    void swapBuffers();
    
    // Event processing
    bool shouldClose() const;
    void pollEvents();
    
    // Input callbacks (set by user)
    std::function<void(double x, double y)> onMouseMove;
    std::function<void(int button, int action, int mods)> onMouseButton;
    std::function<void(int key, int scancode, int action, int mods)> onKey;
    
    // Camera control
    void pan(double dx, double dy);
    void zoom(double factor);
    void resetCamera();
    const Camera& getCamera() const { return camera_; }
    
    // Window info
    int getWindowWidth() const { return windowWidth_; }
    int getWindowHeight() const { return windowHeight_; }
    GLFWwindow* getWindow() const { return window_; }
    float getWireAlpha() const { return wireAlpha_; }
    
    // Get current mouse position
    double getMouseX() const { return mouseX_; }
    double getMouseY() const { return mouseY_; }
    
    // Check if mouse buttons are pressed
    bool isMouseLeftDown() const { return mouseLeft_; }
    bool isMouseRightDown() const { return mouseRight_; }
    
    // Screen capture
    void screenshot(const std::string& filename = "");
    
    // Text overlay (simplified - just shows basic info)
    void setOverlayText(const std::string& text);
    
    // FPS counter
    float getFPS() const { return fps_; }
    void setShowFPS(bool show) { showFPS_ = show; }
    
    // Configuration
    void loadConfig(const std::string& configPath);
    void saveConfig(const std::string& configPath) const;
    
private:
    static void mouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* w, double x, double y);
    static void keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods);
    
    int canvasWidth_;
    int canvasHeight_;
    int windowWidth_;
    int windowHeight_;
    GLFWwindow* window_ = nullptr;
    Canvas* canvas_ = nullptr;
    Camera camera_;
    
    // Mouse state
    double mouseX_ = 0, mouseY_ = 0;
    double prevMouseX_ = 0, prevMouseY_ = 0;
    bool mouseLeft_ = false;
    bool mouseRight_ = false;
    
    // OpenGL resources
    GLuint cvsColorTex_ = 0;
    GLuint cvsLightTex_ = 0;
    GLuint cvsOccuTex_ = 0;  // occupancy texture for quadtree optimization
    GLuint renderTex_ = 0;
    GLuint rayProg_ = 0;
    GLuint dispProg_ = 0;
    GLuint fullVAO_ = 0;
    GLuint overlayTex_ = 0;
    GLuint overlayProg_ = 0;
    
    // Overlay text
    std::string overlayText_;
    std::string overlayTextCache_;
    int overlayTexW_ = 0, overlayTexH_ = 0;
    
    // Settings
    int circleRadius_ = 32;
    float wireAlpha_ = 0.8f;
    float textColor_[4] = {1, 1, 1, 1};
    float sunColor_[3] = {1, 0.9f, 0.7f};
    float wallColor_[3] = {0.3f, 0.4f, 0.5f};
    
    // Adaptive continuous sampling
    int adNearStep_ = 2;
    int adFarStep_ = 8;
    int adNearRays_ = 100;
    int adFarRays_ = 50;
    
    // FPS counter
    float fps_ = 0.0f;
    bool showFPS_ = true;
    double lastFrameTime_ = 0.0;
    
    bool initGL();
    bool initShaders();
    bool initTextures();
    void uploadCanvasTexture();
    void renderRayTrace();
    void renderTextOverlay();
    void renderDisplay();
    
    GLuint compileShader(const char* src, GLenum type);
    GLuint linkProgram(GLuint vs, GLuint fs);
};

} // namespace easy_renderer