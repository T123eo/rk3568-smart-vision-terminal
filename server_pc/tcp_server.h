#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stdint.h>
#include <stddef.h>


/*
 * ============================================================
 * Windows / Linux socket兼容
 * ============================================================
 */
#ifdef _WIN32

#include <winsock2.h>

typedef SOCKET socket_t;

#else

typedef int socket_t;

#endif


/*
 * ============================================================
 * TCP Server控制结构
 * ============================================================
 */
typedef struct
{
    /*
     * 监听socket
     */
    socket_t listen_fd;

    /*
     * 当前已经连接的RK3568客户端socket
     */
    socket_t client_fd;

    /*
     * 服务端监听端口
     */
    uint16_t port;


    /*
     * H264接收buffer
     *
     * TCP_Server_RecvH264内部根据payload大小
     * 自动进行realloc扩容。
     */
    uint8_t *recv_buffer;

    /*
     * recv_buffer当前容量
     */
    size_t recv_buffer_capacity;


    /*
     * 上一个H264 packet的序号
     */
    uint32_t last_sequence;

    /*
     * sequence是否已经初始化
     */
    int sequence_initialized;

} TCP_Server;


/*
 * ============================================================
 * 初始化TCP服务端
 *
 * 完成：
 *
 * socket
 * bind
 * listen
 *
 * Windows下同时完成WSAStartup。
 *
 *
 * 参数：
 *
 * server
 *      TCP Server结构体
 *
 * port
 *      服务端监听端口
 *
 *
 * 返回：
 *
 * 0     成功
 * -1    失败
 * ============================================================
 */
int TCP_Server_Init(
    TCP_Server *server,
    uint16_t port
);


/*
 * ============================================================
 * 等待RK3568客户端连接
 *
 * 内部调用accept。
 *
 *
 * 返回：
 *
 * 0     成功建立连接
 * -1    accept失败
 * ============================================================
 */
int TCP_Server_Accept(
    TCP_Server *server
);


/*
 * ============================================================
 * 接收一个完整H264 packet
 *
 *
 * 内部自动完成：
 *
 * recv完整16字节协议头
 *
 *          ↓
 *
 * 校验：
 *
 * magic
 * version
 * type
 *
 *          ↓
 *
 * 解析payload_len
 *
 *          ↓
 *
 * recv完整H264 payload
 *
 *
 * 参数：
 *
 * data
 *      返回H264数据地址
 *
 * len
 *      返回H264数据长度
 *
 * sequence
 *      返回当前H264 packet序号
 *
 *
 * 注意：
 *
 * *data指向TCP_Server内部recv_buffer。
 *
 * 下一次调用TCP_Server_RecvH264以后，
 * 内容可能会被覆盖。
 *
 *
 * 返回：
 *
 * 1     成功收到完整H264 packet
 *
 * 0     RK3568客户端断开连接
 *
 * -1    TCP或协议错误
 * ============================================================
 */
int TCP_Server_RecvH264(
    TCP_Server *server,
    uint8_t **data,
    size_t *len,
    uint32_t *sequence
);


/*
 * ============================================================
 * 关闭当前客户端连接
 *
 * listen_fd继续保留。
 *
 * 后面还可以再次调用TCP_Server_Accept。
 * ============================================================
 */
void TCP_Server_CloseClient(
    TCP_Server *server
);


/*
 * ============================================================
 * 完全销毁TCP Server
 *
 * 关闭：
 *
 * client_fd
 * listen_fd
 *
 * 释放：
 *
 * recv_buffer
 *
 * Windows下执行WSACleanup。
 * ============================================================
 */
void TCP_Server_Destroy(
    TCP_Server *server
);


#endif