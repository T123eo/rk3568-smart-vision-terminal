#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "tcp_client.h"
/*
 * ============================================================
 * TCP应用层协议
 * ============================================================
 *
 * 每发送一个MPP输出的H264 packet：
 *
 * +-------------------------------------------------------+
 * | magic      | 4 bytes | 固定 0x48323634 ("H264")      |
 * | version    | 2 bytes | 协议版本，目前为1             |
 * | type       | 2 bytes | 数据类型，目前1代表H264       |
 * | length     | 4 bytes | 后面H264 payload长度           |
 * | sequence   | 4 bytes | 包序号                         |
 * +-------------------------------------------------------+
 * | H264 payload                                           |
 * +-------------------------------------------------------+
 *
 * 固定头长度：16 bytes
 *
 * 所有多字节整数统一使用网络字节序（大端）。
 */


/* ================= 协议定义 ================= */

#define TCP_MAGIC               0x48323634U    /* ASCII: H264 */
#define TCP_PROTOCOL_VERSION    1

#define TCP_PACKET_H264         1

#define TCP_HEADER_SIZE         16

/*
 * 防止服务端因为错误长度申请超大内存。
 *
 * 实际MPP单个packet通常远远小于8MB。
 */
#define TCP_MAX_PAYLOAD_SIZE    (8U * 1024U * 1024U)




/*
 * ============================================================
 * send_all
 *
 * TCP的send()并不保证一次发送完全部数据。
 *
 * 例如：
 *
 * send(fd, data, 10000, 0)
 *
 * 可能只返回：
 *
 * 4096
 *
 * 所以必须循环send，直到所有数据全部进入TCP发送缓冲区。
 *
 * 返回：
 *      0  成功
 *     -1 失败
 * ============================================================
 */
static int tcp_send_all(int fd,
                        const void *data,
                        size_t len)
{
    const uint8_t *ptr = (const uint8_t *)data;

    size_t total_sent = 0;

    while (total_sent < len)
    {
        ssize_t ret = send(
            fd,
            ptr + total_sent,
            len - total_sent,
            MSG_NOSIGNAL
        );

        if (ret > 0)
        {
            total_sent += (size_t)ret;
        }
        else if (ret == 0)
        {
            fprintf(stderr,
                    "send returned 0\n");

            return -1;
        }
        else
        {
            /*
             * 被信号中断不算真正错误，
             * 继续发送即可。
             */
            if (errno == EINTR)
            {
                continue;
            }

            perror("send");

            return -1;
        }
    }

    return 0;
}


/*
 * ============================================================
 * 将uint16_t写入buffer
 *
 * 使用网络字节序。
 * ============================================================
 */
static void write_u16(uint8_t *buf,
                      uint16_t value)
{
    uint16_t temp = htons(value);

    memcpy(buf, &temp, sizeof(temp));
}


/*
 * ============================================================
 * 将uint32_t写入buffer
 *
 * 使用网络字节序。
 * ============================================================
 */
static void write_u32(uint8_t *buf,
                      uint32_t value)
{
    uint32_t temp = htonl(value);

    memcpy(buf, &temp, sizeof(temp));
}


/*
 * ============================================================
 * 构造16字节TCP协议头
 *
 * offset:
 *
 * 0  ~ 3   magic
 * 4  ~ 5   version
 * 6  ~ 7   type
 * 8  ~ 11  payload length
 * 12 ~ 15  sequence
 * ============================================================
 */
static void tcp_build_header(uint8_t header[TCP_HEADER_SIZE],
                             uint16_t type,
                             uint32_t payload_len,
                             uint32_t sequence)
{
    memset(header, 0, TCP_HEADER_SIZE);

    write_u32(header + 0,
              TCP_MAGIC);

    write_u16(header + 4,
              TCP_PROTOCOL_VERSION);

    write_u16(header + 6,
              type);

    write_u32(header + 8,
              payload_len);

    write_u32(header + 12,
              sequence);
}


/*
 * ============================================================
 * TCP_Client_Init
 *
 * 功能：
 *
 * 1. 创建socket
 * 2. 设置服务端IP和端口
 * 3. connect连接PC服务端
 *
 *
 * 参数：
 *
 * client
 *      TCP客户端结构体
 *
 * server_ip
 *      服务端IPv4地址
 *
 *      例如：
 *      "192.168.1.100"
 *
 * server_port
 *      服务端监听端口
 *
 *      例如：
 *      8088
 *
 *
 * 返回：
 *
 *      0  成功
 *     -1 失败
 * ============================================================
 */
int TCP_Client_Init(TCP_Client *client,
                    const char *server_ip,
                    uint16_t server_port)
{
    if (client == NULL ||
        server_ip == NULL)
    {
        fprintf(stderr,
                "TCP_Client_Init invalid argument\n");

        return -1;
    }

    memset(client, 0, sizeof(*client));

    client->sockfd = -1;

    /*
     * 创建TCP socket
     */
    client->sockfd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (client->sockfd < 0)
    {
        perror("socket");

        return -1;
    }


    /*
     * 设置服务端地址
     */
    memset(&client->server_addr,
           0,
           sizeof(client->server_addr));

    client->server_addr.sin_family =
        AF_INET;

    client->server_addr.sin_port =
        htons(server_port);


    /*
     * 将IPv4字符串：
     *
     * "192.168.1.100"
     *
     * 转换成网络地址格式。
     */
    int ret = inet_pton(
        AF_INET,
        server_ip,
        &client->server_addr.sin_addr
    );

    if (ret != 1)
    {
        if (ret == 0)
        {
            fprintf(stderr,
                    "Invalid IPv4 address: %s\n",
                    server_ip);
        }
        else
        {
            perror("inet_pton");
        }

        close(client->sockfd);

        client->sockfd = -1;

        return -1;
    }


    /*
     * 与PC服务端建立TCP连接
     */
    ret = connect(
        client->sockfd,
        (struct sockaddr *)&client->server_addr,
        sizeof(client->server_addr)
    );

    if (ret < 0)
    {
        perror("connect");

        close(client->sockfd);

        client->sockfd = -1;

        return -1;
    }


    client->sequence = 0;
    client->connected = 1;

    printf("TCP connected to %s:%u\n",
           server_ip,
           server_port);

    return 0;
}


/*
 * ============================================================
 * TCP_Client_SendPacket
 *
 * 通用发送函数：
 *
 * 协议头
 * +
 * payload
 *
 *
 * TCP本身没有消息边界。
 *
 * 所以这里逻辑上发送：
 *
 *      header
 *      payload
 *
 * 服务端不应该依赖recv()次数。
 *
 * 服务端应该：
 *
 *      recv_exact(16)
 *
 *      解析payload_len
 *
 *      recv_exact(payload_len)
 *
 *
 * 返回：
 *
 *      0  成功
 *     -1 失败
 * ============================================================
 */
int TCP_Client_SendPacket(TCP_Client *client,
                          uint16_t type,
                          const void *payload,
                          size_t payload_len)
{
    if (client == NULL ||
        !client->connected ||
        client->sockfd < 0)
    {
        fprintf(stderr,
                "TCP client not connected\n");

        return -1;
    }


    if (payload == NULL ||
        payload_len == 0)
    {
        fprintf(stderr,
                "Invalid TCP payload\n");

        return -1;
    }


    /*
     * 协议长度字段只有uint32_t。
     */
    if (payload_len > UINT32_MAX)
    {
        fprintf(stderr,
                "TCP payload too large\n");

        return -1;
    }


    if (payload_len > TCP_MAX_PAYLOAD_SIZE)
    {
        fprintf(stderr,
                "TCP payload exceeds limit: %zu\n",
                payload_len);

        return -1;
    }


    uint8_t header[TCP_HEADER_SIZE];


    /*
     * 构造协议头。
     */
    tcp_build_header(
        header,
        type,
        (uint32_t)payload_len,
        client->sequence
    );


    /*
     * -------------------------
     * 第一步：发送16字节协议头
     * -------------------------
     */
    if (tcp_send_all(
            client->sockfd,
            header,
            sizeof(header)) < 0)
    {
        client->connected = 0;

        return -1;
    }


    /*
     * -------------------------
     * 第二步：发送H264数据
     * -------------------------
     */
    if (tcp_send_all(
            client->sockfd,
            payload,
            payload_len) < 0)
    {
        client->connected = 0;

        return -1;
    }


    /*
     * 当前packet发送完成后，
     * 包序号+1。
     */
    client->sequence++;


    return 0;
}


/*
 * ============================================================
 * TCP_Client_SendH264
 *
 * 专门供MPP线程使用。
 *
 * packet_data：
 *      enc->packet_ptr
 *
 * packet_len：
 *      enc->len
 *
 *
 * 返回：
 *
 *      0  成功
 *     -1 失败
 * ============================================================
 */
int TCP_Client_SendH264(TCP_Client *client,
                        const void *packet_data,
                        size_t packet_len)
{
    return TCP_Client_SendPacket(
        client,
        TCP_PACKET_H264,
        packet_data,
        packet_len
    );
}


/*
 * ============================================================
 * TCP_Client_Close
 *
 * 关闭TCP连接。
 * ============================================================
 */
void TCP_Client_Close(TCP_Client *client)
{
    if (client == NULL)
    {
        return;
    }


    if (client->sockfd >= 0)
    {
        /*
         * 通知TCP协议栈：
         * 不再发送和接收。
         */
        shutdown(
            client->sockfd,
            SHUT_RDWR
        );


        close(client->sockfd);

        client->sockfd = -1;
    }


    client->connected = 0;

    printf("TCP client closed\n");
}