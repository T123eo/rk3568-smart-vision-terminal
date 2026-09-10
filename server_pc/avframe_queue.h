#ifndef AVFRAME_QUEUE_H
#define AVFRAME_QUEUE_H

#include <stdint.h>
#include <stddef.h>

#include <libavutil/frame.h>


/*
 * ============================================================
 * Windows / Linux线程同步兼容
 * ============================================================
 */
#ifdef _WIN32

#include <windows.h>

#else

#include <pthread.h>

#endif


/*
 * ============================================================
 * AVFrame Queue默认容量
 *
 * 解码后的帧数据远大于H264压缩packet，
 * 所以这里不要设置太大。
 *
 * 例如1080x1920 YUV420P：
 *
 * 大约：
 *
 * 1080 * 1920 * 1.5
 *
 * ≈ 3MB / frame
 *
 * 8帧已经可能对应几十MB引用的数据。
 * ============================================================
 */
#define AVFRAME_QUEUE_DEFAULT_CAPACITY 8


/*
 * ============================================================
 * 单个解码帧
 * ============================================================
 */
typedef struct
{
    /*
     * 独立AVFrame引用
     *
     * 由AVFrameQueue_Push内部通过：
     *
     * av_frame_clone()
     *
     * 创建。
     *
     * Pop以后所有权交给消费者。
     */
    AVFrame *frame;


    /*
     * 解码帧序号
     */
    uint64_t frame_index;

} DecodedFrame;


/*
 * ============================================================
 * AVFrame环形队列
 * ============================================================
 */
typedef struct
{
    /*
     * DecodedFrame数组
     */
    DecodedFrame *frames;


    /*
     * Queue总容量
     */
    size_t capacity;


    /*
     * 下一个读取位置
     */
    size_t front;


    /*
     * 下一个写入位置
     */
    size_t rear;


    /*
     * 当前Queue中Frame数量
     */
    size_t count;


    /*
     * stop = 1：
     *
     * 不会再产生新Frame。
     */
    int stop;


    /*
     * 是否已经成功初始化
     */
    int initialized;


#ifdef _WIN32

    CRITICAL_SECTION mutex;

    CONDITION_VARIABLE not_empty;

    CONDITION_VARIABLE not_full;

#else

    pthread_mutex_t mutex;

    pthread_cond_t not_empty;

    pthread_cond_t not_full;

#endif

} AVFrameQueue;


/*
 * ============================================================
 * 初始化AVFrame Queue
 *
 * capacity == 0：
 *
 * 使用默认容量8。
 *
 *
 * 返回：
 *
 *      0   成功
 *     -1   失败
 * ============================================================
 */
int AVFrameQueue_Init(
    AVFrameQueue *queue,
    size_t capacity
);


/*
 * ============================================================
 * Push
 *
 * 将FFmpeg解码得到的AVFrame放入Queue。
 *
 * 内部自动：
 *
 * av_frame_clone(frame)
 *
 * 所以调用完成以后，
 * decoder可以继续复用自己的AVFrame。
 *
 *
 * 队列满：
 *
 * 阻塞等待SDL线程取走一帧。
 *
 *
 * 返回：
 *
 *      1   成功
 *      0   Queue已经Stop
 *     -1   错误
 * ============================================================
 */
int AVFrameQueue_Push(
    AVFrameQueue *queue,
    const AVFrame *frame,
    uint64_t frame_index
);


/*
 * ============================================================
 * Pop
 *
 * 从Queue取得一个解码完成的视频Frame。
 *
 * 成功以后：
 *
 * decoded_frame->frame
 *
 * 的所有权交给消费者。
 *
 * SDL显示完成以后必须调用：
 *
 * DecodedFrame_Free()
 *
 *
 * 返回：
 *
 *      1   成功
 *
 *      0   Queue已经Stop
 *          并且Queue中没有剩余Frame
 *
 *     -1   错误
 * ============================================================
 */
int AVFrameQueue_Pop(
    AVFrameQueue *queue,
    DecodedFrame *decoded_frame
);


/*
 * ============================================================
 * 释放Pop取得的Frame
 * ============================================================
 */
void DecodedFrame_Free(
    DecodedFrame *decoded_frame
);


/*
 * ============================================================
 * 停止Frame Queue
 *
 * 会唤醒所有正在等待的线程。
 *
 * 已经存在于Queue中的Frame仍然可以继续Pop。
 * ============================================================
 */
void AVFrameQueue_Stop(
    AVFrameQueue *queue
);


/*
 * ============================================================
 * 销毁Frame Queue
 *
 * 自动释放Queue中还没有被SDL消费的AVFrame。
 *
 * 应该在线程全部退出以后调用。
 * ============================================================
 */
void AVFrameQueue_Destroy(
    AVFrameQueue *queue
);

/*
 * 非阻塞获取Frame
 *
 * 返回：
 *  1  成功取得一帧
 *  0  当前Queue暂时为空
 *  2  Queue已经Stop并且为空，后续不会再有Frame
 * -1  错误
 */
int AVFrameQueue_TryPop(
    AVFrameQueue *queue,
    DecodedFrame *decoded_frame
);

#endif