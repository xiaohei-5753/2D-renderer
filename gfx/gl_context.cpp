// gfx/gl_context.cpp
#include "gl_context.h"

#include "gl_common.h"

#include <GLFW/glfw3.h>

#include <cstdio>

namespace pb::gfx {

namespace {
int g_glfwRefCount = 0;

void glfwErrorCallback(int code, const char* desc) {
    fprintf(stderr, "[glfw] error %d: %s\n", code, desc ? desc : "(null)");
}
}  // namespace

GLContext::~GLContext() { destroy(); }

bool GLContext::create(const ContextDesc& desc) {
    if (window_) {
        fprintf(stderr, "[gl] context already created\n");
        return false;
    }

    glfwSetErrorCallback(glfwErrorCallback);
    if (g_glfwRefCount == 0) {
        if (!glfwInit()) {
            fprintf(stderr, "[gl] glfwInit() failed\n");
            return false;
        }
    }
    g_glfwRefCount++;
    ownsGlfw_ = true;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, desc.glMajor);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, desc.glMinor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, desc.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

    window_ = glfwCreateWindow(desc.width, desc.height, desc.title, nullptr, nullptr);
    if (!window_) {
        fprintf(stderr, "[gl] failed to create a %d.%d core profile window\n",
                desc.glMajor, desc.glMinor);
        destroy();
        return false;
    }

    makeCurrent();

    glewExperimental = GL_TRUE;
    const GLenum err = glewInit();
    if (err != GLEW_OK) {
        fprintf(stderr, "[gl] glewInit() failed: %s\n", glewGetErrorString(err));
        destroy();
        return false;
    }
    // glewInit 在 core profile 下必定留下一个 GL_INVALID_ENUM，这里把它吃掉，
    // 免得污染后面第一次真正的错误检查
    while (glGetError() != GL_NO_ERROR) {}

    const GLubyte* ver = glGetString(GL_VERSION);
    const GLubyte* ren = glGetString(GL_RENDERER);
    glVersion_ = ver ? reinterpret_cast<const char*>(ver) : "?";
    glRenderer_ = ren ? reinterpret_cast<const char*>(ren) : "?";

    printf("[gl] version : %s\n", glVersion_.c_str());
    printf("[gl] renderer: %s\n", glRenderer_.c_str());
    return true;
}

void GLContext::destroy() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    if (ownsGlfw_) {
        ownsGlfw_ = false;
        if (--g_glfwRefCount <= 0) {
            g_glfwRefCount = 0;
            glfwTerminate();
        }
    }
}

void GLContext::makeCurrent() {
    if (window_) glfwMakeContextCurrent(window_);
}

void GLContext::swapBuffers() {
    if (window_) glfwSwapBuffers(window_);
}

void GLContext::pollEvents() { glfwPollEvents(); }

bool GLContext::shouldClose() const {
    return window_ ? glfwWindowShouldClose(window_) != 0 : true;
}

void GLContext::requestClose() {
    if (window_) glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void GLContext::framebufferSize(int& w, int& h) const {
    w = h = 0;
    if (window_) glfwGetFramebufferSize(window_, &w, &h);
}

void GLContext::windowSize(int& w, int& h) const {
    w = h = 0;
    if (window_) glfwGetWindowSize(window_, &w, &h);
}

void GLContext::setTitle(const std::string& t) {
    if (window_) glfwSetWindowTitle(window_, t.c_str());
}

}  // namespace pb::gfx
