// app/main.cpp — 画板应用（P0 阶段：最小可运行外壳）
//
// 当前只做三件事：开窗口、把示例场景过一遍光照、用相机显示出来。
// 交互式绘图工具、调色板、图层面板会在 P1/P3 接进来（Dear ImGui 已 vendor 到
// third_party/，届时在这里初始化）。
#include <cstdio>
#include <string>

#include "core/canvas.h"
#include "core/console.h"
#include "core/image.h"
#include "gfx/gl_context.h"
#include "gfx/light_sweep.h"
#include "gfx/viewport.h"

#include <GLFW/glfw3.h>

using namespace pb;

namespace {

// P0 的占位示例场景：一根霓虹管 + 一个实心块，用来确认光照管线在窗口里也正常
void buildDemo(Canvas& c) {
    c.clear(RGBA8{0, 0, 0, 0});
    const int W = c.width(), H = c.height();
    c.drawLine(40, H / 3, W - 40, H / 3, RGBA8{80, 200, 255, 90});
    c.drawLine(40, H * 2 / 3, W - 40, H * 2 / 3, RGBA8{255, 120, 90, 90});
    c.fillRect(W / 2 - 24, H / 2 - 24, W / 2 + 24, H / 2 + 24, RGBA8{40, 40, 48, 255});
}

double g_lastMouseX = 0.0, g_lastMouseY = 0.0;
bool g_dragging = false;

void mouseButtonCb(GLFWwindow* w, int button, int action, int) {
    if (button != GLFW_MOUSE_BUTTON_LEFT && button != GLFW_MOUSE_BUTTON_MIDDLE) return;
    if (action == GLFW_PRESS) {
        g_dragging = true;
        glfwGetCursorPos(w, &g_lastMouseX, &g_lastMouseY);
    } else if (action == GLFW_RELEASE) {
        g_dragging = false;
    }
}

}  // namespace

int main() {
    setupConsoleUtf8();
    constexpr int kCanvasW = 320;
    constexpr int kCanvasH = 180;

    Canvas canvas(kCanvasW, kCanvasH);
    buildDemo(canvas);

    gfx::ContextDesc desc;
    desc.width = 1280;
    desc.height = 720;
    desc.visible = true;
    desc.title = "paintboard (P0)";

    gfx::GLContext ctx;
    if (!ctx.create(desc)) {
        fprintf(stderr, "无法创建 OpenGL 上下文\n");
        return 1;
    }
    glfwSwapInterval(1);

    gfx::LightSweep sweep;
    if (!sweep.init(canvas.width(), canvas.height())) {
        fprintf(stderr, "光照管线初始化失败\n");
        return 1;
    }
    sweep.settings().rayRadius = 128;
    sweep.settings().ambient[0] = 0.10f;
    sweep.settings().ambient[1] = 0.10f;
    sweep.settings().ambient[2] = 0.12f;
    sweep.uploadAll(canvas.image());

    gfx::Viewport vp;
    if (!vp.init()) {
        fprintf(stderr, "视口初始化失败\n");
        return 1;
    }
    vp.setCanvasSize(canvas.width(), canvas.height());

    glfwSetMouseButtonCallback(ctx.window(), mouseButtonCb);

    printf("操作：左键拖拽平移 | 滚轮缩放 | 方向键平移 | +/- 缩放 | 0 复位 | ESC 退出\n");

    double lastTime = glfwGetTime();
    int frames = 0;

    while (!ctx.shouldClose()) {
        ctx.pollEvents();

        // --- 相机输入 ---
        GLFWwindow* w = ctx.window();
        if (g_dragging) {
            double mx = 0, my = 0;
            glfwGetCursorPos(w, &mx, &my);
            int winW = 0, winH = 0, fbW = 0, fbH = 0;
            ctx.windowSize(winW, winH);
            ctx.framebufferSize(fbW, fbH);
            if (winW > 0 && winH > 0) {
                float du = 1.0f, dv = 1.0f;
                vp.viewUV(fbW, fbH, du, dv);
                vp.panByUV(-(mx - g_lastMouseX) / winW * du,
                           (my - g_lastMouseY) / winH * dv);
            }
            g_lastMouseX = mx;
            g_lastMouseY = my;
        }
        if (glfwGetKey(w, GLFW_KEY_ESCAPE) == GLFW_PRESS) ctx.requestClose();
        if (glfwGetKey(w, GLFW_KEY_0) == GLFW_PRESS) vp.resetCamera();
        const double step = 0.01;
        if (glfwGetKey(w, GLFW_KEY_LEFT) == GLFW_PRESS) vp.panByUV(-step, 0.0);
        if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) vp.panByUV(step, 0.0);
        if (glfwGetKey(w, GLFW_KEY_UP) == GLFW_PRESS) vp.panByUV(0.0, step);
        if (glfwGetKey(w, GLFW_KEY_DOWN) == GLFW_PRESS) vp.panByUV(0.0, -step);
        if (glfwGetKey(w, GLFW_KEY_EQUAL) == GLFW_PRESS) vp.zoomBy(0.97);
        if (glfwGetKey(w, GLFW_KEY_MINUS) == GLFW_PRESS) vp.zoomBy(1.03);

        // --- 渲染 ---
        sweep.render();

        int fbW = 0, fbH = 0;
        ctx.framebufferSize(fbW, fbH);
        vp.draw(sweep.resultTexture(), fbW, fbH);
        ctx.swapBuffers();

        // --- FPS ---
        frames++;
        const double now = glfwGetTime();
        if (now - lastTime >= 1.0) {
            char title[192];
            snprintf(title, sizeof(title),
                     "paintboard (P0) | %dx%d | %d 方向 | %.0f fps", canvas.width(),
                     canvas.height(), sweep.directionCount(), frames / (now - lastTime));
            ctx.setTitle(title);
            frames = 0;
            lastTime = now;
        }
    }

    vp.shutdown();
    sweep.shutdown();
    ctx.destroy();
    return 0;
}
