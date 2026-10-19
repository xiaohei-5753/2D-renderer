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
    pending_[readIdx] = false;

    // 直接取结果，**不**先问 GL_QUERY_RESULT_AVAILABLE。
    //
    // 原因：在这台机器（NVIDIA 572.83 / GL 4.3）上实测
    // GL_QUERY_RESULT_AVAILABLE 永远返回 0，于是 poll() 永远返回 false，
    // 界面上"光照 xx ms"就一直显示 0.00 —— 而光照其实跑得好好的。
    // 我用 `valid=1 got=0` 的调试输出定位到了这一点。
    //
    // 直接取结果也不会阻塞：这是**双查询轮转**，刚 end() 的那支要等到下一次
    // end() 才会被读取，中间隔了整整一帧的 GPU 工作，正常情况下早已完成。
    // 即使偶发未完成，GL_QUERY_RESULT 的等待也是"等这个计数器"，代价可控。
    GLuint64 ns = 0;
    glGetQueryObjectui64v(queries_[readIdx], GL_QUERY_RESULT, &ns);
    lastMs_ = (float)((double)ns / 1.0e6);
    msOut = lastMs_;
    return true;
}

}  // namespace pb::gfx
