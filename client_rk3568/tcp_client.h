#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>


/*
 * TCP客户端控制结构
 */
typedef struct
{
    /* TCP socket文件描述符 */
    int sockfd;

    /* PC服务端地址 */
    struct sockaddr_in server_addr;

    /* H264 packet发送序号 */
    uint32_t sequence;

    /* 当前连接状态 */
    int connected;

} TCP_Client;


/*
 * 初始化TCP客户端并连接服务端
 *
 * client:
 *      TCP客户端结构体
 *
 * server_ip:
 *      服务端IPv4地址
 *
 * server_port:
 *      服务端端口
 *
 * 返回：
 *      0  成功
 *     -1 失败
 */
int TCP_Client_Init(TCP_Client *client,
                    const char *server_ip,
                    uint16_t server_port);


/*
 * 通用协议packet发送函数
 *
 * 一般main.c不需要直接调用，
 * H264数据使用TCP_Client_SendH264即可。
 */
int TCP_Client_SendPacket(TCP_Client *client,
                          uint16_t type,
                          const void *payload,
                          size_t payload_len);


/*
 * 发送一包MPP编码后的H264数据
 *
 * packet_data:
 *      MPP输出的H264数据地址
 *
 * packet_len:
 *      H264数据长度
 */
int TCP_Client_SendH264(TCP_Client *client,
                        const void *packet_data,
                        size_t packet_len);


/*
 * 关闭TCP连接
 */
void TCP_Client_Close(TCP_Client *client);


#endif