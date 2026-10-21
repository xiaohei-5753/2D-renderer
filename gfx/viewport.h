// gfx/viewport.h — 相机、裁剪适配与画布显示
//
// 从原 renderer.cpp 的 viewScaleUV() / display() / scr2mem() 抽出并改造：
// 有 UI 之后画布不再铺满整个帧缓冲，而是画进中央面板给定的矩形。
//
// 适配模式是 CROP（cover，不是 letterbox）：画布放大到铺满绘制矩形，溢出的轴被裁掉。
// 这样可以保证像素永远是方的，不会因为矩形比例而拉伸。
// 显示与"屏幕坐标 -> 画布坐标"共用同一套 uv 计算，所以看到的位置和画到的位置始终一致。
//
// 坐标空间有三个，务必分清：
//   * 逻辑坐标：ImGui / 鼠标用的坐标（通常等于窗口客户区像素）
//   * 帧缓冲像素：glViewport / glScissor 用的坐标（HiDPI 下可能是逻辑坐标的 2 倍）
//   * 画布像素：文档的像素坐标
#pragma once

#include "gl_common.h"

namespace pb::gfx {

struct Camera {
    double x = 0.5, y = 0.5;   // 视野中心（画布 UV，0..1）
    double zoom = 1.0;         // 可见比例：1.0 = 恰好铺满；越大越缩小（视野越广）
};

// 适配模式。
//
// 原实现在窗口里只有 CROP（cover），因为那时画布铺满整个窗口、比例接近。
// 有了 UI 之后画布要画进一个又高又窄的面板，CROP 会把左右各裁掉一半——
// 用户看不全自己的画。所以画板默认改成 CONTAIN（完整看到画布，多余处留边）。
enum class FitMode {
    Contain,   // 完整显示整个画布（letterbox），默认
    Cover,     // 铺满并裁掉溢出部分（原行为）
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

    void setFitMode(FitMode m) { fit_ = m; }
    FitMode fitMode() const { return fit_; }

    void panByUV(double du, double dv) { cam_.x += du; cam_.y += dv; }
    void zoomBy(double factor);

    // 把缩放吸附到"一个画布像素 = 整数个屏幕像素"，并让画布居中。
    //
    // 像素画必须这样：非整数倍下 NEAREST 采样会让像素列宽忽大忽小（有的列占 2 个屏幕
    // 像素、有的占 3 个），表现为整片竖条纹。扫描 GI 输出平滑时看不出来，
    // 路径追踪带噪声时就很明显。
    void fitIntegerZoom();

    // --- 绘制目标 ---
    // 逻辑坐标（左上原点）的矩形：所有屏幕<->画布换算都以它为基准
    void setViewRect(double x, double y, double w, double h);
    // 帧缓冲像素 / 逻辑像素 的比例，以及帧缓冲高度（用于 y 轴翻转）
    void setFramebuffer(double scale, int height) {
        fbScale_ = (scale > 0.0) ? scale : 1.0;
        fbHeight_ = height;
    }

    double viewX() const { return vx_; }
    double viewY() const { return vy_; }
    double viewW() const { return vw_; }
    double viewH() const { return vh_; }
    bool viewRectValid() const { return vw_ > 1.0 && vh_ > 1.0; }

    // 当前视野在画布 UV 空间的尺寸（du,dv）。zoom==1 时恰好铺满一个轴。
    void viewUV(float& du, float& dv) const;
    // 一个画布像素在逻辑坐标下占多少像素
    double pixelsPerCell() const;

    // 逻辑屏幕坐标 -> 画布像素。夹取版结果一定在画布范围内。
    void screenToCanvasF(double sx, double sy, double& cx, double& cy) const;
    void screenToCanvas(double sx, double sy, int& cx, int& cy) const;

    // 画布像素 -> 绘制矩形内的归一化坐标 [0,1]（叠加层绘制用）
    void canvasToViewUV(double cx, double cy, double& u, double& v) const;

    // 把纹理画进绘制矩形（带 scissor，不会溢出到 UI 面板上）
    // flipY：把纹理上下翻转后再画。基准场景（scenes/bench_scene）是按"y=0 在顶部"写的，
    // 而本视口的画布坐标约定是 y 向上（screenToCanvasF 用 1-v），两者朝向相反，
    // 不翻转就会让同一个场景在 paintboard 与 bench_view 里一个正一个倒。
    void draw(GLuint texture, bool flipY = false);

    // 矩形的逻辑坐标 -> 帧缓冲像素
    void viewRectToFramebuffer(double& fx, double& fy, double& fw, double& fh) const;

private:
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLint uTex_ = -1, uCenter_ = -1, uViewUV_ = -1, uFlipY_ = -1;
    int cw_ = 1, ch_ = 1;
    Camera cam_;
    FitMode fit_ = FitMode::Contain;

    double vx_ = 0.0, vy_ = 0.0, vw_ = 1.0, vh_ = 1.0;   // 逻辑坐标
    double fbScale_ = 1.0;
    int fbHeight_ = 0;
};

}  // namespace pb::gfx
