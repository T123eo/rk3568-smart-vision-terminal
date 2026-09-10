#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>

#include "SDL_display.h"


/*
 * ============================================================
 * 根据FFmpeg AVPixelFormat
 *
 * 获取对应SDL PixelFormat。
 *
 *
 * 返回：
 *
 *      SDL PixelFormat
 *
 *      0 = 当前不支持
 * ============================================================
 */
static Uint32 get_sdl_pixel_format(
    enum AVPixelFormat format
)
{
    switch (format)
    {
        /*
         * ----------------------------------------
         * FFmpeg:
         *
         * Y
         * U
         * V
         *
         * 对应SDL IYUV：
         *
         * Y
         * U
         * V
         * ----------------------------------------
         */
        case AV_PIX_FMT_YUV420P:

        case AV_PIX_FMT_YUVJ420P:

            return SDL_PIXELFORMAT_IYUV;


        /*
         * ----------------------------------------
         * NV12
         *
         * Y plane
         * UV plane
         * ----------------------------------------
         */
#if SDL_VERSION_ATLEAST(2, 0, 16)

        case AV_PIX_FMT_NV12:

            return SDL_PIXELFORMAT_NV12;


        /*
         * ----------------------------------------
         * NV21
         *
         * Y plane
         * VU plane
         * ----------------------------------------
         */
        case AV_PIX_FMT_NV21:

            return SDL_PIXELFORMAT_NV21;

#endif


        default:

            return 0;
    }
}


/*
 * ============================================================
 * 创建 / 重新创建SDL Texture
 *
 * 当：
 *
 * 1. 第一帧到来
 * 2. 分辨率变化
 * 3. Pixel Format变化
 *
 * 调用。
 * ============================================================
 */
static int create_texture(
    SDL_Display *display,
    const AVFrame *frame
)
{
    if (display == NULL ||
        frame == NULL)
    {
        return -1;
    }


    /*
     * ========================================================
     * 1. 获取对应SDL像素格式
     * ========================================================
     */
    Uint32 sdl_format =
        get_sdl_pixel_format(
            (enum AVPixelFormat)frame->format
        );


    if (sdl_format == 0)
    {
        const char *format_name =
            av_get_pix_fmt_name(
                (enum AVPixelFormat)frame->format
            );


        fprintf(
            stderr,
            "Unsupported AVFrame pixel format: %s (%d)\n",
            format_name != NULL ?
                format_name :
                "unknown",
            frame->format
        );


        return -1;
    }


    /*
     * ========================================================
     * 2. 如果当前Texture已经匹配
     *
     * 就不需要重新创建。
     * ========================================================
     */
    if (display->texture != NULL &&
        display->texture_width ==
            frame->width &&
        display->texture_height ==
            frame->height &&
        display->frame_format ==
            (enum AVPixelFormat)frame->format)
    {
        return 0;
    }


    /*
     * ========================================================
     * 3. 如果已经存在旧Texture
     *
     * 先销毁。
     * ========================================================
     */
    if (display->texture != NULL)
    {
        SDL_DestroyTexture(
            display->texture
        );


        display->texture =
            NULL;
    }


    /*
     * ========================================================
     * 4. 创建新的Streaming Texture
     *
     * width / height：
     *
     * 直接使用FFmpeg实际解码出来的分辨率。
     *
     * 不写死1080x1920。
     * ========================================================
     */
    display->texture =
        SDL_CreateTexture(
            display->renderer,

            sdl_format,

            SDL_TEXTUREACCESS_STREAMING,

            frame->width,

            frame->height
        );


    if (display->texture == NULL)
    {
        fprintf(
            stderr,
            "SDL_CreateTexture failed: %s\n",
            SDL_GetError()
        );


        return -1;
    }


    /*
     * 记录当前Texture信息
     */
    display->texture_width =
        frame->width;


    display->texture_height =
        frame->height;


    display->frame_format =
        (enum AVPixelFormat)frame->format;


    display->texture_format =
        sdl_format;


    printf(
        "SDL Texture created: "
        "%dx%d "
        "AVFormat=%s "
        "SDLFormat=%s\n",

        frame->width,

        frame->height,

        av_get_pix_fmt_name(
            (enum AVPixelFormat)frame->format
        ),

        SDL_GetPixelFormatName(
            sdl_format
        )
    );


    return 0;
}


/*
 * ============================================================
 * 计算保持宽高比的显示区域
 *
 *
 * 例如：
 *
 * 视频：
 *
 *      1080 x 1920
 *
 * PC窗口：
 *
 *      1280 x 720
 *
 *
 * 不直接：
 *
 * SDL_RenderCopy(..., NULL, NULL)
 *
 * 否则可能把竖屏视频强制拉伸成横屏。
 *
 *
 * 最终：
 *
 * 保持视频比例
 * +
 * 居中显示
 * ============================================================
 */
static void calculate_display_rect(
    SDL_Display *display,
    SDL_Rect *dst_rect
)
{
    if (display == NULL ||
        dst_rect == NULL)
    {
        return;
    }


    /*
     * Renderer当前实际输出尺寸
     */
    int output_width = 0;

    int output_height = 0;


    if (SDL_GetRendererOutputSize(
            display->renderer,
            &output_width,
            &output_height) < 0)
    {
        /*
         * 获取失败时直接使用Texture尺寸。
         */
        output_width =
            display->texture_width;


        output_height =
            display->texture_height;
    }


    /*
     * 视频宽高比
     */
    double video_ratio =
        (double)display->texture_width /
        (double)display->texture_height;


    /*
     * 窗口宽高比
     */
    double window_ratio =
        (double)output_width /
        (double)output_height;


    /*
     * ========================================================
     * 窗口比视频更宽
     *
     * 例如：
     *
     * 视频：竖屏
     * 窗口：横屏
     *
     * 高度铺满
     * 左右留黑边
     * ========================================================
     */
    if (window_ratio >
        video_ratio)
    {
        dst_rect->h =
            output_height;


        dst_rect->w =
            (int)(
                output_height *
                video_ratio
            );


        dst_rect->x =
            (output_width -
             dst_rect->w) / 2;


        dst_rect->y =
            0;
    }


    /*
     * ========================================================
     * 窗口比视频更窄
     *
     * 宽度铺满
     * 上下留黑边
     * ========================================================
     */
    else
    {
        dst_rect->w =
            output_width;


        dst_rect->h =
            (int)(
                output_width /
                video_ratio
            );


        dst_rect->x =
            0;


        dst_rect->y =
            (output_height -
             dst_rect->h) / 2;
    }
}


/*
 * ============================================================
 * SDL_Display_Init
 * ============================================================
 */
int SDL_Display_Init(
    SDL_Display *display,
    const char *title,
    int window_width,
    int window_height
)
{
    if (display == NULL)
    {
        fprintf(
            stderr,
            "SDL_Display_Init: display is NULL\n"
        );


        return -1;
    }


    /*
     * ========================================================
     * 1. 清零结构体
     * ========================================================
     */
    memset(
        display,
        0,
        sizeof(*display)
    );


    /*
     * 防止非法窗口尺寸
     */
    if (window_width <= 0)
    {
        window_width =
            540;
    }


    if (window_height <= 0)
    {
        window_height =
            960;
    }


    if (title == NULL)
    {
        title =
            "RK3568 H264 Monitor";
    }


    /*
     * ========================================================
     * 2. SDL初始化
     *
     * SDL_INIT_VIDEO：
     *
     * 初始化视频系统。
     * ========================================================
     */
    if (SDL_Init(
            SDL_INIT_VIDEO) < 0)
    {
        fprintf(
            stderr,
            "SDL_Init failed: %s\n",
            SDL_GetError()
        );


        return -1;
    }


    /*
     * ========================================================
     * 3. 创建SDL Window
     *
     * SDL_WINDOW_RESIZABLE：
     *
     * 后续允许用户拖动调整窗口大小。
     * ========================================================
     */
    display->window =
        SDL_CreateWindow(
            title,

            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,

            window_width,
            window_height,

            SDL_WINDOW_SHOWN |
            SDL_WINDOW_RESIZABLE
        );


    if (display->window == NULL)
    {
        fprintf(
            stderr,
            "SDL_CreateWindow failed: %s\n",
            SDL_GetError()
        );


        SDL_Quit();


        return -1;
    }


    /*
     * ========================================================
     * 4. 创建Renderer
     *
     * 优先：
     *
     * 硬件加速
     * +
     * VSYNC
     * ========================================================
     */
    display->renderer =
        SDL_CreateRenderer(
            display->window,

            -1,

            SDL_RENDERER_ACCELERATED |
            SDL_RENDERER_PRESENTVSYNC
        );


    /*
     * --------------------------------------------------------
     * 某些Linux图形环境无法提供：
     *
     * accelerated + vsync
     *
     * 则退回普通Renderer。
     * --------------------------------------------------------
     */
    if (display->renderer == NULL)
    {
        fprintf(
            stderr,
            "Accelerated SDL Renderer failed: %s\n"
            "Trying fallback renderer...\n",
            SDL_GetError()
        );


        display->renderer =
            SDL_CreateRenderer(
                display->window,

                -1,

                0
            );
    }


    if (display->renderer == NULL)
    {
        fprintf(
            stderr,
            "SDL_CreateRenderer failed: %s\n",
            SDL_GetError()
        );


        SDL_DestroyWindow(
            display->window
        );


        display->window =
            NULL;


        SDL_Quit();


        return -1;
    }


    /*
     * ========================================================
     * 5. 设置默认清屏颜色
     *
     * 视频没有铺满窗口时，
     * 剩余区域显示黑色。
     * ========================================================
     */
    SDL_SetRenderDrawColor(
        display->renderer,

        0,
        0,
        0,
        255
    );


    /*
     * ========================================================
     * 6. Texture暂时不创建
     *
     * 等第一帧AVFrame到来以后，
     * 根据实际：
     *
     * width
     * height
     * format
     *
     * 动态创建。
     * ========================================================
     */
    display->texture =
        NULL;


    display->texture_width =
        0;


    display->texture_height =
        0;


    display->frame_format =
        AV_PIX_FMT_NONE;


    display->texture_format =
        0;


    display->initialized =
        1;


    printf(
        "SDL display initialized, "
        "window=%dx%d\n",

        window_width,

        window_height
    );


    return 0;
}


/*
 * ============================================================
 * SDL_Display_RenderFrame
 * ============================================================
 */
int SDL_Display_RenderFrame(
    SDL_Display *display,
    const AVFrame *frame
)
{
    if (display == NULL ||
        frame == NULL)
    {
        return -1;
    }


    if (!display->initialized ||
        display->renderer == NULL)
    {
        fprintf(
            stderr,
            "SDL display is not initialized\n"
        );


        return -1;
    }


    if (frame->width <= 0 ||
        frame->height <= 0)
    {
        fprintf(
            stderr,
            "Invalid AVFrame size: %dx%d\n",
            frame->width,
            frame->height
        );


        return -1;
    }


    /*
     * ========================================================
     * 1. 根据AVFrame检查/创建Texture
     * ========================================================
     */
    if (create_texture(
            display,
            frame) < 0)
    {
        return -1;
    }


    /*
     * ========================================================
     * 2. 根据像素格式更新Texture
     * ========================================================
     */
    int ret;


    switch (
        (enum AVPixelFormat)frame->format
    )
    {
        /*
         * ====================================================
         * YUV420P
         *
         * frame->data[0] = Y
         * frame->data[1] = U
         * frame->data[2] = V
         *
         * frame->linesize[]
         *
         * 就相当于你之前V4L2里的bytesperline。
         *
         * 一定不要用width代替linesize。
         * ====================================================
         */
        case AV_PIX_FMT_YUV420P:

        case AV_PIX_FMT_YUVJ420P:


            if (frame->data[0] == NULL ||
                frame->data[1] == NULL ||
                frame->data[2] == NULL)
            {
                fprintf(
                    stderr,
                    "Invalid YUV420P AVFrame data\n"
                );


                return -1;
            }


            ret =
                SDL_UpdateYUVTexture(
                    display->texture,

                    NULL,

                    /*
                     * Y
                     */
                    frame->data[0],

                    frame->linesize[0],

                    /*
                     * U
                     */
                    frame->data[1],

                    frame->linesize[1],

                    /*
                     * V
                     */
                    frame->data[2],

                    frame->linesize[2]
                );


            if (ret < 0)
            {
                fprintf(
                    stderr,
                    "SDL_UpdateYUVTexture failed: %s\n",
                    SDL_GetError()
                );


                return -1;
            }


            break;


#if SDL_VERSION_ATLEAST(2, 0, 16)

        /*
         * ====================================================
         * NV12
         *
         * data[0] = Y
         * data[1] = UV
         * ====================================================
         */
        case AV_PIX_FMT_NV12:


            if (frame->data[0] == NULL ||
                frame->data[1] == NULL)
            {
                fprintf(
                    stderr,
                    "Invalid NV12 AVFrame data\n"
                );


                return -1;
            }


            ret =
                SDL_UpdateNVTexture(
                    display->texture,

                    NULL,

                    frame->data[0],

                    frame->linesize[0],

                    frame->data[1],

                    frame->linesize[1]
                );


            if (ret < 0)
            {
                fprintf(
                    stderr,
                    "SDL_UpdateNVTexture failed: %s\n",
                    SDL_GetError()
                );


                return -1;
            }


            break;


        /*
         * ====================================================
         * NV21
         *
         * data[0] = Y
         * data[1] = VU
         * ====================================================
         */
        case AV_PIX_FMT_NV21:


            if (frame->data[0] == NULL ||
                frame->data[1] == NULL)
            {
                fprintf(
                    stderr,
                    "Invalid NV21 AVFrame data\n"
                );


                return -1;
            }


            ret =
                SDL_UpdateNVTexture(
                    display->texture,

                    NULL,

                    frame->data[0],

                    frame->linesize[0],

                    frame->data[1],

                    frame->linesize[1]
                );


            if (ret < 0)
            {
                fprintf(
                    stderr,
                    "SDL_UpdateNVTexture failed: %s\n",
                    SDL_GetError()
                );


                return -1;
            }


            break;

#endif


        default:
        {
            const char *format_name =
                av_get_pix_fmt_name(
                    (enum AVPixelFormat)frame->format
                );


            fprintf(
                stderr,
                "SDL cannot display AVFrame format: %s\n",
                format_name != NULL ?
                    format_name :
                    "unknown"
            );


            return -1;
        }
    }


    /*
     * ========================================================
     * 3. 清除上一帧
     * ========================================================
     */
    if (SDL_RenderClear(
            display->renderer) < 0)
    {
        fprintf(
            stderr,
            "SDL_RenderClear failed: %s\n",
            SDL_GetError()
        );


        return -1;
    }


    /*
     * ========================================================
     * 4. 计算保持比例的目标区域
     * ========================================================
     */
    SDL_Rect dst_rect;


    memset(
        &dst_rect,
        0,
        sizeof(dst_rect)
    );


    calculate_display_rect(
        display,
        &dst_rect
    );


    /*
     * ========================================================
     * 5. 将Texture绘制到窗口
     * ========================================================
     */
    if (SDL_RenderCopy(
            display->renderer,

            display->texture,

            NULL,

            &dst_rect) < 0)
    {
        fprintf(
            stderr,
            "SDL_RenderCopy failed: %s\n",
            SDL_GetError()
        );


        return -1;
    }


    /*
     * ========================================================
     * 6. 真正显示这一帧
     * ========================================================
     */
    SDL_RenderPresent(
        display->renderer
    );


    return 0;
}


/*
 * ============================================================
 * SDL_Display_PollQuit
 *
 * 必须由main线程频繁调用。
 * ============================================================
 */
int SDL_Display_PollQuit(
    SDL_Display *display
)
{
    if (display == NULL ||
        !display->initialized)
    {
        return -1;
    }


    SDL_Event event;


    /*
     * 把当前所有SDL事件处理掉。
     */
    while (
        SDL_PollEvent(
            &event
        )
    )
    {
        /*
         * 用户点击窗口关闭按钮
         */
        if (event.type ==
            SDL_QUIT)
        {
            return 1;
        }


        /*
         * 按ESC退出
         */
        if (event.type ==
            SDL_KEYDOWN)
        {
            if (
                event.key.keysym.sym ==
                SDLK_ESCAPE
            )
            {
                return 1;
            }
        }
    }


    return 0;
}


/*
 * ============================================================
 * SDL_Display_Destroy
 * ============================================================
 */
void SDL_Display_Destroy(
    SDL_Display *display
)
{
    if (display == NULL)
    {
        return;
    }


    /*
     * ========================================================
     * 1. Texture
     * ========================================================
     */
    if (display->texture != NULL)
    {
        SDL_DestroyTexture(
            display->texture
        );


        display->texture =
            NULL;
    }


    /*
     * ========================================================
     * 2. Renderer
     * ========================================================
     */
    if (display->renderer != NULL)
    {
        SDL_DestroyRenderer(
            display->renderer
        );


        display->renderer =
            NULL;
    }


    /*
     * ========================================================
     * 3. Window
     * ========================================================
     */
    if (display->window != NULL)
    {
        SDL_DestroyWindow(
            display->window
        );


        display->window =
            NULL;
    }


    /*
     * ========================================================
     * 4. SDL
     * ========================================================
     */
    if (display->initialized)
    {
        SDL_Quit();
    }


    display->texture_width =
        0;


    display->texture_height =
        0;


    display->frame_format =
        AV_PIX_FMT_NONE;


    display->texture_format =
        0;


    display->initialized =
        0;


    printf(
        "SDL display destroyed\n"
    );
}