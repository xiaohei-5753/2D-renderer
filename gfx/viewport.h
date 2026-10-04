// gfx/viewport.h — 相机、裁剪适配与显示
//
// 从原 renderer.cpp 的 viewScaleUV() / display() / scr2mem() 抽出。
//
// 适配模式是 CROP（cover，不是 letterbox）：画布放大到铺满整个窗口，溢出的轴被裁掉。
// 这样可以保证像素永远是方的，不会因为窗口比例而拉伸。
// 显示与"屏幕坐标 -> 画布坐标"共用同一套 uv 计算，所以看到的位置和画到的位置始终一致。
#pragma once

#include "gl_common.h"

namespace pb::gfx {

struct Camera {
    double x = 0.5, y = 0.5;   // 视野中心（画布 UV，0..1）
    double zoom = 1.0;         // 可见比例：1.0 = 恰好铺满；越大越缩小（视野越广）
};

class Viewport {
public:
    Viewport() = default;
    ~Viewport();

    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;

    // 需要已有一个 current 的 GL 上下文
    bool init();
    void shutdown();
    bool valid() const { return prog_ != 0; }

    void setCanvasSize(int w, int h);
    int canvasWidth() const { return cw_; }
    int canvasHeight() const { return ch_; }

    Camera& camera() { return cam_; }
    const Camera& camera() const { return cam_; }
    void resetCamera() { cam_ = Camera(); }

    void panByUV(double du, double dv) { cam_.x += du; cam_.y += dv; }
    void zoomBy(double factor);

    // 当前视野在画布 UV 空间的尺寸（du,dv）。zoom==1 时恰好铺满一个轴。
    void viewUV(int fbW, int fbH, float& du, float& dv) const;

    // 屏幕坐标（窗口坐标，含 DPI）-> 画布像素坐标。结果一定在画布范围内。
    //
    // 光标位置和窗口尺寸都是"屏幕坐标"，而视口用的是帧缓冲像素；用窗口尺寸做归一化
    // 正好抵消两者之间的 DPI 缩放（原代码在这里混用过两种坐标，导致 125%/150% 缩放下画歪）。
    void screenToCanvas(double sx, double sy, int winW, int winH, int fbW, int fbH,
                        int& cx, int& cy) const;

    // 画布像素坐标 -> 帧缓冲像素坐标（HUD 叠加绘制用）。与 screenToCanvas 互为逆运算。
    void canvasToFramebuffer(double cx, double cy, int fbW, int fbH, double& fx,
                             double& fy) const;

    // 同 screenToCanvas，但不做边界夹取。
    // 缩放锚定光标时需要真实的（可能越界的）画布坐标，夹取会让画面在边缘漂移。
    void screenToCanvasF(double sx, double sy, int winW, int winH, int fbW, int fbH,
                         double& cx, double& cy) const;

    // 一个画布像素在帧缓冲上占多少像素（笔刷光标大小、命中判定都要用）
    double pixelsPerCell(int fbW, int fbH) const;

    // 把纹理画满整个帧缓冲
    void draw(GLuint texture, int fbW, int fbH);

private:
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLint uTex_ = -1, uCenter_ = -1, uViewUV_ = -1;
    int cw_ = 1, ch_ = 1;
    Camera cam_;
};

}  // namespace pb::gfx
