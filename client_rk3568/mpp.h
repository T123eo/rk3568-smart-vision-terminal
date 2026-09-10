#ifndef __MPP_H__
#define __MPP_H__

#include <stddef.h>
#include <stdint.h>

#include <rockchip/rk_mpi.h>


typedef struct
{
    /*
     * MPP encoder
     */
    MppCtx ctx;
    MppApi *mpi;

    /*
     * MPP buffer
     */
    MppBufferGroup buf_group;
    MppBuffer buffer;

    /*
     * 图像参数
     */
    int width;
    int height;

    /*
     * stride
     *
     * hor_stride：
     * 每一行实际占用的字节数
     *
     * ver_stride：
     * 图像实际占用的行数
     */
    int hor_stride;
    int ver_stride;

    /*
     * MPP 输入 buffer
     */
    void *mpp_ptr;
    size_t frame_size;

    /*
     * H264 编码结果
     *
     * packet_ptr：
     * 当前一帧编码后的 H264 数据
     *
     * len：
     * 当前有效 H264 数据长度
     *
     * packet_capacity：
     * packet_ptr 当前分配空间大小
     */
    uint8_t *packet_ptr;
    size_t packet_capacity;
    size_t len;

    /*
     * H264 SPS/PPS
     */
    uint8_t *header_ptr;
    size_t header_len;

    /*
     * 是否已经发送 SPS/PPS
     */
    int header_sent;

} MPP_Encode_cfg;


/*
 * 初始化 H264 编码器
 *
 * 调用前需要设置：
 *
 * enc->width
 * enc->height
 * enc->hor_stride
 * enc->ver_stride
 *
 * 成功：
 * return 0
 *
 * 失败：
 * return -1
 */
int MPP_Init(MPP_Encode_cfg *enc);


/*
 * 编码一帧 NV12
 *
 * enc：
 * MPP 编码器结构体
 *
 * src：
 * V4L2 获取到的 NV12 数据地址
 *
 * src_size：
 * 当前摄像头 buffer 的有效数据长度
 * 一般传：
 *
 * buf.m.planes[0].bytesused
 *
 * src_stride：
 * 摄像头每行实际字节数
 * 一般传：
 *
 * bytesperline
 *
 *
 * 编码成功后：
 *
 * enc->packet_ptr
 *      H264 数据地址
 *
 * enc->len
 *      H264 数据长度
 *
 *
 * return 0：
 * 编码成功
 *
 * return -1：
 * 编码失败
 */
int MPP_Encode(
    MPP_Encode_cfg *enc,
    const void *src,
    size_t src_size,
    int src_stride
);


/*
 * 释放 MPP 编码器资源
 */
void MPP_Free(MPP_Encode_cfg *enc);


#endif