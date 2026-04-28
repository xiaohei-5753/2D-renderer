#define _CRT_SECURE_NO_WARNINGS
#define GLFW_EXPOSE_NATIVE_WIN32
#include "easy_renderer.h"
#include <cstdio>
#include <iostream>
#include <string>
#include <algorithm>
#include <windows.h>
#include <GLFW/glfw3native.h>

using namespace easy_renderer;

// ============================================================================
// Drawing helpers (moved from library to application layer)
// ============================================================================

static void drawLine(Canvas* canvas, int x1, int y1, int x2, int y2, const Pixel& p) {
    int dx = abs(x2 - x1), dy = -abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1, sy = (y1 < y2) ? 1 : -1;
    int err = dx + dy;
    while (true) {
        canvas->setPixel(x1, y1, p);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { if (x1 == x2) break; err += dy; x1 += sx; }
        if (e2 <= dx) { if (y1 == y2) break; err += dx; y1 += sy; }
    }
}

static void drawRect(Canvas* canvas, int x1, int y1, int x2, int y2, const Pixel& p) {
    int cx1 = std::max(0, std::min(x1, x2));
    int cy1 = std::max(0, std::min(y1, y2));
    int cx2 = std::min(canvas->width() - 1, std::max(x1, x2));
    int cy2 = std::min(canvas->height() - 1, std::max(y1, y2));
    for (int x = cx1; x <= cx2; x++) { canvas->setPixel(x, cy1, p); canvas->setPixel(x, cy2, p); }
    for (int y = cy1; y <= cy2; y++) { canvas->setPixel(cx1, y, p); canvas->setPixel(cx2, y, p); }
}

static void screenToCanvas(double sx, double sy, int& cx, int& cy,
                           const Camera& cam, int winW, int winH) {
    double u = sx / winW, v = 1.0 - sy / winH;
    double abx = cam.scale * (u - 0.5) + cam.x;
    double aby = cam.scale * (v - 0.5) + cam.y;
    // canvas size used in coordinate mapping — we'll pass it separately
    // For now assume 512x512 default, will be refined.
}

// Convert screen coords to canvas pixel coords
static void scr2cvs(double sx, double sy, int& cx, int& cy,
                    const Camera& cam, int winW, int winH,
                    int cvsW, int cvsH) {
    double u = sx / winW, v = 1.0 - sy / winH;
    double abx = cam.scale * (u - 0.5) + cam.x;
    double aby = cam.scale * (v - 0.5) + cam.y;
    cx = (int)(abx * cvsW);
    cy = (int)(aby * cvsH);
    if (cx < 0) cx = 0; if (cx >= cvsW) cx = cvsW - 1;
    if (cy < 0) cy = 0; if (cy >= cvsH) cy = cvsH - 1;
}

// ============================================================================
// GDI text overlay (moved from library to application)
// ============================================================================

class TextOverlay {
public:
    GLuint vao = 0;

    ~TextOverlay() {
        if (tex) glDeleteTextures(1, &tex);
        if (prog) glDeleteProgram(prog);
    }

    void setText(const std::string& t) {
        if (t != textCache) {
            textCache = t;
            rebuild();
        }
    }

    void draw(int winW, int winH) {
        if (textCache.empty() || tex == 0) return;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        if (prog == 0) {
            const char* vsSrc = R"(#version 330 core
                layout(location=0)in vec2 aP;layout(location=1)in vec2 aU;
                out vec2 vU;void main(){gl_Position=vec4(aP,0,1);vU=aU;})";
            const char* fsSrc = R"(#version 330 core
                uniform sampler2D u_t;uniform vec2 u_tl;uniform vec2 u_size;
                uniform vec2 u_win;uniform vec3 u_color;
                in vec2 vU;out vec4 oC;
                void main(){
                    vec2 px = vU * u_win;
                    vec2 pos = px - u_tl;
                    if(pos.x>=0&&pos.x<u_size.x&&pos.y>=0&&pos.y<u_size.y){
                        vec2 tc=vec2(pos.x/u_size.x,1-pos.y/u_size.y);
                        float a=texture(u_t,tc).a;
                        if(a<0.005)discard;
                        oC=vec4(u_color,a);
                    } else discard;
                })";
            GLuint vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vs, 1, &vsSrc, nullptr); glCompileShader(vs);
            GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(fs, 1, &fsSrc, nullptr); glCompileShader(fs);
            prog = glCreateProgram();
            glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
            glDeleteShader(vs); glDeleteShader(fs);
        }

        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(prog, "u_t"), 0);
        glUniform2f(glGetUniformLocation(prog, "u_tl"), 10.0f, 10.0f);
        glUniform2f(glGetUniformLocation(prog, "u_size"), (float)texW, (float)texH);
        glUniform2f(glGetUniformLocation(prog, "u_win"), (float)winW, (float)winH);
        glUniform3f(glGetUniformLocation(prog, "u_color"), 1, 1, 1);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glDisable(GL_BLEND);
    }

private:
    GLuint tex = 0, prog = 0;
    int texW = 0, texH = 0;
    std::string textCache;

    void rebuild() {
        HDC memDC = CreateCompatibleDC(nullptr);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 255));
        HFONT hFont = CreateFontA(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FIXED_PITCH, "Consolas");
        SelectObject(memDC, hFont);
        SIZE sz = {};
        GetTextExtentPoint32A(memDC, textCache.c_str(), (int)textCache.size(), &sz);
        texW = ((sz.cx + 15) / 16) * 16 + 8; texH = sz.cy + 6;
        if (texW < 1) texW = 1; if (texH < 1) texH = 1;
        HBITMAP hBmp = CreateCompatibleBitmap(memDC, texW, texH);
        SelectObject(memDC, hBmp);
        RECT rc = {0, 0, texW, texH};
        HBRUSH hBr = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &rc, hBr); DeleteObject(hBr);
        DrawTextA(memDC, textCache.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
        std::vector<unsigned char> pixels((size_t)texW * texH * 4, 0);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = texW; bmi.bmiHeader.biHeight = -texH;
        bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(memDC, hBmp, 0, texH, pixels.data(), &bmi, DIB_RGB_COLORS);
        for (int i = 0; i < texW * texH; i++) {
            unsigned char a = (unsigned char)(0.299f * pixels[i*4+2] + 0.587f * pixels[i*4+1] + 0.114f * pixels[i*4+0]);
            pixels[i*4+0] = 255; pixels[i*4+1] = 255; pixels[i*4+2] = 255; pixels[i*4+3] = a;
        }
        if (tex == 0) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        DeleteObject(hBmp); DeleteObject(hFont); DeleteDC(memDC);
    }
};

// ============================================================================
// Screenshot helper
// ============================================================================

static void screenshot(int winW, int winH) {
    static int scrCnt = 0;
    scrCnt++;
    std::string fn = std::to_string(scrCnt) + "_screenshot.tga";
    std::vector<unsigned char> buf(winW * winH * 3);
    glReadPixels(0, 0, winW, winH, GL_BGR, GL_UNSIGNED_BYTE, buf.data());
    FILE* f = fopen(fn.c_str(), "wb");
    if (f) {
        unsigned char hdr[18] = {0};
        hdr[2] = 2; hdr[12] = winW & 255; hdr[13] = winW >> 8;
        hdr[14] = winH & 255; hdr[15] = winH >> 8; hdr[16] = 24; hdr[17] = 0x20;
        fwrite(hdr, 1, 18, f);
        fwrite(buf.data(), 1, buf.size(), f);
        fclose(f);
        printf("Screenshot: %s\n", fn.c_str());
    }
}

// ============================================================================
// Paint Application
// ============================================================================

class PaintApp {
public:
    PaintApp()
        : canvas_(nullptr), renderer_(nullptr)
        , currentTool_(Tool::Brush), currentColorIndex_(0) {

        canvas_   = new Canvas(512, 512);
        renderer_ = new Renderer(512, 512, 1280, 1280);

        colors_[0] = {0, 0, 0, 1, 0,   0,   0  };  // Black
        colors_[1] = {1, 0, 0, 1, 1,   0,   0  };  // Red
        colors_[2] = {0, 1, 0, 1, 0,   1,   0  };  // Green
        colors_[3] = {0, 0, 1, 1, 0,   0,   1  };  // Blue
        colors_[4] = {1, 1, 0, 1, 1,   1,   0  };  // Yellow
        colors_[5] = {1, 0, 1, 1, 1,   0,   1  };  // Magenta
        colors_[6] = {0, 1, 1, 1, 0,   1,   1  };  // Cyan
        colors_[7] = {1, 1, 1, 1, 1,   1,   1  };  // White

        currentColor_ = colors_[0];
    }

    ~PaintApp() {
        delete renderer_;
        delete canvas_;
    }

    bool init() {
        renderer_->loadConfig("cfg.txt");

        if (!renderer_->init()) {
            std::cerr << "Renderer init failed\n";
            return false;
        }
        renderer_->setCanvas(canvas_);

        // Build overlay VAO
        float verts[] = { -1,-1,0,0, 3,-1,2,0, -1,3,0,2 };
        glGenVertexArrays(1, &overlayVAO_);
        glBindVertexArray(overlayVAO_);
        GLuint vbo; glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
        glBindVertexArray(0);
        overlay_.vao = overlayVAO_;

        // Wire callback hooks
        renderer_->onMouseMove = [this](double x, double y) { onMouseMove(x, y); };
        renderer_->onMouseButton = [this](int btn, int act, int mods) { onMouseButton(btn, act, mods); };
        renderer_->onKey = [this](int k, int sc, int act, int mods) { onKey(k, sc, act, mods); };

        return true;
    }

    void run() {
        std::cout << "Easy Renderer Paint App\n"
                  << "=======================\n"
                  << "  Left Mouse  - Draw\n"
                  << "  Right Mouse - Erase\n"
                  << "  1-8        - Select color\n"
                  << "  B          - Brush\n"
                  << "  L          - Line\n"
                  << "  R          - Rectangle\n"
                  << "  E          - Eraser\n"
                  << "  C          - Clear\n"
                  << "  Ctrl+S     - Screenshot\n"
                  << "  Ctrl+N     - Reset camera\n"
                  << "  Arrows     - Pan\n"
                  << "  +/-        - Zoom\n";

        double lastFrame = 0;
        while (!renderer_->shouldClose()) {
            renderer_->pollEvents();

            // Update overlay text
            int cx, cy;
            scr2cvs(mouseX_, mouseY_, cx, cy, renderer_->getCamera(),
                    renderer_->getWindowWidth(), renderer_->getWindowHeight(),
                    canvas_->width(), canvas_->height());
            char buf[128];
            snprintf(buf, sizeof(buf), "(%d,%d) %s C%d FPS:%.0f",
                     cx, cy, toolName(), currentColorIndex_ + 1, fps_);
            overlay_.setText(buf);

            // Render
            renderer_->render();

            // FPS calculation
            double now = glfwGetTime();
            if (now - lastFrame > 0) {
                fps_ = 0.9 * fps_ + 0.1 / (now - lastFrame);
            }
            lastFrame = now;

            // App-layer overlay
            overlay_.draw(renderer_->getWindowWidth(), renderer_->getWindowHeight());
            glfwSwapBuffers(glfwGetCurrentContext());
        }
    }

private:
    enum class Tool { Brush, Line, Rect, Eraser };

    // --- Mouse state (formerly in Renderer) ---
    double mouseX_ = 0, mouseY_ = 0;
    bool mouseLeft_ = false, mouseRight_ = false;

    // --- Wire alpha ---
    float wireAlpha_ = 0.5f;

    // --- FPS ---
    double fps_ = 0;

    // --- Overlay ---
    TextOverlay overlay_;
    GLuint overlayVAO_ = 0;

    void onMouseMove(double x, double y) {
        mouseX_ = x;
        mouseY_ = y;

        if (mouseLeft_ || mouseRight_) {
            int cx, cy, pcx, pcy;
            scr2cvs(x, y, cx, cy, renderer_->getCamera(),
                    renderer_->getWindowWidth(), renderer_->getWindowHeight(),
                    canvas_->width(), canvas_->height());
            scr2cvs(mouseX_, mouseY_, pcx, pcy, renderer_->getCamera(),
                    renderer_->getWindowWidth(), renderer_->getWindowHeight(),
                    canvas_->width(), canvas_->height());

            Pixel p = currentColor_;
            if (currentTool_ == Tool::Brush || currentTool_ == Tool::Eraser) {
                drawLine(canvas_, pcx, pcy, cx, cy, p);
            }
        }
    }

    void onMouseButton(int button, int action, int mods) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            mouseLeft_ = (action == GLFW_PRESS);
        }
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            mouseRight_ = (action == GLFW_PRESS);
        }

        if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT) {
            int cx, cy;
            scr2cvs(mouseX_, mouseY_, cx, cy, renderer_->getCamera(),
                    renderer_->getWindowWidth(), renderer_->getWindowHeight(),
                    canvas_->width(), canvas_->height());

            if (currentTool_ == Tool::Line || currentTool_ == Tool::Rect) {
                startPoint_ = {cx, cy};
            } else {
                canvas_->setPixel(cx, cy, currentColor_);
            }
        }

        if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_RIGHT) {
            int cx, cy;
            scr2cvs(mouseX_, mouseY_, cx, cy, renderer_->getCamera(),
                    renderer_->getWindowWidth(), renderer_->getWindowHeight(),
                    canvas_->width(), canvas_->height());
            canvas_->setPixel(cx, cy, Pixel{0, 0, 0, 0, 0, 0, 0});
        }

        if (action == GLFW_RELEASE && button == GLFW_MOUSE_BUTTON_LEFT) {
            if (currentTool_ == Tool::Line || currentTool_ == Tool::Rect) {
                int cx, cy;
                scr2cvs(mouseX_, mouseY_, cx, cy, renderer_->getCamera(),
                        renderer_->getWindowWidth(), renderer_->getWindowHeight(),
                        canvas_->width(), canvas_->height());

                Pixel p{currentColor_.r, currentColor_.g, currentColor_.b, wireAlpha_,
                        currentColor_.lr, currentColor_.lg, currentColor_.lb};

                if (currentTool_ == Tool::Line) {
                    drawLine(canvas_, startPoint_.first, startPoint_.second, cx, cy, p);
                } else if (currentTool_ == Tool::Rect) {
                    drawRect(canvas_, startPoint_.first, startPoint_.second, cx, cy, p);
                }
            }
        }
    }

    void onKey(int key, int sc, int action, int mods) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;

        auto& cam = const_cast<Camera&>(renderer_->getCamera());

        if (mods & GLFW_MOD_CONTROL) {
            switch (key) {
            case GLFW_KEY_S: screenshot(renderer_->getWindowWidth(), renderer_->getWindowHeight()); break;
            case GLFW_KEY_0: cam = Camera(); break;
            }
        } else {
            switch (key) {
            case GLFW_KEY_LEFT:  cam.x -= 0.1 / cam.scale; break;
            case GLFW_KEY_RIGHT: cam.x += 0.1 / cam.scale; break;
            case GLFW_KEY_UP:    cam.y += 0.1 / cam.scale; break;
            case GLFW_KEY_DOWN:  cam.y -= 0.1 / cam.scale; break;
            case GLFW_KEY_EQUAL: case GLFW_KEY_KP_ADD:
                cam.scale = std::min(cam.scale * 1.1, 10.0); break;
            case GLFW_KEY_MINUS: case GLFW_KEY_KP_SUBTRACT:
                cam.scale = std::max(cam.scale * 0.9, 0.1); break;
            case GLFW_KEY_1: case GLFW_KEY_2: case GLFW_KEY_3: case GLFW_KEY_4:
            case GLFW_KEY_5: case GLFW_KEY_6: case GLFW_KEY_7: case GLFW_KEY_8:
                selectColor(key - GLFW_KEY_1); break;
            case GLFW_KEY_B: currentTool_ = Tool::Brush;  std::cout << "Tool: Brush\n"; break;
            case GLFW_KEY_L: currentTool_ = Tool::Line;   std::cout << "Tool: Line\n"; break;
            case GLFW_KEY_R: currentTool_ = Tool::Rect;   std::cout << "Tool: Rectangle\n"; break;
            case GLFW_KEY_E: currentTool_ = Tool::Eraser; std::cout << "Tool: Eraser\n"; break;
            case GLFW_KEY_C: canvas_->clear(1, 1, 1, 1);  std::cout << "Canvas cleared\n"; break;
            }
        }
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
        case Tool::Brush: return "Brush"; case Tool::Line: return "Line";
        case Tool::Rect: return "Rect";   case Tool::Eraser: return "Eraser";
        default: return "?";
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

// ============================================================================

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
