#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h264_packet_queue.h"


/*
 * ============================================================
 * 内部加锁函数
 * ============================================================
 */
static void queue_lock(
    H264PacketQueue *queue
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
 * 内部解锁函数
 * ============================================================
 */
static void queue_unlock(
    H264PacketQueue *queue
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
 * 等待not_empty
 *
 * 队列为空时，
 * 消费者睡眠。
 * ============================================================
 */
static void queue_wait_not_empty(
    H264PacketQueue *queue
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
 * 等待not_full
 *
 * 队列满时，
 * 生产者睡眠。
 * ============================================================
 */
static void queue_wait_not_full(
    H264PacketQueue *queue
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
 * 唤醒一个等待not_empty的消费者
 * ============================================================
 */
static void queue_signal_not_empty(
    H264PacketQueue *queue
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
 * 唤醒一个等待not_full的生产者
 * ============================================================
 */
static void queue_signal_not_full(
    H264PacketQueue *queue
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
 * 唤醒所有等待线程
 *
 * Stop时使用。
 * ============================================================
 */
static void queue_wake_all(
    H264PacketQueue *queue
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
 * H264PacketQueue_Init
 * ============================================================
 */
int H264PacketQueue_Init(
    H264PacketQueue *queue,
    size_t capacity
)
{
    if (queue == NULL)
    {
        fprintf(
            stderr,
            "H264PacketQueue_Init: queue is NULL\n"
        );

        return -1;
    }


    /*
     * capacity = 0时使用默认容量
     */
    if (capacity == 0)
    {
        capacity =
            H264_PACKET_QUEUE_DEFAULT_CAPACITY;
    }


    /*
     * 整个结构体清零
     */
    memset(
        queue,
        0,
        sizeof(*queue)
    );


    /*
     * 创建packet数组
     */
    queue->packets =
        (H264Packet *)calloc(
            capacity,
            sizeof(H264Packet)
        );


    if (queue->packets == NULL)
    {
        fprintf(
            stderr,
            "H264PacketQueue_Init: calloc failed\n"
        );

        return -1;
    }


    queue->capacity =
        capacity;

    queue->front = 0;

    queue->rear = 0;

    queue->count = 0;

    queue->stop = 0;


    /*
     * --------------------------------------------------------
     * 初始化线程同步对象
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

    if (pthread_mutex_init(
            &queue->mutex,
            NULL) != 0)
    {
        fprintf(
            stderr,
            "pthread_mutex_init failed\n"
        );

        free(
            queue->packets
        );

        queue->packets = NULL;

        return -1;
    }


    if (pthread_cond_init(
            &queue->not_empty,
            NULL) != 0)
    {
        fprintf(
            stderr,
            "pthread_cond_init not_empty failed\n"
        );

        pthread_mutex_destroy(
            &queue->mutex
        );

        free(
            queue->packets
        );

        queue->packets = NULL;

        return -1;
    }


    if (pthread_cond_init(
            &queue->not_full,
            NULL) != 0)
    {
        fprintf(
            stderr,
            "pthread_cond_init not_full failed\n"
        );

        pthread_cond_destroy(
            &queue->not_empty
        );

        pthread_mutex_destroy(
            &queue->mutex
        );

        free(
            queue->packets
        );

        queue->packets = NULL;

        return -1;
    }

#endif


    printf(
        "H264 packet queue initialized, capacity=%zu\n",
        queue->capacity
    );


    return 0;
}


/*
 * ============================================================
 * H264PacketQueue_Push
 *
 * TCP线程调用。
 * ============================================================
 */
int H264PacketQueue_Push(
    H264PacketQueue *queue,
    const uint8_t *data,
    size_t len,
    uint32_t sequence
)
{
    if (queue == NULL ||
        data == NULL ||
        len == 0)
    {
        return -1;
    }


    /*
     * --------------------------------------------------------
     * 1. 为当前H264 packet申请独立空间
     *
     * 为什么在加锁之前malloc？
     *
     * 因为malloc可能比较耗时，
     * 不需要在malloc期间占用queue mutex。
     * --------------------------------------------------------
     */

    uint8_t *packet_data =
        (uint8_t *)malloc(
            len
        );


    if (packet_data == NULL)
    {
        fprintf(
            stderr,
            "H264PacketQueue_Push malloc failed: %zu bytes\n",
            len
        );

        return -1;
    }


    /*
     * 复制TCP接收到的H264数据。
     *
     * 从这里开始，
     * packet_data就完全独立于：
     *
     * server->recv_buffer
     */
    memcpy(
        packet_data,
        data,
        len
    );


    /*
     * --------------------------------------------------------
     * 2. 操作Queue之前加锁
     * --------------------------------------------------------
     */

    queue_lock(
        queue
    );


    /*
     * --------------------------------------------------------
     * 3. 如果队列已经满
     *
     * TCP线程睡眠，
     * 等待FFmpeg消费者取走数据。
     * --------------------------------------------------------
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
     * --------------------------------------------------------
     * 4. 程序准备退出
     * --------------------------------------------------------
     */

    if (queue->stop)
    {
        queue_unlock(
            queue
        );


        /*
         * 当前packet还没有进入Queue，
         * 所以这里必须自己释放。
         */
        free(
            packet_data
        );


        return 0;
    }


    /*
     * --------------------------------------------------------
     * 5. 写入rear位置
     * --------------------------------------------------------
     */

    H264Packet *packet =
        &queue->packets[
            queue->rear
        ];


    packet->data =
        packet_data;


    packet->len =
        len;


    packet->sequence =
        sequence;


    /*
     * --------------------------------------------------------
     * 6. rear向后移动
     *
     * 环形队列：
     *
     * rear到达capacity以后重新回到0。
     *
     * 例如capacity = 4：
     *
     * 0 → 1 → 2 → 3 → 0 → 1
     * --------------------------------------------------------
     */

    queue->rear =
        (queue->rear + 1) %
        queue->capacity;


    /*
     * 当前packet数量+1
     */
    queue->count++;


    /*
     * --------------------------------------------------------
     * 7. 通知解码线程：
     *
     * 队列中现在有数据了。
     * --------------------------------------------------------
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
 * H264PacketQueue_Pop
 *
 * FFmpeg解码线程调用。
 * ============================================================
 */
int H264PacketQueue_Pop(
    H264PacketQueue *queue,
    H264Packet *packet
)
{
    if (queue == NULL ||
        packet == NULL)
    {
        return -1;
    }


    /*
     * 先把输出packet清零。
     */
    memset(
        packet,
        0,
        sizeof(*packet)
    );


    queue_lock(
        queue
    );


    /*
     * --------------------------------------------------------
     * 1. 如果队列为空
     *
     * 解码线程进入睡眠。
     * --------------------------------------------------------
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
     * --------------------------------------------------------
     * 2. Stop并且已经没有数据
     *
     * 解码线程可以退出。
     * --------------------------------------------------------
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
     * --------------------------------------------------------
     * 3. 读取front位置的packet
     * --------------------------------------------------------
     */

    H264Packet *src =
        &queue->packets[
            queue->front
        ];


    /*
     * 注意：
     *
     * 这里不是memcpy H264数据。
     *
     * 只是把这块malloc内存的所有权
     * 转移给解码线程。
     */
    packet->data =
        src->data;


    packet->len =
        src->len;


    packet->sequence =
        src->sequence;


    /*
     * 队列内部slot清零。
     *
     * 防止Destroy时再次free。
     */
    src->data = NULL;

    src->len = 0;

    src->sequence = 0;


    /*
     * --------------------------------------------------------
     * 4. front向后移动
     * --------------------------------------------------------
     */

    queue->front =
        (queue->front + 1) %
        queue->capacity;


    /*
     * packet数量-1
     */
    queue->count--;


    /*
     * --------------------------------------------------------
     * 5. 通知TCP线程：
     *
     * Queue现在有空位了。
     * --------------------------------------------------------
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
 * H264Packet_Free
 *
 * 解码线程处理完成以后调用。
 * ============================================================
 */
void H264Packet_Free(
    H264Packet *packet
)
{
    if (packet == NULL)
    {
        return;
    }


    if (packet->data != NULL)
    {
        free(
            packet->data
        );


        packet->data =
            NULL;
    }


    packet->len = 0;

    packet->sequence = 0;
}


/*
 * ============================================================
 * H264PacketQueue_Stop
 * ============================================================
 */
void H264PacketQueue_Stop(
    H264PacketQueue *queue
)
{
    if (queue == NULL)
    {
        return;
    }


    queue_lock(
        queue
    );


    queue->stop = 1;


    /*
     * 唤醒所有可能正在睡眠的线程。
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
 * H264PacketQueue_Destroy
 * ============================================================
 */
void H264PacketQueue_Destroy(
    H264PacketQueue *queue
)
{
    if (queue == NULL)
    {
        return;
    }


    /*
     * --------------------------------------------------------
     * 1. 释放Queue中还没有被消费的Packet
     * --------------------------------------------------------
     */

    if (queue->packets != NULL)
    {
        for (
            size_t i = 0;
            i < queue->capacity;
            i++
        )
        {
            if (
                queue->packets[i].data != NULL
            )
            {
                free(
                    queue->packets[i].data
                );


                queue->packets[i].data =
                    NULL;
            }
        }


        free(
            queue->packets
        );


        queue->packets =
            NULL;
    }


    /*
     * --------------------------------------------------------
     * 2. 销毁同步资源
     * --------------------------------------------------------
     */

#ifdef _WIN32

    /*
     * Windows CONDITION_VARIABLE
     * 不需要显式Destroy。
     *
     * CriticalSection需要释放。
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


    queue->capacity = 0;

    queue->front = 0;

    queue->rear = 0;

    queue->count = 0;

    queue->stop = 1;
}