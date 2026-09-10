#ifndef SDL_DISPLAY_H
#define SDL_DISPLAY_H

#include <SDL2/SDL.h>

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>


/*
 * ============================================================
 * SDL显示控制结构
 * ============================================================
 */
typedef struct
{
    /*
     * SDL窗口
     */
    SDL_Window *window;


    /*
     * SDL渲染器
     */
    SDL_Renderer *renderer;


    /*
     * 视频Texture
     *
     * 第一帧AVFrame到来以后再创建。
     */
    SDL_Texture *texture;


    /*
     * 当前Texture分辨率
     */
    int texture_width;

    int texture_height;


    /*
     * 当前Texture对应的FFmpeg像素格式
     *
     * 例如：
     *
     * AV_PIX_FMT_YUV420P
     * AV_PIX_FMT_NV12
     */
    enum AVPixelFormat frame_format;


    /*
     * SDL Texture实际像素格式
     *
     * 例如：
     *
     * SDL_PIXELFORMAT_IYUV
     * SDL_PIXELFORMAT_NV12
     */
    Uint32 texture_format;


    /*
     * 是否完成初始化
     */
    int initialized;

} SDL_Display;


/*
 * ============================================================
 * 初始化SDL显示模块
 *
 * title：
 *      窗口标题
 *
 * window_width：
 *      初始窗口宽度
 *
 * window_height：
 *      初始窗口高度
 *
 *
 * 注意：
 *
 * 这里只创建：
 *
 * SDL Window
 * SDL Renderer
 *
 * 不创建Texture。
 *
 * Texture会根据第一帧AVFrame动态创建。
 *
 *
 * 返回：
 *
 *      0   成功
 *     -1   失败
 * ============================================================
 */
int SDL_Display_Init(
    SDL_Display *display,
    const char *title,
    int window_width,
    int window_height
);


/*
 * ============================================================
 * 显示一个FFmpeg AVFrame
 *
 * 当前支持：
 *
 * AV_PIX_FMT_YUV420P
 * AV_PIX_FMT_YUVJ420P
 *
 * SDL2 >= 2.0.16时另外支持：
 *
 * AV_PIX_FMT_NV12
 * AV_PIX_FMT_NV21
 *
 *
 * 如果：
 *
 * 分辨率变化
 * 或
 * 像素格式变化
 *
 * 会自动销毁旧Texture并重新创建。
 *
 *
 * 返回：
 *
 *      0   显示成功
 *     -1   显示失败
 * ============================================================
 */
int SDL_Display_RenderFrame(
    SDL_Display *display,
    const AVFrame *frame
);


/*
 * ============================================================
 * SDL事件处理
 *
 * 检查：
 *
 * 关闭窗口
 * ESC
 *
 *
 * 返回：
 *
 *      1   用户要求退出
 *      0   继续运行
 *     -1   错误
 * ============================================================
 */
int SDL_Display_PollQuit(
    SDL_Display *display
);


/*
 * ============================================================
 * 销毁SDL显示模块
 * ============================================================
 */
void SDL_Display_Destroy(
    SDL_Display *display
);


#endif