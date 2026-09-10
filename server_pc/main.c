#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "tcp_server.h"
#include "h264_packet_queue.h"
#include "h264_decoder.h"
#include "avframe_queue.h"
#include "SDL_display.h"


/*
 * ============================================================
 * TCP监听端口
 * ============================================================
 */
#define SERVER_PORT 8088


/*
 * ============================================================
 * H264压缩Packet Queue容量
 * ============================================================
 */
#define H264_QUEUE_CAPACITY 32


/*
 * ============================================================
 * AVFrame Queue容量
 * ============================================================
 */
#define AVFRAME_QUEUE_CAPACITY 8


/*
 * ============================================================
 * SDL窗口初始大小
 *
 * 当前摄像头1080x1920为竖屏，
 * 所以这里先使用540x960。
 *
 * 窗口本身支持Resize。
 * ============================================================
 */
#define SDL_WINDOW_WIDTH  540
#define SDL_WINDOW_HEIGHT 960


/*
 * ============================================================
 * 服务端线程共享结构
 * ============================================================
 */
typedef struct
{
    /*
     * TCP Server
     */
    TCP_Server *server;


    /*
     * TCP → Decoder
     */
    H264PacketQueue *packet_queue;


    /*
     * FFmpeg Decoder
     */
    H264_Decoder *decoder;


    /*
     * Decoder → SDL
     */
    AVFrameQueue *frame_queue;


    /*
     * TCP收到的H264 Packet数量
     */
    uint64_t recv_packet_count;


    /*
     * 错误标志
     */
    int tcp_error;

    int decoder_error;

} ServerThreadShare;


/*
 * ============================================================
 * 解码Frame回调
 *
 * Decoder：
 *
 * avcodec_receive_frame()
 *
 *          ↓
 *
 * frame_callback()
 *
 *          ↓
 *
 * AVFrameQueue_Push()
 *
 *          ↓
 *
 * av_frame_clone()
 *
 *          ↓
 *
 * main / SDL
 * ============================================================
 */
static int frame_callback(
    const AVFrame *frame,
    uint64_t frame_index,
    void *opaque
)
{
    if (frame == NULL ||
        opaque == NULL)
    {
        return -1;
    }


    ServerThreadShare *share =
        (ServerThreadShare *)opaque;


    /*
     * 将Decoder输出的AVFrame
     * 放入FrameQueue。
     *
     * Push内部执行av_frame_clone。
     */
    int ret =
        AVFrameQueue_Push(
            share->frame_queue,
            frame,
            frame_index
        );


    /*
     * Queue已经Stop。
     *
     * 一般意味着用户关闭SDL窗口，
     * 整个Pipeline正在退出。
     */
    if (ret == 0)
    {
        return -1;
    }


    if (ret < 0)
    {
        fprintf(
            stderr,
            "AVFrameQueue_Push failed, "
            "frame=%llu\n",

            (unsigned long long)
                frame_index
        );


        return -1;
    }


    return 0;
}


/*
 * ============================================================
 * TCP接收线程主体
 *
 * RK3568
 *
 *      ↓
 *
 * TCP_Server_RecvH264
 *
 *      ↓
 *
 * H264PacketQueue
 * ============================================================
 */
static void tcp_receive_thread_run(
    ServerThreadShare *share
)
{
    if (share == NULL)
    {
        return;
    }


    printf(
        "TCP receive thread started\n"
    );


    while (1)
    {
        /*
         * TCP_Server内部recv_buffer。
         */
        uint8_t *h264_data = NULL;


        size_t h264_len = 0;


        uint32_t sequence = 0;


        /*
         * ====================================================
         * 1. 接收一个完整H264 Packet
         *
         * TCP_Server_RecvH264内部已经完成：
         *
         * Header
         * ↓
         * payload_len
         * ↓
         * 完整H264 Payload
         * ====================================================
         */
        int ret =
            TCP_Server_RecvH264(
                share->server,

                &h264_data,

                &h264_len,

                &sequence
            );


        /*
         * RK3568正常断开。
         */
        if (ret == 0)
        {
            printf(
                "RK3568 disconnected\n"
            );


            break;
        }


        /*
         * TCP / 协议错误。
         */
        if (ret < 0)
        {
            fprintf(
                stderr,
                "TCP_Server_RecvH264 failed\n"
            );


            share->tcp_error = 1;


            break;
        }


        /*
         * ====================================================
         * 2. 收到Packet
         * ====================================================
         */
        share->recv_packet_count++;


        /*
         * 调试阶段打印。
         *
         * 后面实时运行稳定以后，
         * 可以降低打印频率。
         */
        printf(
            "Recv H264 packet: "
            "seq=%u "
            "len=%zu "
            "count=%llu\n",

            sequence,

            h264_len,

            (unsigned long long)
                share->recv_packet_count
        );


        /*
         * ====================================================
         * 3. H264数据放入Queue
         *
         * Push内部进行：
         *
         * malloc
         * +
         * memcpy
         *
         * TCP收到下一Packet以后
         * recv_buffer可以安全覆盖。
         * ====================================================
         */
        ret =
            H264PacketQueue_Push(
                share->packet_queue,

                h264_data,

                h264_len,

                sequence
            );


        /*
         * Pipeline正在关闭。
         */
        if (ret == 0)
        {
            break;
        }


        if (ret < 0)
        {
            fprintf(
                stderr,
                "H264PacketQueue_Push failed\n"
            );


            share->tcp_error = 1;


            break;
        }
    }


    /*
     * ========================================================
     * TCP不再生产H264数据。
     *
     * Decoder把Queue里面已经存在的数据消费完以后退出。
     * ========================================================
     */
    H264PacketQueue_Stop(
        share->packet_queue
    );


    printf(
        "TCP receive thread stopped\n"
    );
}


/*
 * ============================================================
 * Decoder线程
 *
 * H264PacketQueue
 *
 *      ↓
 *
 * FFmpeg Parser
 *
 *      ↓
 *
 * FFmpeg Decoder
 *
 *      ↓
 *
 * frame_callback
 *
 *      ↓
 *
 * AVFrameQueue
 * ============================================================
 */
static void decoder_thread_run(
    ServerThreadShare *share
)
{
    if (share == NULL)
    {
        return;
    }


    printf(
        "H264 decoder thread started\n"
    );


    while (1)
    {
        H264Packet packet;


        memset(
            &packet,
            0,
            sizeof(packet)
        );


        /*
         * ====================================================
         * 1. 从压缩数据Queue取得Packet
         * ====================================================
         */
        int ret =
            H264PacketQueue_Pop(
                share->packet_queue,
                &packet
            );


        /*
         * TCP线程已经Stop Queue，
         *
         * 并且所有Packet已经处理完成。
         */
        if (ret == 0)
        {
            break;
        }


        if (ret < 0)
        {
            fprintf(
                stderr,
                "H264PacketQueue_Pop failed\n"
            );


            share->decoder_error = 1;


            break;
        }


        /*
         * ====================================================
         * 2. FFmpeg解码
         *
         * 解码成功得到AVFrame以后：
         *
         * frame_callback()
         *
         * 会将Frame送入AVFrameQueue。
         * ====================================================
         */
        ret =
            H264_Decoder_Decode(
                share->decoder,

                packet.data,

                packet.len,

                frame_callback,

                share
            );


        if (ret < 0)
        {
            fprintf(
                stderr,
                "H264 decode failed: "
                "sequence=%u "
                "len=%zu\n",

                packet.sequence,

                packet.len
            );


            /*
             * 如果是因为SDL主动退出，
             * FrameQueue已经Stop，
             * 此时出现callback返回错误是正常退出过程。
             *
             * 这里仍记录错误，
             * 后面可以进一步细分退出状态。
             */
            share->decoder_error = 1;
        }


        /*
         * ====================================================
         * 3. 当前压缩Packet已经使用完成
         * ====================================================
         */
        H264Packet_Free(
            &packet
        );
    }


    /*
     * ========================================================
     * 4. Flush Decoder
     *
     * 将FFmpeg内部最后缓存的视频帧输出。
     * ========================================================
     */
    int flush_ret =
        H264_Decoder_Flush(
            share->decoder,

            frame_callback,

            share
        );


    if (flush_ret < 0)
    {
        /*
         * 如果FrameQueue已经因为用户关闭窗口而Stop，
         * callback也可能导致这里返回错误。
         */
        fprintf(
            stderr,
            "H264_Decoder_Flush failed\n"
        );
    }


    /*
     * ========================================================
     * Decoder以后不会再产生任何AVFrame。
     *
     * 通知main线程。
     * ========================================================
     */
    AVFrameQueue_Stop(
        share->frame_queue
    );


    printf(
        "H264 decoder thread stopped, "
        "decoded frames=%llu\n",

        (unsigned long long)
            share->decoder->frame_count
    );
}


/*
 * ============================================================
 * pthread TCP入口
 * ============================================================
 */
static void *tcp_receive_thread(
    void *arg
)
{
    ServerThreadShare *share =
        (ServerThreadShare *)arg;


    tcp_receive_thread_run(
        share
    );


    return NULL;
}


/*
 * ============================================================
 * pthread Decoder入口
 * ============================================================
 */
static void *decoder_thread(
    void *arg
)
{
    ServerThreadShare *share =
        (ServerThreadShare *)arg;


    decoder_thread_run(
        share
    );


    return NULL;
}


/*
 * ============================================================
 * 停止整个数据Pipeline
 *
 * 用户关闭SDL窗口时调用。
 * ============================================================
 */
static void stop_pipeline(
    TCP_Server *server,
    H264PacketQueue *packet_queue,
    AVFrameQueue *frame_queue
)
{
    /*
     * SDL不再需要新的Frame。
     */
    AVFrameQueue_Stop(
        frame_queue
    );


    /*
     * Decoder不再等待新的H264 Packet。
     */
    H264PacketQueue_Stop(
        packet_queue
    );


    /*
     * shutdown + close client socket。
     *
     * 让正在阻塞recv的TCP线程立即退出。
     */
    TCP_Server_CloseClient(
        server
    );
}


/*
 * ============================================================
 * main
 * ============================================================
 */
int main(void)
{
    /*
     * ========================================================
     * 1. 创建各模块
     * ========================================================
     */

    TCP_Server server;


    H264PacketQueue packet_queue;


    H264_Decoder decoder;


    AVFrameQueue frame_queue;


    /*
     * 新增：
     *
     * SDL显示模块
     */
    SDL_Display display;


    /*
     * ========================================================
     * 2. 初始化TCP Server
     * ========================================================
     */
    if (TCP_Server_Init(
            &server,
            SERVER_PORT) < 0)
    {
        fprintf(
            stderr,
            "TCP_Server_Init failed\n"
        );


        return -1;
    }


    /*
     * ========================================================
     * 3. 初始化H264 Packet Queue
     * ========================================================
     */
    if (H264PacketQueue_Init(
            &packet_queue,
            H264_QUEUE_CAPACITY) < 0)
    {
        fprintf(
            stderr,
            "H264PacketQueue_Init failed\n"
        );


        TCP_Server_Destroy(
            &server
        );


        return -1;
    }


    /*
     * ========================================================
     * 4. 初始化AVFrame Queue
     * ========================================================
     */
    if (AVFrameQueue_Init(
            &frame_queue,
            AVFRAME_QUEUE_CAPACITY) < 0)
    {
        fprintf(
            stderr,
            "AVFrameQueue_Init failed\n"
        );


        H264PacketQueue_Destroy(
            &packet_queue
        );


        TCP_Server_Destroy(
            &server
        );


        return -1;
    }


    /*
     * ========================================================
     * 5. 初始化FFmpeg H264 Decoder
     * ========================================================
     */
    if (H264_Decoder_Init(
            &decoder) < 0)
    {
        fprintf(
            stderr,
            "H264_Decoder_Init failed\n"
        );


        AVFrameQueue_Destroy(
            &frame_queue
        );


        H264PacketQueue_Destroy(
            &packet_queue
        );


        TCP_Server_Destroy(
            &server
        );


        return -1;
    }


    /*
     * ========================================================
     * 6. 等待RK3568连接
     *
     * 注意：
     *
     * SDL放在Accept之后初始化。
     *
     * 否则Accept阻塞期间SDL窗口已经创建，
     * 但main无法处理SDL事件，
     * 窗口可能显示未响应。
     * ========================================================
     */
    if (TCP_Server_Accept(
            &server) < 0)
    {
        fprintf(
            stderr,
            "TCP_Server_Accept failed\n"
        );


        H264_Decoder_Destroy(
            &decoder
        );


        AVFrameQueue_Destroy(
            &frame_queue
        );


        H264PacketQueue_Destroy(
            &packet_queue
        );


        TCP_Server_Destroy(
            &server
        );


        return -1;
    }


    /*
     * ========================================================
     * 7. 初始化SDL2
     *
     * SDL必须由main线程维护。
     *
     * Texture暂时还没创建，
     * 第一帧到达以后根据AVFrame自动创建。
     * ========================================================
     */
    if (SDL_Display_Init(
            &display,

            "RK3568 H264 Monitor",

            SDL_WINDOW_WIDTH,

            SDL_WINDOW_HEIGHT) < 0)
    {
        fprintf(
            stderr,
            "SDL_Display_Init failed\n"
        );


        H264_Decoder_Destroy(
            &decoder
        );


        AVFrameQueue_Destroy(
            &frame_queue
        );


        H264PacketQueue_Destroy(
            &packet_queue
        );


        TCP_Server_Destroy(
            &server
        );


        return -1;
    }


    /*
     * ========================================================
     * 8. 初始化线程共享结构
     * ========================================================
     */
    ServerThreadShare share;


    memset(
        &share,
        0,
        sizeof(share)
    );


    share.server =
        &server;


    share.packet_queue =
        &packet_queue;


    share.decoder =
        &decoder;


    share.frame_queue =
        &frame_queue;


    /*
     * ========================================================
     * 9. 创建TCP接收线程
     * ========================================================
     */
    pthread_t tcp_tid;


    if (pthread_create(
            &tcp_tid,
            NULL,
            tcp_receive_thread,
            &share) != 0)
    {
        fprintf(
            stderr,
            "pthread_create TCP thread failed\n"
        );


        SDL_Display_Destroy(
            &display
        );


        H264_Decoder_Destroy(
            &decoder
        );


        AVFrameQueue_Destroy(
            &frame_queue
        );


        H264PacketQueue_Destroy(
            &packet_queue
        );


        TCP_Server_Destroy(
            &server
        );


        return -1;
    }


    /*
     * ========================================================
     * 10. 创建Decoder线程
     * ========================================================
     */
    pthread_t decoder_tid;


    if (pthread_create(
            &decoder_tid,
            NULL,
            decoder_thread,
            &share) != 0)
    {
        fprintf(
            stderr,
            "pthread_create decoder thread failed\n"
        );


        /*
         * 让TCP线程退出。
         */
        stop_pipeline(
            &server,
            &packet_queue,
            &frame_queue
        );


        pthread_join(
            tcp_tid,
            NULL
        );


        SDL_Display_Destroy(
            &display
        );


        H264_Decoder_Destroy(
            &decoder
        );


        AVFrameQueue_Destroy(
            &frame_queue
        );


        H264PacketQueue_Destroy(
            &packet_queue
        );


        TCP_Server_Destroy(
            &server
        );


        return -1;
    }


    /*
     * ========================================================
     * 11. SDL Main Loop
     *
     * main线程现在只做：
     *
     * SDL Event
     *
     *       +
     *
     * AVFrameQueue TryPop
     *
     *       +
     *
     * SDL Render
     * ========================================================
     */

    int running = 1;


    uint64_t display_frame_count =
        0;


    while (running)
    {
        /*
         * ====================================================
         * A. SDL事件
         *
         * 用户点击X：
         *
         * running = 0
         *
         * 或ESC：
         *
         * running = 0
         * ====================================================
         */
        int quit =
            SDL_Display_PollQuit(
                &display
            );


        if (quit == 1)
        {
            printf(
                "SDL quit requested\n"
            );


            running = 0;


            /*
             * 用户主动退出：
             *
             * 通知整个Pipeline停止。
             */
            stop_pipeline(
                &server,
                &packet_queue,
                &frame_queue
            );


            break;
        }


        if (quit < 0)
        {
            fprintf(
                stderr,
                "SDL event processing failed\n"
            );


            running = 0;


            stop_pipeline(
                &server,
                &packet_queue,
                &frame_queue
            );


            break;
        }


        /*
         * ====================================================
         * B. 非阻塞取得一个解码Frame
         * ====================================================
         */
        DecodedFrame decoded_frame;


        memset(
            &decoded_frame,
            0,
            sizeof(decoded_frame)
        );


        int ret =
            AVFrameQueue_TryPop(
                &frame_queue,
                &decoded_frame
            );


        /*
         * ====================================================
         * ret == 1
         *
         * 成功拿到视频Frame。
         * ====================================================
         */
        if (ret == 1)
        {
            /*
             * FFmpeg AVFrame
             */
            AVFrame *frame =
                decoded_frame.frame;


            /*
             * ================================================
             * SDL显示
             *
             * 内部：
             *
             * YUV420P：
             *
             * SDL_UpdateYUVTexture
             *
             * 或NV12：
             *
             * SDL_UpdateNVTexture
             *
             *       ↓
             *
             * SDL_RenderCopy
             *
             *       ↓
             *
             * SDL_RenderPresent
             * ================================================
             */
            if (SDL_Display_RenderFrame(
                    &display,
                    frame) < 0)
            {
                fprintf(
                    stderr,
                    "SDL_Display_RenderFrame failed\n"
                );


                /*
                 * 当前Frame仍然必须释放。
                 */
                DecodedFrame_Free(
                    &decoded_frame
                );


                running = 0;


                stop_pipeline(
                    &server,
                    &packet_queue,
                    &frame_queue
                );


                break;
            }


            display_frame_count++;


            /*
             * 调试阶段：
             *
             * 不再每帧打印，
             * 每60帧打印一次。
             */
            if (
                display_frame_count % 60 ==
                0
            )
            {
                printf(
                    "Displayed frame: "
                    "index=%llu "
                    "size=%dx%d "
                    "format=%d "
                    "count=%llu\n",

                    (unsigned long long)
                        decoded_frame.frame_index,

                    frame->width,

                    frame->height,

                    frame->format,

                    (unsigned long long)
                        display_frame_count
                );
            }


            /*
             * ================================================
             * SDL_RenderPresent完成以后，
             *
             * 当前AVFrame不再需要。
             *
             * 解除av_frame_clone产生的引用。
             * ================================================
             */
            DecodedFrame_Free(
                &decoded_frame
            );
        }


        /*
         * ====================================================
         * ret == 0
         *
         * 当前暂时没有视频Frame。
         *
         * 不阻塞，
         * 下一次继续PollEvent。
         * ====================================================
         */
        else if (ret == 0)
        {
            /*
             * 防止main线程空转占满一个CPU核心。
             */
            SDL_Delay(1);
        }


        /*
         * ====================================================
         * ret == 2
         *
         * Decoder已经Stop FrameQueue
         *
         * 并且所有剩余Frame已经显示完成。
         *
         * 视频数据流正常结束。
         * ====================================================
         */
        else if (ret == 2)
        {
            printf(
                "Video stream finished\n"
            );


            running = 0;


            break;
        }


        /*
         * ====================================================
         * ret < 0
         *
         * FrameQueue错误。
         * ====================================================
         */
        else
        {
            fprintf(
                stderr,
                "AVFrameQueue_TryPop failed\n"
            );


            running = 0;


            stop_pipeline(
                &server,
                &packet_queue,
                &frame_queue
            );


            break;
        }
    }


    /*
     * ========================================================
     * 12. 确保Pipeline全部停止
     *
     * 正常视频结束时这些函数重复调用也没关系，
     * Stop本身只是设置stop标志。
     * ========================================================
     */

    H264PacketQueue_Stop(
        &packet_queue
    );


    AVFrameQueue_Stop(
        &frame_queue
    );


    /*
     * ========================================================
     * 13. 等待TCP / Decoder线程真正退出
     * ========================================================
     */
    pthread_join(
        tcp_tid,
        NULL
    );


    pthread_join(
        decoder_tid,
        NULL
    );


    /*
     * ========================================================
     * 14. 打印统计
     * ========================================================
     */
    printf(
        "\n"
        "=================================\n"
        "Server statistics\n"
        "=================================\n"
        "Received H264 packets : %llu\n"
        "Decoded video frames  : %llu\n"
        "Displayed video frames: %llu\n"
        "TCP error              : %d\n"
        "Decoder error          : %d\n"
        "=================================\n",

        (unsigned long long)
            share.recv_packet_count,

        (unsigned long long)
            decoder.frame_count,

        (unsigned long long)
            display_frame_count,

        share.tcp_error,

        share.decoder_error
    );


    /*
     * ========================================================
     * 15. SDL释放
     *
     * SDL资源继续由main线程释放。
     * ========================================================
     */
    SDL_Display_Destroy(
        &display
    );


    /*
     * ========================================================
     * 16. Decoder
     * ========================================================
     */
    H264_Decoder_Destroy(
        &decoder
    );


    /*
     * ========================================================
     * 17. Frame Queue
     * ========================================================
     */
    AVFrameQueue_Destroy(
        &frame_queue
    );


    /*
     * ========================================================
     * 18. H264 Packet Queue
     * ========================================================
     */
    H264PacketQueue_Destroy(
        &packet_queue
    );


    /*
     * ========================================================
     * 19. TCP
     * ========================================================
     */
    TCP_Server_Destroy(
        &server
    );


    printf(
        "Server exited\n"
    );


    return 0;
}