// gfx/gl_timer.h — GPU 侧计时
//
// 为什么需要它：OpenGL 的命令是**异步**提交的。在 CPU 侧用时间戳包住 sweep.render()
// 量到的只是"把命令塞进队列"的耗时，真正的 GPU 工作还没开始——实测会稳定显示 0.0 ms，
// 而实际光照要 4 ms 左右。用 glFinish() 能测准，但会把管线打断、明显拖慢帧率。
//
// 计时器查询（GL_TIME_ELAPSED）两者兼得：本帧发起查询，下一帧再取结果，全程不阻塞。
#pragma once

#include "gl_common.h"

namespace pb::gfx {

class GpuTimer {
public:
    GpuTimer() = default;
    ~GpuTimer();

    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    bool init();
    void shutdown();
    bool valid() const { return queries_[0] != 0; }

    void begin();
    void end();

    // 取已完成的上一轮结果。有新结果时写入 msOut 并返回 true。
    // 需要每帧调用（否则结果永远不会被消费）。
    bool poll(float& msOut);

    float lastMs() const { return lastMs_; }

private:
    GLuint queries_[2] = {0, 0};
    int writeIdx_ = 0;
    bool pending_[2] = {false, false};
    float lastMs_ = 0.0f;
};

}  // namespace pb::gfx
