// gfx/gl_timer.cpp
#include "gl_timer.h"

#include "shader.h"

namespace pb::gfx {

GpuTimer::~GpuTimer() { shutdown(); }

bool GpuTimer::init() {
    shutdown();
    glGenQueries(2, queries_);
    if (queries_[0] == 0 || queries_[1] == 0) {
        shutdown();
        return false;
    }
    return !checkGLError("gputimer/init");
}

void GpuTimer::shutdown() {
    if (queries_[0] || queries_[1]) glDeleteQueries(2, queries_);
    queries_[0] = queries_[1] = 0;
    writeIdx_ = 0;
    pending_[0] = pending_[1] = false;
    lastMs_ = 0.0f;
}

void GpuTimer::begin() {
    if (!valid()) return;
    glBeginQuery(GL_TIME_ELAPSED, queries_[writeIdx_]);
}

void GpuTimer::end() {
    if (!valid()) return;
    glEndQuery(GL_TIME_ELAPSED);
    pending_[writeIdx_] = true;
    writeIdx_ ^= 1;   // 下一个查询写到另一支，于是刚结束的那支可以被安全读取
}

bool GpuTimer::poll(float& msOut) {
    if (!valid()) return false;

    const int readIdx = writeIdx_ ^ 1;   // 上一次 end() 落到的那支
    if (!pending_[readIdx]) return false;

    GLint available = 0;
    glGetQueryObjectiv(queries_[readIdx], GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) return false;   // GPU 还没做完，下一帧再看

    GLuint64 ns = 0;
    glGetQueryObjectui64v(queries_[readIdx], GL_QUERY_RESULT, &ns);
    pending_[readIdx] = false;
    lastMs_ = (float)((double)ns / 1.0e6);
    msOut = lastMs_;
    return true;
}

}  // namespace pb::gfx
