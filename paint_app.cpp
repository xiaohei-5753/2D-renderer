#define _CRT_SECURE_NO_WARNINGS
#include "easy_renderer.h"
#include <cstdio>
#include <iostream>
#include <string>
#include <algorithm>
#include <windows.h>

using namespace easy_renderer;

// Simple paint application
class PaintApp {
public:
    PaintApp()
        : canvas_(nullptr)
        , renderer_(nullptr)
        , currentTool_(Tool::Brush)
        , currentColorIndex_(0) {
        
        canvas_ = new Canvas(512, 512);
        renderer_ = new Renderer(512, 512, 1280, 1280);
        
        // Predefined colors
        colors_[0] = {0, 0, 0, .5, 0, 0, 0};       // Black
        colors_[1] = {1, 0, 0, .5, 1, 0, 0};       // Red
        colors_[2] = {0, 1, 0, .5, 0, 1, 0};       // Green
        colors_[3] = {0, 0, 1, .5, 0, 0, 1};       // Blue
        colors_[4] = {1, 1, 0, .5, 1, 1, 0};       // Yellow
        colors_[5] = {1, 0, 1, .5, 1, 0, 1};       // Magenta
        colors_[6] = {0, 1, 1, .5, 0, 1, 1};       // Cyan
        colors_[7] = {1, 1, 1, .5, 1, 1, 1};       // White
        
        currentColor_ = colors_[0];
    }
    
    ~PaintApp() {
        delete renderer_;
        delete canvas_;
    }
    
    bool init() {
        // Load config
        renderer_->loadConfig("cfg.txt");
        
        if (!renderer_->init()) {
            std::cerr << "Renderer init failed\n";
            return false;
        }
        
        renderer_->setCanvas(canvas_);
        
        // Setup callbacks
        renderer_->onMouseMove = [this](double x, double y) {
            onMouseMove(x, y);
        };
        
        renderer_->onMouseButton = [this](int button, int action, int mods) {
            onMouseButton(button, action, mods);
        };
        
        renderer_->onKey = [this](int key, int sc, int action, int mods) {
            onKey(key, sc, action, mods);
        };
        
        return true;
    }
    
    void run() {
        std::cout << "Easy Renderer Paint App\n";
        std::cout << "===============\n";
        std::cout << "Controls:\n";
        std::cout << "  Left Mouse  - Draw\n";
        std::cout << "  Right Mouse - Erase\n";
        std::cout << "  1-8        - Select color\n";
        std::cout << "  B          - Brush tool\n";
        std::cout << "  L          - Line tool\n";
        std::cout << "  R          - Rectangle tool\n";
        std::cout << "  E          - Eraser (fill with white)\n";
        std::cout << "  C          - Clear canvas\n";
        std::cout << "  Ctrl+S     - Screenshot\n";
        std::cout << "  Ctrl+N     - Reset camera\n";
        std::cout << "  Arrow Keys - Pan\n";
        std::cout << "  +/-        - Zoom\n";
        std::cout << "  ESC        - Exit\n";
        
        while (!renderer_->shouldClose()) {
            renderFrame();
        }
    }
    
private:
    enum class Tool { Brush, Line, Rect, Eraser };
    
    void onMouseMove(double x, double y) {
        if (renderer_->isMouseLeftDown() || renderer_->isMouseRightDown()) {
            int cx, cy;
            canvas_->screenToCanvas(x, y, cx, cy, renderer_->getCamera(), 
                              renderer_->getWindowWidth(), renderer_->getWindowHeight());
            
            int pcx, pcy;
            canvas_->screenToCanvas(renderer_->getMouseX(), renderer_->getMouseY(), pcx, pcy, 
                          renderer_->getCamera(), renderer_->getWindowWidth(), renderer_->getWindowHeight());
            
            Pixel p = currentColor_;
            
            // Apply tool
            switch (currentTool_) {
            case Tool::Brush:
            case Tool::Eraser:
                canvas_->drawLine(pcx, pcy, cx, cy, p);
                break;
            case Tool::Line:
            case Tool::Rect:
                // Just track position for preview, actual draw happens on mouse up
                break;
            }
        }
        
        // Update overlay text
        int cx, cy;
        canvas_->screenToCanvas(x, y, cx, cy, renderer_->getCamera(),
                            renderer_->getWindowWidth(), renderer_->getWindowHeight());
        
        char buf[128];
        snprintf(buf, sizeof(buf), "(%d, %d) | Tool: %s | Color: %d",
                cx, cy, toolName(), currentColorIndex_ + 1);
        renderer_->setOverlayText(buf);
    }
    
    void onMouseButton(int button, int action, int mods) {
        if (action == GLFW_PRESS) {
            if (button == GLFW_MOUSE_BUTTON_LEFT) {
                int cx, cy;
                canvas_->screenToCanvas(renderer_->getMouseX(), renderer_->getMouseY(), 
                                  cx, cy, renderer_->getCamera(),
                                  renderer_->getWindowWidth(), renderer_->getWindowHeight());
                
                // Start point for Line/Rect tools
                if (currentTool_ == Tool::Line || currentTool_ == Tool::Rect) {
                    startPoint_ = {cx, cy};
                } else {
                    Pixel p = currentColor_;
                    canvas_->setPixel(cx, cy, p);
                }
            }
        } else if (action == GLFW_RELEASE) {
            if (button == GLFW_MOUSE_BUTTON_LEFT) {
                if (currentTool_ == Tool::Line || currentTool_ == Tool::Rect) {
                    int cx, cy;
                    canvas_->screenToCanvas(renderer_->getMouseX(), renderer_->getMouseY(),
                                      cx, cy, renderer_->getCamera(),
                                      renderer_->getWindowWidth(), renderer_->getWindowHeight());
                    
                    Pixel p{currentColor_.r, currentColor_.g, currentColor_.b, renderer_->getWireAlpha(),
                           currentColor_.lr, currentColor_.lg, currentColor_.lb};
                    
                    if (currentTool_ == Tool::Line) {
                        canvas_->drawLine(startPoint_.first, startPoint_.second, cx, cy, p);
                    } else if (currentTool_ == Tool::Rect) {
                        canvas_->drawRect(startPoint_.first, startPoint_.second, cx, cy, p);
                    }
                }
            }
        }
    }
    
    void onKey(int key, int sc, int action, int mods) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
        
        if (mods & GLFW_MOD_CONTROL) {
            switch (key) {
            case GLFW_KEY_S:
                renderer_->screenshot();
                break;
            case GLFW_KEY_0:
                renderer_->resetCamera();
                break;
            }
        } else {
            switch (key) {
            case GLFW_KEY_ESCAPE:
                // Close window
                break;
            case GLFW_KEY_LEFT:
                renderer_->pan(-0.1, 0);
                break;
            case GLFW_KEY_RIGHT:
                renderer_->pan(0.1, 0);
                break;
            case GLFW_KEY_UP:
                renderer_->pan(0, 0.1);
                break;
            case GLFW_KEY_DOWN:
                renderer_->pan(0, -0.1);
                break;
            case GLFW_KEY_EQUAL:  // + key
            case GLFW_KEY_KP_ADD:
                renderer_->zoom(1.1);
                break;
            case GLFW_KEY_MINUS:
            case GLFW_KEY_KP_SUBTRACT:
                renderer_->zoom(0.9);
                break;
            case GLFW_KEY_1: case GLFW_KEY_2: case GLFW_KEY_3: case GLFW_KEY_4:
            case GLFW_KEY_5: case GLFW_KEY_6: case GLFW_KEY_7: case GLFW_KEY_8:
                selectColor(key - GLFW_KEY_1);
                break;
            case GLFW_KEY_B:
                currentTool_ = Tool::Brush;
                std::cout << "Tool: Brush\n";
                break;
            case GLFW_KEY_L:
                currentTool_ = Tool::Line;
                std::cout << "Tool: Line\n";
                break;
            case GLFW_KEY_R:
                currentTool_ = Tool::Rect;
                std::cout << "Tool: Rectangle\n";
                break;
            case GLFW_KEY_E:
                currentTool_ = Tool::Eraser;
                std::cout << "Tool: Eraser\n";
                break;
            case GLFW_KEY_C:
                canvas_->clear(1, 1, 1, 1);
                std::cout << "Canvas cleared\n";
                break;
            }
        }
    }
    
    void renderFrame() {
        renderer_->pollEvents();
        renderer_->render();
    }
    
    void selectColor(int index) {
        if (index >= 0 && index < 8) {
            currentColorIndex_ = index;
            currentColor_ = colors_[index];
            std::cout << "Color: " << (index + 1) << "\n";
        }
    }
    
    const char* toolName() const {
        switch (currentTool_) {
        case Tool::Brush: return "Brush";
        case Tool::Line: return "Line";
        case Tool::Rect: return "Rect";
        case Tool::Eraser: return "Eraser";
        default: return "Unknown";
        }
    }
    
    Canvas* canvas_ = nullptr;
    Renderer* renderer_ = nullptr;
    
    Tool currentTool_;
    Pixel currentColor_;
    int currentColorIndex_;
    Pixel colors_[8];
    
    std::pair<int, int> startPoint_;
};

int main() {
    SetProcessDPIAware();
    
    PaintApp app;
    if (!app.init()) {
        std::cerr << "App initialization failed\n";
        return 1;
    }
    
    app.run();
    
    return 0;
}
