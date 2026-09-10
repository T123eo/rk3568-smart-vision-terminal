/*
 * tcp_server.c
 *
 * 功能：
 * 1. 创建TCP服务端
 * 2. bind / listen
 * 3. accept RK3568客户端
 * 4. 解析客户端定义的16字节协议头
 * 5. 解决TCP粘包 / 拆包 / 边界问题
 * 6. 接收一个完整的H264 packet
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "tcp_server.h"

/* ============================================================
 * Windows / Linux socket兼容
 * ============================================================ */
#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

typedef SOCKET socket_t;

#define INVALID_SOCKET_FD INVALID_SOCKET
#define CLOSE_SOCKET(fd)  closesocket(fd)

#else

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

typedef int socket_t;

#define INVALID_SOCKET_FD (-1)
#define CLOSE_SOCKET(fd)  close(fd)

#endif


/* ============================================================
 * 和RK3568客户端保持完全一致的协议
 *
 * 16 Byte Header
 *
 * 0  ~ 3     magic
 * 4  ~ 5     version
 * 6  ~ 7     type
 * 8  ~ 11    payload length
 * 12 ~ 15    sequence
 * ============================================================ */

#define TCP_MAGIC               0x48323634U   /* ASCII "H264" */

#define TCP_PROTOCOL_VERSION    1

#define TCP_PACKET_H264         1

#define TCP_HEADER_SIZE         16

/*
 * 防止错误数据导致服务端申请异常大的内存。
 */
#define TCP_MAX_PAYLOAD_SIZE    (8U * 1024U * 1024U)



/* ============================================================
 * Socket错误打印
 * ============================================================ */
static void print_socket_error(const char *msg)
{
#ifdef _WIN32

    fprintf(
        stderr,
        "%s failed, WSA error = %d\n",
        msg,
        WSAGetLastError()
    );

#else

    perror(msg);

#endif
}


/* ============================================================
 * recv_all
 *
 * 作用：
 *
 * TCP recv()不保证一次就收到请求的长度。
 *
 * 例如：
 *
 * recv(fd, header, 16)
 *
 * 完全有可能只返回：
 *
 * 4
 * 7
 * 5
 *
 * 所以必须循环接收，直到：
 *
 * total_recv == len
 *
 *
 * 返回值：
 *
 *   1   成功接收len字节
 *
 *   0   对端正常关闭连接
 *
 *  -1   socket发生错误
 *
 * ============================================================ */
static int tcp_recv_all(socket_t fd,
                        void *data,
                        size_t len)
{
    uint8_t *ptr = (uint8_t *)data;

    size_t total_recv = 0;


    while (total_recv < len)
    {
        int ret = recv(
            fd,
            (char *)(ptr + total_recv),
            (int)(len - total_recv),
            0
        );


        /*
         * 收到数据
         */
        if (ret > 0)
        {
            total_recv += (size_t)ret;
        }


        /*
         * recv == 0
         *
         * 客户端正常关闭TCP连接
         */
        else if (ret == 0)
        {
            return 0;
        }


        /*
         * recv < 0
         */
        else
        {

#ifdef _WIN32

            int error_code =
                WSAGetLastError();


            /*
             * Windows socket被信号/系统调用打断
             */
            if (error_code == WSAEINTR)
            {
                continue;
            }

#else

            /*
             * Linux被signal打断
             */
            if (errno == EINTR)
            {
                continue;
            }

#endif

            print_socket_error(
                "recv"
            );

            return -1;
        }
    }


    return 1;
}


/* ============================================================
 * 从buffer读取网络字节序uint16_t
 * ============================================================ */
static uint16_t read_u16(const uint8_t *buf)
{
    uint16_t value;

    memcpy(
        &value,
        buf,
        sizeof(value)
    );

    return ntohs(value);
}


/* ============================================================
 * 从buffer读取网络字节序uint32_t
 * ============================================================ */
static uint32_t read_u32(const uint8_t *buf)
{
    uint32_t value;

    memcpy(
        &value,
        buf,
        sizeof(value)
    );

    return ntohl(value);
}


/* ============================================================
 * TCP_Server_Init
 *
 * 完成：
 *
 * Windows:
 *      WSAStartup
 *
 * socket
 * ↓
 * setsockopt
 * ↓
 * bind
 * ↓
 * listen
 *
 *
 * 返回：
 *
 * 0    成功
 * -1   失败
 * ============================================================ */
int TCP_Server_Init(TCP_Server *server,
                    uint16_t port)
{
    if (server == NULL)
    {
        fprintf(
            stderr,
            "TCP_Server_Init: server is NULL\n"
        );

        return -1;
    }


    memset(
        server,
        0,
        sizeof(*server)
    );


    server->listen_fd =
        INVALID_SOCKET_FD;

    server->client_fd =
        INVALID_SOCKET_FD;

    server->port =
        port;


#ifdef _WIN32

    /*
     * Windows使用socket之前必须初始化Winsock。
     */
    WSADATA wsa_data;

    int ret =
        WSAStartup(
            MAKEWORD(2, 2),
            &wsa_data
        );


    if (ret != 0)
    {
        fprintf(
            stderr,
            "WSAStartup failed: %d\n",
            ret
        );

        return -1;
    }

#endif


    /*
     * --------------------------------------------------------
     * 1. 创建TCP socket
     * --------------------------------------------------------
     */

    server->listen_fd =
        socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP
        );


    if (server->listen_fd ==
        INVALID_SOCKET_FD)
    {
        print_socket_error(
            "socket"
        );

#ifdef _WIN32
        WSACleanup();
#endif

        return -1;
    }


    /*
     * --------------------------------------------------------
     * 2. SO_REUSEADDR
     *
     * 防止程序刚关闭后重新启动出现：
     *
     * Address already in use
     * --------------------------------------------------------
     */

    int reuse = 1;


    if (setsockopt(
            server->listen_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            (const char *)&reuse,
            sizeof(reuse)) < 0)
    {
        print_socket_error(
            "setsockopt SO_REUSEADDR"
        );

        /*
         * 这个错误一般不是致命的，
         * 暂时不直接退出。
         */
    }


    /*
     * --------------------------------------------------------
     * 3. 配置服务端地址
     * --------------------------------------------------------
     */

    struct sockaddr_in server_addr;

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );


    server_addr.sin_family =
        AF_INET;


    /*
     * 接收PC当前网卡上的所有IPv4地址。
     */
    server_addr.sin_addr.s_addr =
        htonl(INADDR_ANY);


    server_addr.sin_port =
        htons(port);


    /*
     * --------------------------------------------------------
     * 4. bind
     * --------------------------------------------------------
     */

    if (bind(
            server->listen_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) < 0)
    {
        print_socket_error(
            "bind"
        );

        CLOSE_SOCKET(
            server->listen_fd
        );

        server->listen_fd =
            INVALID_SOCKET_FD;


#ifdef _WIN32
        WSACleanup();
#endif

        return -1;
    }


    /*
     * --------------------------------------------------------
     * 5. listen
     * --------------------------------------------------------
     */

    if (listen(
            server->listen_fd,
            5) < 0)
    {
        print_socket_error(
            "listen"
        );

        CLOSE_SOCKET(
            server->listen_fd
        );

        server->listen_fd =
            INVALID_SOCKET_FD;


#ifdef _WIN32
        WSACleanup();
#endif

        return -1;
    }


    printf(
        "TCP server listening on port %u\n",
        port
    );


    return 0;
}


/* ============================================================
 * TCP_Server_Accept
 *
 * 等待RK3568客户端连接。
 *
 *
 * 返回：
 *
 * 0    成功
 * -1   accept失败
 * ============================================================ */
int TCP_Server_Accept(TCP_Server *server)
{
    if (server == NULL)
    {
        return -1;
    }


    if (server->listen_fd ==
        INVALID_SOCKET_FD)
    {
        fprintf(
            stderr,
            "TCP server is not initialized\n"
        );

        return -1;
    }


    struct sockaddr_in client_addr;

    memset(
        &client_addr,
        0,
        sizeof(client_addr)
    );


#ifdef _WIN32

    int client_addr_len =
        sizeof(client_addr);

#else

    socklen_t client_addr_len =
        sizeof(client_addr);

#endif


    printf(
        "Waiting for RK3568 client...\n"
    );


    /*
     * accept会阻塞等待客户端连接。
     */
    server->client_fd =
        accept(
            server->listen_fd,
            (struct sockaddr *)&client_addr,
            &client_addr_len
        );


    if (server->client_fd ==
        INVALID_SOCKET_FD)
    {
        print_socket_error(
            "accept"
        );

        return -1;
    }


    /*
     * 将客户端IP转换成字符串。
     */
    char client_ip[INET_ADDRSTRLEN] =
        {0};


    inet_ntop(
        AF_INET,
        &client_addr.sin_addr,
        client_ip,
        sizeof(client_ip)
    );


    printf(
        "RK3568 connected: %s:%u\n",
        client_ip,
        ntohs(client_addr.sin_port)
    );


    /*
     * 新的TCP连接重新开始sequence检查。
     */
    server->sequence_initialized = 0;

    server->last_sequence = 0;


    return 0;
}


/* ============================================================
 * TCP_Server_RecvH264
 *
 * 从TCP字节流中获取：
 *
 *      一个完整的H264 packet
 *
 *
 * 客户端发送格式：
 *
 *      16Byte Header
 *            +
 *      H264 Payload
 *
 *
 * 服务端：
 *
 * recv_all(header, 16)
 *
 *         ↓
 *
 * 解析payload_len
 *
 *         ↓
 *
 * recv_all(payload, payload_len)
 *
 *
 * 参数：
 *
 * server
 *      TCP服务端
 *
 * data
 *      返回H264数据地址
 *
 * len
 *      返回H264数据长度
 *
 * sequence
 *      返回当前packet序号
 *
 *
 * 注意：
 *
 * *data 指向 server->recv_buffer。
 *
 * 下一次调用TCP_Server_RecvH264时，
 * 该buffer内容会被新的packet覆盖。
 *
 *
 * 返回：
 *
 *   1   成功得到一个完整H264 packet
 *
 *   0   RK3568客户端断开连接
 *
 *  -1   协议或socket错误
 *
 * ============================================================ */
int TCP_Server_RecvH264(TCP_Server *server,
                        uint8_t **data,
                        size_t *len,
                        uint32_t *sequence)
{
    if (server == NULL ||
        data == NULL ||
        len == NULL)
    {
        return -1;
    }


    if (server->client_fd ==
        INVALID_SOCKET_FD)
    {
        fprintf(
            stderr,
            "No TCP client connected\n"
        );

        return -1;
    }


    /*
     * ========================================
     * 1. 先准确接收16字节协议头
     * ========================================
     */

    uint8_t header[TCP_HEADER_SIZE];


    int ret =
        tcp_recv_all(
            server->client_fd,
            header,
            TCP_HEADER_SIZE
        );


    /*
     * RK3568主动关闭
     */
    if (ret == 0)
    {
        printf(
            "RK3568 client disconnected\n"
        );

        return 0;
    }


    if (ret < 0)
    {
        return -1;
    }


    /*
     * ========================================
     * 2. 解析协议头
     * ========================================
     */

    uint32_t magic =
        read_u32(
            header + 0
        );


    uint16_t version =
        read_u16(
            header + 4
        );


    uint16_t type =
        read_u16(
            header + 6
        );


    uint32_t payload_len =
        read_u32(
            header + 8
        );


    uint32_t packet_sequence =
        read_u32(
            header + 12
        );


    /*
     * ========================================
     * 3. 检查Magic
     * ========================================
     */

    if (magic != TCP_MAGIC)
    {
        fprintf(
            stderr,
            "Invalid packet magic: 0x%08X\n",
            magic
        );


        return -1;
    }


    /*
     * ========================================
     * 4. 检查协议版本
     * ========================================
     */

    if (version !=
        TCP_PROTOCOL_VERSION)
    {
        fprintf(
            stderr,
            "Unsupported protocol version: %u\n",
            version
        );


        return -1;
    }


    /*
     * ========================================
     * 5. 检查packet类型
     * ========================================
     */

    if (type !=
        TCP_PACKET_H264)
    {
        fprintf(
            stderr,
            "Unsupported packet type: %u\n",
            type
        );


        return -1;
    }


    /*
     * ========================================
     * 6. 检查payload长度
     * ========================================
     */

    if (payload_len == 0)
    {
        fprintf(
            stderr,
            "Invalid H264 payload length: 0\n"
        );


        return -1;
    }


    if (payload_len >
        TCP_MAX_PAYLOAD_SIZE)
    {
        fprintf(
            stderr,
            "H264 payload too large: %u bytes\n",
            payload_len
        );


        return -1;
    }


    /*
     * ========================================
     * 7. 检查sequence
     *
     * TCP本身保证：
     *
     * 有序
     * 不丢
     * 不重复
     *
     * sequence主要用于检查客户端/服务端
     * 协议处理是否正确。
     * ========================================
     */

    if (server->sequence_initialized)
    {
        uint32_t expected_sequence =
            server->last_sequence + 1;


        if (packet_sequence !=
            expected_sequence)
        {
            fprintf(
                stderr,
                "Sequence warning: expected=%u actual=%u\n",
                expected_sequence,
                packet_sequence
            );
        }
    }


    server->last_sequence =
        packet_sequence;


    server->sequence_initialized =
        1;


    /*
     * ========================================
     * 8. 确保recv_buffer足够大
     * ========================================
     */

    if (server->recv_buffer_capacity <
        payload_len)
    {
        uint8_t *new_buffer =
            (uint8_t *)realloc(
                server->recv_buffer,
                payload_len
            );


        if (new_buffer == NULL)
        {
            fprintf(
                stderr,
                "realloc H264 buffer failed, size=%u\n",
                payload_len
            );


            return -1;
        }


        server->recv_buffer =
            new_buffer;


        server->recv_buffer_capacity =
            payload_len;
    }


    /*
     * ========================================
     * 9. 接收完整H264 Payload
     *
     * 无论TCP把payload拆成多少次recv，
     * tcp_recv_all都会一直收满payload_len。
     * ========================================
     */

    ret =
        tcp_recv_all(
            server->client_fd,
            server->recv_buffer,
            payload_len
        );


    if (ret == 0)
    {
        fprintf(
            stderr,
            "Client disconnected while receiving H264 payload\n"
        );


        return 0;
    }


    if (ret < 0)
    {
        return -1;
    }


    /*
     * ========================================
     * 10. 返回完整H264 packet
     * ========================================
     */

    *data =
        server->recv_buffer;


    *len =
        payload_len;


    if (sequence != NULL)
    {
        *sequence =
            packet_sequence;
    }


    return 1;
}


/* ============================================================
 * TCP_Server_CloseClient
 *
 * 只关闭当前RK3568连接。
 *
 * listen socket仍然保留，
 * 后面可以再次accept。
 * ============================================================ */
void TCP_Server_CloseClient(TCP_Server *server)
{
    if (server == NULL)
    {
        return;
    }


    if (server->client_fd !=
        INVALID_SOCKET_FD)
    {

#ifdef _WIN32

        shutdown(
            server->client_fd,
            SD_BOTH
        );

#else

        shutdown(
            server->client_fd,
            SHUT_RDWR
        );

#endif


        CLOSE_SOCKET(
            server->client_fd
        );


        server->client_fd =
            INVALID_SOCKET_FD;
    }


    server->sequence_initialized = 0;
}


/* ============================================================
 * TCP_Server_Destroy
 *
 * 完全释放TCP服务端。
 * ============================================================ */
void TCP_Server_Destroy(TCP_Server *server)
{
    if (server == NULL)
    {
        return;
    }


    /*
     * 关闭当前客户端
     */
    TCP_Server_CloseClient(
        server
    );


    /*
     * 关闭监听socket
     */
    if (server->listen_fd !=
        INVALID_SOCKET_FD)
    {
        CLOSE_SOCKET(
            server->listen_fd
        );


        server->listen_fd =
            INVALID_SOCKET_FD;
    }


    /*
     * 释放H264接收buffer
     */
    if (server->recv_buffer != NULL)
    {
        free(
            server->recv_buffer
        );


        server->recv_buffer =
            NULL;
    }


    server->recv_buffer_capacity = 0;


#ifdef _WIN32

    WSACleanup();

#endif
}