#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>

#include "avframe_queue.h"


/*
 * ============================================================
 * Queue加锁
 * ============================================================
 */
static void queue_lock(
    AVFrameQueue *queue
)
{
#ifdef _WIN32

    EnterCriticalSection(
        &queue->mutex
    );

#else

    pthread_mutex_lock(
        &queue->mutex
    );

#endif
}


/*
 * ============================================================
 * Queue解锁
 * ============================================================
 */
static void queue_unlock(
    AVFrameQueue *queue
)
{
#ifdef _WIN32

    LeaveCriticalSection(
        &queue->mutex
    );

#else

    pthread_mutex_unlock(
        &queue->mutex
    );

#endif
}


/*
 * ============================================================
 * 等待Queue非空
 *
 * SDL消费者使用。
 * ============================================================
 */
static void queue_wait_not_empty(
    AVFrameQueue *queue
)
{
#ifdef _WIN32

    SleepConditionVariableCS(
        &queue->not_empty,
        &queue->mutex,
        INFINITE
    );

#else

    pthread_cond_wait(
        &queue->not_empty,
        &queue->mutex
    );

#endif
}


/*
 * ============================================================
 * 等待Queue非满
 *
 * Decoder生产者使用。
 * ============================================================
 */
static void queue_wait_not_full(
    AVFrameQueue *queue
)
{
#ifdef _WIN32

    SleepConditionVariableCS(
        &queue->not_full,
        &queue->mutex,
        INFINITE
    );

#else

    pthread_cond_wait(
        &queue->not_full,
        &queue->mutex
    );

#endif
}


/*
 * ============================================================
 * 通知消费者：
 *
 * Queue中现在有Frame。
 * ============================================================
 */
static void queue_signal_not_empty(
    AVFrameQueue *queue
)
{
#ifdef _WIN32

    WakeConditionVariable(
        &queue->not_empty
    );

#else

    pthread_cond_signal(
        &queue->not_empty
    );

#endif
}


/*
 * ============================================================
 * 通知生产者：
 *
 * Queue现在有空位。
 * ============================================================
 */
static void queue_signal_not_full(
    AVFrameQueue *queue
)
{
#ifdef _WIN32

    WakeConditionVariable(
        &queue->not_full
    );

#else

    pthread_cond_signal(
        &queue->not_full
    );

#endif
}


/*
 * ============================================================
 * Stop时唤醒全部等待线程
 * ============================================================
 */
static void queue_wake_all(
    AVFrameQueue *queue
)
{
#ifdef _WIN32

    WakeAllConditionVariable(
        &queue->not_empty
    );


    WakeAllConditionVariable(
        &queue->not_full
    );

#else

    pthread_cond_broadcast(
        &queue->not_empty
    );


    pthread_cond_broadcast(
        &queue->not_full
    );

#endif
}


/*
 * ============================================================
 * AVFrameQueue_Init
 * ============================================================
 */
int AVFrameQueue_Init(
    AVFrameQueue *queue,
    size_t capacity
)
{
    if (queue == NULL)
    {
        fprintf(
            stderr,
            "AVFrameQueue_Init: queue is NULL\n"
        );

        return -1;
    }


    /*
     * 默认容量
     */
    if (capacity == 0)
    {
        capacity =
            AVFRAME_QUEUE_DEFAULT_CAPACITY;
    }


    /*
     * 整个控制结构清零
     */
    memset(
        queue,
        0,
        sizeof(*queue)
    );


    /*
     * --------------------------------------------------------
     * 1. 创建DecodedFrame数组
     * --------------------------------------------------------
     */
    queue->frames =
        (DecodedFrame *)calloc(
            capacity,
            sizeof(DecodedFrame)
        );


    if (queue->frames == NULL)
    {
        fprintf(
            stderr,
            "AVFrameQueue_Init: calloc failed\n"
        );

        return -1;
    }


    queue->capacity =
        capacity;


    queue->front =
        0;


    queue->rear =
        0;


    queue->count =
        0;


    queue->stop =
        0;


    /*
     * --------------------------------------------------------
     * 2. 初始化线程同步对象
     * --------------------------------------------------------
     */

#ifdef _WIN32

    InitializeCriticalSection(
        &queue->mutex
    );


    InitializeConditionVariable(
        &queue->not_empty
    );


    InitializeConditionVariable(
        &queue->not_full
    );


#else

    /*
     * Mutex
     */
    if (pthread_mutex_init(
            &queue->mutex,
            NULL) != 0)
    {
        fprintf(
            stderr,
            "AVFrameQueue: pthread_mutex_init failed\n"
        );


        free(
            queue->frames
        );


        queue->frames =
            NULL;


        return -1;
    }


    /*
     * not_empty
     */
    if (pthread_cond_init(
            &queue->not_empty,
            NULL) != 0)
    {
        fprintf(
            stderr,
            "AVFrameQueue: "
            "pthread_cond_init not_empty failed\n"
        );


        pthread_mutex_destroy(
            &queue->mutex
        );


        free(
            queue->frames
        );


        queue->frames =
            NULL;


        return -1;
    }


    /*
     * not_full
     */
    if (pthread_cond_init(
            &queue->not_full,
            NULL) != 0)
    {
        fprintf(
            stderr,
            "AVFrameQueue: "
            "pthread_cond_init not_full failed\n"
        );


        pthread_cond_destroy(
            &queue->not_empty
        );


        pthread_mutex_destroy(
            &queue->mutex
        );


        free(
            queue->frames
        );


        queue->frames =
            NULL;


        return -1;
    }

#endif


    queue->initialized =
        1;


    printf(
        "AVFrame queue initialized, "
        "capacity=%zu\n",
        queue->capacity
    );


    return 0;
}


/*
 * ============================================================
 * AVFrameQueue_Push
 *
 * Decoder线程调用。
 * ============================================================
 */
int AVFrameQueue_Push(
    AVFrameQueue *queue,
    const AVFrame *frame,
    uint64_t frame_index
)
{
    if (queue == NULL ||
        frame == NULL)
    {
        return -1;
    }


    if (!queue->initialized)
    {
        return -1;
    }


    /*
     * ========================================================
     * 1. Clone当前AVFrame
     *
     * 不能：
     *
     * queue->frames[x].frame = frame;
     *
     * 因为decoder内部AVFrame下一次
     * avcodec_receive_frame时会被继续复用。
     *
     * av_frame_clone创建一个新的AVFrame引用。
     * ========================================================
     */
    AVFrame *cloned_frame =
        av_frame_clone(
            frame
        );


    if (cloned_frame == NULL)
    {
        fprintf(
            stderr,
            "AVFrameQueue_Push: "
            "av_frame_clone failed\n"
        );

        return -1;
    }


    /*
     * ========================================================
     * 2. 操作Queue之前加锁
     * ========================================================
     */
    queue_lock(
        queue
    );


    /*
     * ========================================================
     * 3. Queue已满
     *
     * Decoder线程进入睡眠。
     *
     * 等待SDL线程Pop Frame。
     * ========================================================
     */
    while (
        queue->count ==
            queue->capacity &&
        !queue->stop
    )
    {
        queue_wait_not_full(
            queue
        );
    }


    /*
     * ========================================================
     * 4. 程序已经准备退出
     *
     * clone出来的Frame还没进入Queue，
     * 所以必须在这里释放。
     * ========================================================
     */
    if (queue->stop)
    {
        queue_unlock(
            queue
        );


        av_frame_free(
            &cloned_frame
        );


        return 0;
    }


    /*
     * ========================================================
     * 5. 写入rear位置
     * ========================================================
     */
    DecodedFrame *dst =
        &queue->frames[
            queue->rear
        ];


    dst->frame =
        cloned_frame;


    dst->frame_index =
        frame_index;


    /*
     * ========================================================
     * 6. rear向后移动
     *
     * 环形：
     *
     * capacity = 4
     *
     * 0 → 1 → 2 → 3 → 0
     * ========================================================
     */
    queue->rear =
        (queue->rear + 1) %
        queue->capacity;


    queue->count++;


    /*
     * ========================================================
     * 7. 唤醒SDL消费者
     * ========================================================
     */
    queue_signal_not_empty(
        queue
    );


    queue_unlock(
        queue
    );


    return 1;
}


/*
 * ============================================================
 * AVFrameQueue_Pop
 *
 * SDL线程调用。
 * ============================================================
 */
int AVFrameQueue_Pop(
    AVFrameQueue *queue,
    DecodedFrame *decoded_frame
)
{
    if (queue == NULL ||
        decoded_frame == NULL)
    {
        return -1;
    }


    if (!queue->initialized)
    {
        return -1;
    }


    /*
     * 输出结构先清零
     */
    memset(
        decoded_frame,
        0,
        sizeof(*decoded_frame)
    );


    queue_lock(
        queue
    );


    /*
     * ========================================================
     * 1. Queue为空
     *
     * SDL消费者睡眠。
     * ========================================================
     */
    while (
        queue->count == 0 &&
        !queue->stop
    )
    {
        queue_wait_not_empty(
            queue
        );
    }


    /*
     * ========================================================
     * 2. Queue已经Stop
     *
     * 并且已经没有任何剩余Frame。
     *
     * 消费者可以退出。
     * ========================================================
     */
    if (
        queue->count == 0 &&
        queue->stop
    )
    {
        queue_unlock(
            queue
        );


        return 0;
    }


    /*
     * ========================================================
     * 3. 取得front位置
     * ========================================================
     */
    DecodedFrame *src =
        &queue->frames[
            queue->front
        ];


    /*
     * ========================================================
     * 不复制视频数据。
     *
     * 直接把AVFrame指针所有权
     * 转移给SDL线程。
     * ========================================================
     */
    decoded_frame->frame =
        src->frame;


    decoded_frame->frame_index =
        src->frame_index;


    /*
     * Queue内部slot清空。
     *
     * 防止Destroy时重复av_frame_free。
     */
    src->frame =
        NULL;


    src->frame_index =
        0;


    /*
     * ========================================================
     * 4. front向后移动
     * ========================================================
     */
    queue->front =
        (queue->front + 1) %
        queue->capacity;


    queue->count--;


    /*
     * ========================================================
     * 5. Queue现在有空位
     *
     * 唤醒可能阻塞的Decoder线程。
     * ========================================================
     */
    queue_signal_not_full(
        queue
    );


    queue_unlock(
        queue
    );


    return 1;
}


/*
 * ============================================================
 * DecodedFrame_Free
 *
 * SDL显示完成以后调用。
 * ============================================================
 */
void DecodedFrame_Free(
    DecodedFrame *decoded_frame
)
{
    if (decoded_frame == NULL)
    {
        return;
    }


    /*
     * av_frame_free不仅释放AVFrame结构，
     * 还会解除其对底层AVBuffer的引用。
     */
    if (decoded_frame->frame != NULL)
    {
        av_frame_free(
            &decoded_frame->frame
        );
    }


    decoded_frame->frame_index =
        0;
}


/*
 * ============================================================
 * AVFrameQueue_Stop
 * ============================================================
 */
void AVFrameQueue_Stop(
    AVFrameQueue *queue
)
{
    if (queue == NULL ||
        !queue->initialized)
    {
        return;
    }


    queue_lock(
        queue
    );


    queue->stop =
        1;


    /*
     * Decoder可能正在等待not_full。
     *
     * SDL可能正在等待not_empty。
     *
     * 全部唤醒。
     */
    queue_wake_all(
        queue
    );


    queue_unlock(
        queue
    );
}


/*
 * ============================================================
 * AVFrameQueue_Destroy
 * ============================================================
 */
void AVFrameQueue_Destroy(
    AVFrameQueue *queue
)
{
    if (queue == NULL ||
        !queue->initialized)
    {
        return;
    }


    /*
     * ========================================================
     * 1. 释放Queue中所有尚未被SDL消费的Frame
     * ========================================================
     */
    if (queue->frames != NULL)
    {
        for (
            size_t i = 0;
            i < queue->capacity;
            i++
        )
        {
            if (
                queue->frames[i].frame != NULL
            )
            {
                av_frame_free(
                    &queue->frames[i].frame
                );
            }


            queue->frames[i].frame_index =
                0;
        }


        free(
            queue->frames
        );


        queue->frames =
            NULL;
    }


    /*
     * ========================================================
     * 2. 销毁同步对象
     * ========================================================
     */

#ifdef _WIN32

    /*
     * CONDITION_VARIABLE不需要显式Destroy。
     */
    DeleteCriticalSection(
        &queue->mutex
    );


#else

    pthread_cond_destroy(
        &queue->not_empty
    );


    pthread_cond_destroy(
        &queue->not_full
    );


    pthread_mutex_destroy(
        &queue->mutex
    );

#endif
    queue->capacity =0;
    queue->front =0;
    queue->rear =0;
    queue->count =0;
    queue->stop =1;
    queue->initialized =0;
}
int AVFrameQueue_TryPop(
    AVFrameQueue *queue,
    DecodedFrame *decoded_frame
)
{
    if (queue == NULL ||
        decoded_frame == NULL ||
        !queue->initialized)
    {
        return -1;
    }

    memset(
        decoded_frame,
        0,
        sizeof(*decoded_frame)
    );

    queue_lock(queue);

    /*
     * 当前没有Frame
     */
    if (queue->count == 0)
    {
        /*
         * Decoder已经结束，
         * 后续也不会再有Frame。
         */
        if (queue->stop)
        {
            queue_unlock(queue);

            return 2;
        }

        /*
         * 只是暂时没有Frame。
         */
        queue_unlock(queue);

        return 0;
    }

    /*
     * 有Frame可以立即取得。
     */
    DecodedFrame *src =
        &queue->frames[queue->front];

    /*
     * 转移AVFrame所有权。
     */
    decoded_frame->frame =
        src->frame;

    decoded_frame->frame_index =
        src->frame_index;

    src->frame = NULL;

    src->frame_index = 0;

    /*
     * 环形Queue前移。
     */
    queue->front =
        (queue->front + 1) %
        queue->capacity;

    queue->count--;

    /*
     * 通知Decoder：
     * Queue现在有空位。
     */
    queue_signal_not_full(queue);

    queue_unlock(queue);

    return 1;
}