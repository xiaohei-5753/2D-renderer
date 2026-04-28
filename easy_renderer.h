#pragma once
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <vector>
#include <string>
#include <functional>

namespace easy_renderer {

// ============================================================================
// Core Data Types
// ============================================================================

struct Pixel {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;  // color
    float lr = 0.0f, lg = 0.0f, lb = 0.0f;            // self-emission
};

struct Camera {
    double x = 0.5, y = 0.5, scale = 1.0;
};

// ============================================================================
// Canvas – pixel buffer + dirty tracking
// ============================================================================

class Canvas {
public:
    Canvas(int width, int height);
    ~Canvas();

    void setPixel(int x, int y, const Pixel& p);
    void clear(float r = 0, float g = 0, float b = 0, float a = 0);

    const Pixel* data() const { return pixels_.data(); }
    int width()  const { return width_; }
    int height() const { return height_; }
    bool isDirty() const { return dirty_; }
    void markClean() { dirty_ = false; }

private:
    int width_;
    int height_;
    std::vector<Pixel> pixels_;
    bool dirty_ = true;
};

// ============================================================================
// Renderer – GPU ray tracing pipeline
// ============================================================================

class Renderer {
public:
    Renderer(int canvasWidth, int canvasHeight,
             int windowWidth = 1280, int windowHeight = 1280);
    ~Renderer();

    // --- Lifecycle ------------------------------------------------
    bool init();
    void setCanvas(Canvas* canvas);

    bool shouldClose() const;
    void pollEvents();
    void render();

    // --- Input hooks (app sets these, library dispatches) ---------
    std::function<void(double x, double y)> onMouseMove;
    std::function<void(int button, int action, int mods)> onMouseButton;
    std::function<void(int key, int scancode, int action, int mods)> onKey;

    // --- Camera ---------------------------------------------------
    const Camera& getCamera() const { return camera_; }

    // --- Window info ----------------------------------------------
    int getWindowWidth()  const { return windowWidth_; }
    int getWindowHeight() const { return windowHeight_; }

    // --- Config ---------------------------------------------------
    void loadConfig(const std::string& configPath);

    // --- Read-only settings for the app ---------------------------
    int getCircleRadius() const { return circleRadius_; }

private:
    // GLFW callbacks (dispatch to std::function hooks)
    static void mouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* w, double x, double y);
    static void keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods);

    // --- State ----------------------------------------------------
    int canvasWidth_;
    int canvasHeight_;
    int windowWidth_;
    int windowHeight_;
    GLFWwindow* window_ = nullptr;
    Canvas* canvas_ = nullptr;
    Camera camera_;

    double mouseX_ = 0, mouseY_ = 0;  // for callbacks
    bool mouseLeft_ = false, mouseRight_ = false;

    // --- OpenGL resources -----------------------------------------
    GLuint cvsColorTex_ = 0;
    GLuint cvsLightTex_ = 0;
    GLuint cvsOccuTex_ = 0;              // occupancy (empty-region skip)
    GLuint renderTex_   = 0;
    GLuint rayProg_     = 0;
    GLuint dispProg_    = 0;
    GLuint fullVAO_     = 0;

    // --- Settings -------------------------------------------------
    int circleRadius_ = 64;
    float wireAlpha_  = 0.5f;
    float sunColor_[3]  = {1.0f, 0.9f, 0.7f};
    float wallColor_[3] = {0.3f, 0.4f, 0.5f};

    // --- Private helpers ------------------------------------------
    bool initGL();
    bool initShaders();
    bool initTextures();
    void uploadCanvasTexture();
    void renderRayTrace();
    void renderDisplay();

    GLuint compileShader(const char* src, GLenum type);
    GLuint linkProgram(GLuint vs, GLuint fs);
};

} // namespace easy_renderer
