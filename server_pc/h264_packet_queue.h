#ifndef H264_PACKET_QUEUE_H
#define H264_PACKET_QUEUE_H

#include <stdint.h>
#include <stddef.h>


/*
 * ============================================================
 * Windows / Linux 线程同步兼容
 * ============================================================
 */
#ifdef _WIN32

#include <windows.h>

#else

#include <pthread.h>

#endif


/*
 * ============================================================
 * 默认队列容量
 *
 * 表示最多缓存32个H264 packet。
 *
 * 注意：
 * 这里是packet数量，不是视频帧数量。
 * ============================================================
 */
#define H264_PACKET_QUEUE_DEFAULT_CAPACITY 32


/*
 * ============================================================
 * 单个H264 Packet
 * ============================================================
 */
typedef struct
{
    /*
     * H264压缩数据
     *
     * 该空间由队列内部malloc产生。
     *
     * 从Queue_Pop取出以后，
     * 所有权转移给消费者。
     */
    uint8_t *data;

    /*
     * H264数据长度
     */
    size_t len;

    /*
     * TCP协议中的packet sequence
     */
    uint32_t sequence;

} H264Packet;


/*
 * ============================================================
 * H264 Packet Queue
 *
 * 使用环形队列实现。
 * ============================================================
 */
typedef struct
{
    /*
     * Packet数组
     */
    H264Packet *packets;

    /*
     * 队列总容量
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
     * 当前队列中packet数量
     */
    size_t count;


    /*
     * stop = 1
     *
     * 表示程序准备退出。
     *
     * 用于唤醒正在等待的生产者/消费者。
     */
    int stop;


#ifdef _WIN32

    /*
     * Windows同步对象
     */
    CRITICAL_SECTION mutex;

    CONDITION_VARIABLE not_empty;

    CONDITION_VARIABLE not_full;

#else

    /*
     * Linux pthread同步对象
     */
    pthread_mutex_t mutex;

    pthread_cond_t not_empty;

    pthread_cond_t not_full;

#endif

} H264PacketQueue;


/*
 * ============================================================
 * 初始化H264 Packet队列
 *
 * capacity:
 *
 *      队列最多保存多少个packet。
 *
 * 如果capacity == 0，
 * 自动使用默认容量：
 *
 *      H264_PACKET_QUEUE_DEFAULT_CAPACITY
 *
 *
 * 返回：
 *
 *      0   成功
 *     -1   失败
 * ============================================================
 */
int H264PacketQueue_Init(
    H264PacketQueue *queue,
    size_t capacity
);


/*
 * ============================================================
 * Push
 *
 * 将一个H264 packet放入队列。
 *
 * 重要：
 *
 * 本函数会：
 *
 * malloc(len)
 * memcpy()
 *
 * 所以调用完成以后，
 * 原来的TCP recv_buffer可以立即被下一次recv覆盖。
 *
 *
 * 如果队列已满：
 *
 * 当前线程阻塞等待not_full。
 *
 *
 * 返回：
 *
 *      1   成功
 *      0   队列已经Stop
 *     -1   错误
 * ============================================================
 */
int H264PacketQueue_Push(
    H264PacketQueue *queue,
    const uint8_t *data,
    size_t len,
    uint32_t sequence
);


/*
 * ============================================================
 * Pop
 *
 * 从队列取出一个完整H264 packet。
 *
 *
 * 如果队列为空：
 *
 * 当前线程阻塞等待not_empty。
 *
 *
 * 成功以后：
 *
 * packet->data
 *
 * 指向队列之前malloc的空间。
 *
 * 所有权转移给调用者。
 *
 * 使用完以后必须：
 *
 * H264Packet_Free(packet);
 *
 *
 * 返回：
 *
 *      1   成功得到packet
 *      0   queue已经Stop并且没有剩余数据
 *     -1   错误
 * ============================================================
 */
int H264PacketQueue_Pop(
    H264PacketQueue *queue,
    H264Packet *packet
);


/*
 * ============================================================
 * 释放一个Pop出来的H264 Packet
 * ============================================================
 */
void H264Packet_Free(
    H264Packet *packet
);


/*
 * ============================================================
 * 停止队列
 *
 * 会唤醒：
 *
 * Push中等待not_full的线程
 * Pop中等待not_empty的线程
 *
 * 通常程序退出时调用。
 * ============================================================
 */
void H264PacketQueue_Stop(
    H264PacketQueue *queue
);


/*
 * ============================================================
 * 销毁队列
 *
 * 自动释放队列中尚未消费的所有packet。
 *
 * 调用前应该确保生产者和消费者线程已经退出。
 * ============================================================
 */
void H264PacketQueue_Destroy(
    H264PacketQueue *queue
);


#endif