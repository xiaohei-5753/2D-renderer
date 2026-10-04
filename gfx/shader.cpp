// gfx/shader.cpp
#include "shader.h"

#include <cstdio>

namespace pb::gfx {

namespace {
const char* glTypeName(GLenum t) {
    switch (t) {
        case GL_VERTEX_SHADER: return "vertex";
        case GL_FRAGMENT_SHADER: return "fragment";
        case GL_COMPUTE_SHADER: return "compute";
        default: return "shader";
    }
}
}  // namespace

GLuint compileShader(GLenum type, const char* src, const char* label) {
    const GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
        log[n < 0 ? 0 : (n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1)] = '\0';
        fprintf(stderr, "[gl] %s %s shader compile failed:\n%s\n", label,
                glTypeName(type), log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint createComputeProgram(const char* label, const char* src) {
    const GLuint cs = compileShader(GL_COMPUTE_SHADER, src, label);
    if (!cs) return 0;

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, cs);
    glLinkProgram(prog);
    glDeleteShader(cs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[8192];
        GLsizei n = 0;
        glGetProgramInfoLog(prog, sizeof(log) - 1, &n, log);
        log[n < 0 ? 0 : (n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1)] = '\0';
        fprintf(stderr, "[gl] %s compute program link failed:\n%s\n", label, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

GLuint createGraphicsProgram(const char* label, const char* vsSrc, const char* fsSrc) {
    const GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc, label);
    if (!vs) return 0;
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc, label);
    if (!fs) { glDeleteShader(vs); return 0; }

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[8192];
        GLsizei n = 0;
        glGetProgramInfoLog(prog, sizeof(log) - 1, &n, log);
        log[n < 0 ? 0 : (n < (GLsizei)sizeof(log) ? n : (GLsizei)sizeof(log) - 1)] = '\0';
        fprintf(stderr, "[gl] %s program link failed:\n%s\n", label, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool checkGLError(const char* label) {
    bool any = false;
    for (int guard = 0; guard < 16; guard++) {
        const GLenum e = glGetError();
        if (e == GL_NO_ERROR) break;
        any = true;
        fprintf(stderr, "[gl] %s: error 0x%04X\n", label, e);
    }
    return any;
}

}  // namespace pb::gfx
