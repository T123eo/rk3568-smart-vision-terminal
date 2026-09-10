#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpp.h"

#include <rockchip/rk_mpi.h>

/*
 * 获取 H264 SPS / PPS
 */
static int MPP_Get_Header(MPP_Encode_cfg *enc)
{
    MPP_RET ret;

    MppPacket packet = NULL;

    /*
     * SPS/PPS 一般非常小，
     * 这里 4096 字节已经绰绰有余。
     */
    uint8_t header_buf[4096];

    ret = mpp_packet_init(
        &packet,
        header_buf,
        sizeof(header_buf)
    );

    if (ret != MPP_OK)
    {
        printf("mpp_packet_init header failed, ret = %d\n", ret);
        return -1;
    }


    /*
     * 很重要：
     *
     * MPP_ENC_GET_HDR_SYNC 前
     * 要把 packet length 清零。
     */
    mpp_packet_set_length(packet, 0);


    ret = enc->mpi->control(
        enc->ctx,
        MPP_ENC_GET_HDR_SYNC,
        packet
    );

    if (ret != MPP_OK)
    {
        printf("MPP_ENC_GET_HDR_SYNC failed, ret = %d\n", ret);

        mpp_packet_deinit(&packet);

        return -1;
    }


    void *ptr = mpp_packet_get_pos(packet);
    size_t len = mpp_packet_get_length(packet);


    enc->header_ptr = malloc(len);

    if (enc->header_ptr == NULL)
    {
        printf("malloc header failed\n");

        mpp_packet_deinit(&packet);

        return -1;
    }


    memcpy(
        enc->header_ptr,
        ptr,
        len
    );

    enc->header_len = len;


    printf(
        "H264 SPS/PPS header size = %zu bytes\n",
        enc->header_len
    );


    mpp_packet_deinit(&packet);

    return 0;
}


/*
 * MPP 编码器初始化
 *
 * 调用之前：
 *
 * enc.width
 * enc.height
 * enc.hor_stride
 * enc.ver_stride
 *
 * 必须已经填写。
 */
int MPP_Init(MPP_Encode_cfg *enc)
{
    MPP_RET ret;
    MppEncCfg cfg = NULL;

    if (enc == NULL)
    {
        return -1;
    }
    enc->ctx = NULL;
    enc->mpi = NULL;
    enc->buf_group = NULL;
    enc->buffer = NULL;
    enc->mpp_ptr = NULL;
    enc->packet_ptr = NULL;
    enc->packet_capacity = 0;
    enc->len = 0;

    enc->header_ptr = NULL;
    enc->header_len = 0;
    enc->header_sent = 0;


    /*
     * 基本参数检查
     */
    if (enc->width <= 0 ||
        enc->height <= 0 ||
        enc->hor_stride < enc->width ||
        enc->ver_stride < enc->height)
    {
        printf(
            "invalid MPP size: "
            "width=%d height=%d "
            "hstride=%d vstride=%d\n",
            enc->width,
            enc->height,
            enc->hor_stride,
            enc->ver_stride
        );

        return -1;
    }


    /*
     * NV12 要求宽高一般为偶数
     */
    if ((enc->width & 1) ||
        (enc->height & 1))
    {
        printf("NV12 width/height must be even\n");

        return -1;
    }


    /*
     * ------------------------------------------------
     * 1. 创建 MPP
     * ------------------------------------------------
     */
    ret = mpp_create(
        &enc->ctx,
        &enc->mpi
    );

    if (ret != MPP_OK)
    {
        printf(
            "mpp_create failed, ret = %d\n",
            ret
        );

        return -1;
    }


    /*
     * 输入输出使用阻塞模式。
     *
     * 你的编码线程：
     *
     * camera -> MPP -> TCP
     *
     * 目前使用阻塞模式最简单。
     */
    MppPollType timeout = MPP_POLL_BLOCK;


    ret = enc->mpi->control(
        enc->ctx,
        MPP_SET_INPUT_TIMEOUT,
        &timeout
    );

    if (ret != MPP_OK)
    {
        printf(
            "MPP_SET_INPUT_TIMEOUT failed, ret = %d\n",
            ret
        );

        goto ERROR;
    }


    ret = enc->mpi->control(
        enc->ctx,
        MPP_SET_OUTPUT_TIMEOUT,
        &timeout
    );

    if (ret != MPP_OK)
    {
        printf(
            "MPP_SET_OUTPUT_TIMEOUT failed, ret = %d\n",
            ret
        );

        goto ERROR;
    }


    /*
     * ------------------------------------------------
     * 2. 初始化 H264 encoder
     * ------------------------------------------------
     */
    ret = mpp_init(
        enc->ctx,
        MPP_CTX_ENC,
        MPP_VIDEO_CodingAVC
    );

    if (ret != MPP_OK)
    {
        printf(
            "mpp_init failed, ret = %d\n",
            ret
        );

        goto ERROR;
    }


    printf("H264 encoder init success\n");


    /*
     * ------------------------------------------------
     * 3. 创建编码配置
     * ------------------------------------------------
     */
    ret = mpp_enc_cfg_init(&cfg);

    if (ret != MPP_OK)
    {
        printf(
            "mpp_enc_cfg_init failed, ret = %d\n",
            ret
        );

        goto ERROR;
    }


    /*
     * ------------------------------------------------
     * 4. 输入图像配置
     *
     * IMX415 当前输出 NV12
     * ------------------------------------------------
     */
    mpp_enc_cfg_set_s32(
        cfg,
        "prep:width",
        enc->width
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "prep:height",
        enc->height
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "prep:hor_stride",
        enc->hor_stride
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "prep:ver_stride",
        enc->ver_stride
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "prep:format",
        MPP_FMT_YUV420SP
    );


    /*
     * ------------------------------------------------
     * 5. 码率控制
     * ------------------------------------------------
     *
     * 监控视频目前使用 CBR 比较合适。
     */
    mpp_enc_cfg_set_s32(
        cfg,
        "rc:mode",
        MPP_ENC_RC_MODE_CBR
    );


    /*
     * ------------------------------------------------
     * 6. 输入帧率
     * ------------------------------------------------
     */
    mpp_enc_cfg_set_s32(
        cfg,
        "rc:fps_in_flex",
        0
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "rc:fps_in_num",
        30
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "rc:fps_in_denom",
        1
    );


    /*
     * ------------------------------------------------
     * 7. 输出帧率
     * ------------------------------------------------
     */
    mpp_enc_cfg_set_s32(
        cfg,
        "rc:fps_out_flex",
        0
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "rc:fps_out_num",
        30
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "rc:fps_out_denom",
        1
    );


    /*
     * ------------------------------------------------
     * 8. GOP
     *
     * 30 fps
     * GOP = 60
     *
     * 即大约每 2 秒一个 I 帧
     * ------------------------------------------------
     */
    mpp_enc_cfg_set_s32(
        cfg,
        "rc:gop",
        60
    );


    /*
     * ------------------------------------------------
     * 9. 码率
     *
     * 当前你的项目先使用：
     *
     * target = 4Mbps
     * max    = 5Mbps
     * min    = 3Mbps
     * ------------------------------------------------
     */
    mpp_enc_cfg_set_s32(
        cfg,
        "rc:bps_target",
        4 * 1024 * 1024
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "rc:bps_max",
        5 * 1024 * 1024
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "rc:bps_min",
        3 * 1024 * 1024
    );


    /*
     * 禁止自动丢帧
     */
    mpp_enc_cfg_set_u32(
        cfg,
        "rc:drop_mode",
        MPP_ENC_RC_DROP_FRM_DISABLED
    );


    /*
     * ------------------------------------------------
     * 10. H264 参数
     * ------------------------------------------------
     *
     * High Profile
     */
    mpp_enc_cfg_set_s32(
        cfg,
        "codec:type",
        MPP_VIDEO_CodingAVC
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:profile",
        100
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:level",
        42
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:cabac_en",
        1
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:cabac_idc",
        0
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:trans8x8",
        1
    );


    /*
     * ------------------------------------------------
     * 11. QP
     * ------------------------------------------------
     */
    mpp_enc_cfg_set_s32(
        cfg,
        "h264:qp_init",
        26
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:qp_max",
        51
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:qp_min",
        10
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:qp_max_i",
        46
    );

    mpp_enc_cfg_set_s32(
        cfg,
        "h264:qp_min_i",
        18
    );


    /*
     * ------------------------------------------------
     * 12. 配置写入编码器
     * ------------------------------------------------
     */
    ret = enc->mpi->control(
        enc->ctx,
        MPP_ENC_SET_CFG,
        cfg
    );


    mpp_enc_cfg_deinit(cfg);
    cfg = NULL;


    if (ret != MPP_OK)
    {
        printf(
            "MPP_ENC_SET_CFG failed, ret = %d\n",
            ret
        );

        goto ERROR;
    }


    /*
     * ------------------------------------------------
     * 13. 创建 MPP Buffer Group
     * ------------------------------------------------
     */
    ret = mpp_buffer_group_get_internal(
        &enc->buf_group,
        MPP_BUFFER_TYPE_DRM
    );

    if (ret != MPP_OK)
    {
        printf(
            "mpp_buffer_group_get_internal failed, ret = %d\n",
            ret
        );

        goto ERROR;
    }


    /*
     * NV12:
     *
     * Y  = hor_stride * ver_stride
     *
     * UV = Y / 2
     *
     * 所以：
     *
     * frame_size =
     * hor_stride * ver_stride * 3 / 2
     */
    enc->frame_size =
        (size_t)enc->hor_stride *
        enc->ver_stride *
        3 / 2;


    printf(
        "MPP frame buffer size = %zu bytes\n",
        enc->frame_size
    );


    /*
     * ------------------------------------------------
     * 14. 创建输入 MPP Buffer
     * ------------------------------------------------
     */
    ret = mpp_buffer_get(
        enc->buf_group,
        &enc->buffer,
        enc->frame_size
    );

    if (ret != MPP_OK)
    {
        printf(
            "mpp_buffer_get failed, ret = %d\n",
            ret
        );

        goto ERROR;
    }


    enc->mpp_ptr =
        mpp_buffer_get_ptr(enc->buffer);


    if (enc->mpp_ptr == NULL)
    {
        printf("mpp_buffer_get_ptr failed\n");

        goto ERROR;
    }


    /*
     * ------------------------------------------------
     * 15. 获取 SPS/PPS
     * ------------------------------------------------
     */
    if (MPP_Get_Header(enc) < 0)
    {
        printf("MPP_Get_Header failed\n");

        goto ERROR;
    }


    printf(
        "MPP Init success: "
        "%dx%d stride=%dx%d\n",
        enc->width,
        enc->height,
        enc->hor_stride,
        enc->ver_stride
    );

    return 0;


ERROR:

    if (cfg)
    {
        mpp_enc_cfg_deinit(cfg);
    }


    if (enc->buffer)
    {
        mpp_buffer_put(enc->buffer);
        enc->buffer = NULL;
    }


    if (enc->buf_group)
    {
        mpp_buffer_group_put(enc->buf_group);
        enc->buf_group = NULL;
    }


    if (enc->ctx)
    {
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
    }


    enc->mpi = NULL;

    return -1;
}
/*
 * ============================================================
 * 确保 H264 输出 buffer 容量足够
 *
 * packet_ptr 是我们自己维护的内存，
 * 用于保存 MPP 输出的 H264 数据。
 *
 * need_size:
 *      当前至少需要的容量
 *
 * 返回：
 *      0  成功
 *     -1  失败
 * ============================================================
 */
static int MPP_Ensure_Packet_Capacity(
    MPP_Encode_cfg *enc,
    size_t need_size
)
{
    if (enc == NULL ||
        need_size == 0)
    {
        return -1;
    }

    /*
     * 当前空间已经够用。
     */
    if (enc->packet_capacity >= need_size)
    {
        return 0;
    }

    /*
     * 预留一点额外空间，
     * 避免每一帧大小稍有变化就不断 realloc。
     */
    size_t new_capacity =
        need_size + 64 * 1024;

    uint8_t *new_ptr =
        realloc(
            enc->packet_ptr,
            new_capacity
        );

    if (new_ptr == NULL)
    {
        printf(
            "realloc packet buffer failed, "
            "need=%zu\n",
            need_size
        );

        return -1;
    }

    enc->packet_ptr =
        new_ptr;

    enc->packet_capacity =
        new_capacity;

    return 0;
}


/*
 * ============================================================
 * 编码一帧 NV12
 *
 * 输入：
 *
 * V4L2 NV12
 *
 *      ↓
 *
 * memcpy / stride处理
 *
 *      ↓
 *
 * MPP Buffer
 *
 *      ↓
 *
 * MppFrame
 *
 *      ↓
 *
 * encode_put_frame
 *
 *      ↓
 *
 * encode_get_packet
 *
 *      ↓
 *
 * H264
 *
 *      ↓
 *
 * enc->packet_ptr
 * enc->len
 *
 *
 * 第一次编码时：
 *
 * SPS/PPS + 第一帧H264
 *
 * 后续：
 *
 * 普通H264 packet
 *
 *
 * 返回：
 *
 *      0  成功
 *     -1  失败
 * ============================================================
 */
int MPP_Encode(
    MPP_Encode_cfg *enc,
    const void *src,
    size_t src_size,
    int src_stride
)
{
    if (enc == NULL ||
        src == NULL)
    {
        return -1;
    }

    if (enc->ctx == NULL ||
        enc->mpi == NULL ||
        enc->buffer == NULL ||
        enc->mpp_ptr == NULL)
    {
        printf(
            "MPP encoder is not initialized\n"
        );

        return -1;
    }

    if (enc->width <= 0 ||
        enc->height <= 0 ||
        enc->hor_stride <= 0 ||
        enc->ver_stride <= 0 ||
        src_stride <= 0)
    {
        printf(
            "invalid MPP encode parameters\n"
        );

        return -1;
    }


    /*
     * ========================================================
     * 1. 检查源NV12数据长度
     *
     * NV12：
     *
     * Y：
     *      src_stride * height
     *
     * UV：
     *      src_stride * height / 2
     * ========================================================
     */
    size_t required_src_size =
        (size_t)src_stride *
        enc->height *
        3 / 2;


    if (src_size < required_src_size)
    {
        printf(
            "NV12 source data too small: "
            "src_size=%zu required=%zu\n",
            src_size,
            required_src_size
        );

        return -1;
    }


    /*
     * ========================================================
     * 2. 将V4L2 NV12复制到MPP输入Buffer
     *
     * 这里不直接：
     *
     * memcpy(mpp_ptr, src, frame_size)
     *
     * 而是按照stride逐行复制。
     *
     * 这样即使：
     *
     * V4L2 src_stride
     *
     * 和
     *
     * MPP hor_stride
     *
     * 不一样，也可以正常处理。
     * ========================================================
     */

    uint8_t *dst =
        (uint8_t *)enc->mpp_ptr;

    const uint8_t *src_ptr =
        (const uint8_t *)src;


    /*
     * 先把整个MPP Buffer清零。
     *
     * 这样stride padding部分不会存在旧数据。
     */
    memset(
        dst,
        0,
        enc->frame_size
    );


    /*
     * --------------------------------------------------------
     * Y Plane
     * --------------------------------------------------------
     *
     * 每行真正有效数据：
     *
     * width 字节
     */
    for (int y = 0;
         y < enc->height;
         y++)
    {
        const uint8_t *src_row =
            src_ptr +
            (size_t)y *
            src_stride;


        uint8_t *dst_row =
            dst +
            (size_t)y *
            enc->hor_stride;


        memcpy(
            dst_row,
            src_row,
            enc->width
        );
    }


    /*
     * --------------------------------------------------------
     * UV Plane
     *
     * NV12：
     *
     * UVUVUVUV...
     *
     * 行数：
     *
     * height / 2
     * --------------------------------------------------------
     */

    const uint8_t *src_uv =
        src_ptr +
        (size_t)src_stride *
        enc->height;


    uint8_t *dst_uv =
        dst +
        (size_t)enc->hor_stride *
        enc->ver_stride;


    for (int y = 0;
         y < enc->height / 2;
         y++)
    {
        const uint8_t *src_row =
            src_uv +
            (size_t)y *
            src_stride;


        uint8_t *dst_row =
            dst_uv +
            (size_t)y *
            enc->hor_stride;


        /*
         * NV12每行UV有效数据同样是width字节。
         */
        memcpy(
            dst_row,
            src_row,
            enc->width
        );
    }


    /*
     * ========================================================
     * 3. 创建 MppFrame
     * ========================================================
     */

    MppFrame frame = NULL;


    MPP_RET ret =
        mpp_frame_init(
            &frame
        );


    if (ret != MPP_OK)
    {
        printf(
            "mpp_frame_init failed, ret=%d\n",
            ret
        );

        return -1;
    }


    /*
     * ========================================================
     * 4. 设置当前帧参数
     *
     * 注意：
     *
     * MPP_Init中的prep配置描述编码器输入格式。
     *
     * 这里描述的是“当前真正送进去的这一帧”。
     * ========================================================
     */

    mpp_frame_set_width(
        frame,
        enc->width
    );


    mpp_frame_set_height(
        frame,
        enc->height
    );


    mpp_frame_set_hor_stride(
        frame,
        enc->hor_stride
    );


    mpp_frame_set_ver_stride(
        frame,
        enc->ver_stride
    );


    /*
     * 摄像头输入：
     *
     * NV12
     */
    mpp_frame_set_fmt(
        frame,
        MPP_FMT_YUV420SP
    );


    /*
     * 将前面申请好的MPP Buffer
     * 绑定给当前Frame。
     */
    mpp_frame_set_buffer(
        frame,
        enc->buffer
    );


    /*
     * 当前不是EOS。
     */
    mpp_frame_set_eos(
        frame,
        0
    );


    /*
     * ========================================================
     * 5. 将Frame送入MPP编码器
     * ========================================================
     */

    ret =
        enc->mpi->encode_put_frame(
            enc->ctx,
            frame
        );


    if (ret != MPP_OK)
    {
        printf(
            "encode_put_frame failed, ret=%d\n",
            ret
        );


        mpp_frame_deinit(
            &frame
        );


        return -1;
    }


    /*
     * ========================================================
     * 6. 获取H264编码结果
     *
     * 由于初始化时设置：
     *
     * MPP_SET_OUTPUT_TIMEOUT
     * =
     * MPP_POLL_BLOCK
     *
     * 所以这里会等待编码结果。
     * ========================================================
     */

    MppPacket packet = NULL;


    ret =
        enc->mpi->encode_get_packet(
            enc->ctx,
            &packet
        );


    if (ret != MPP_OK)
    {
        printf(
            "encode_get_packet failed, ret=%d\n",
            ret
        );


        mpp_frame_deinit(
            &frame
        );


        return -1;
    }


    /*
     * Frame对象到这里已经可以释放。
     *
     * enc->buffer本身仍由enc结构体长期持有。
     */
    mpp_frame_deinit(
        &frame
    );


    /*
     * 理论上阻塞模式应该获得packet。
     *
     * 仍然检查一下。
     */
    if (packet == NULL)
    {
        printf(
            "encode_get_packet returned NULL packet\n"
        );

        return -1;
    }


    /*
     * ========================================================
     * 7. 获取MPP输出H264数据
     * ========================================================
     */

    void *packet_data =
        mpp_packet_get_pos(
            packet
        );


    size_t packet_len =
        mpp_packet_get_length(
            packet
        );


    if (packet_data == NULL ||
        packet_len == 0)
    {
        printf(
            "invalid MPP packet: ptr=%p len=%zu\n",
            packet_data,
            packet_len
        );


        mpp_packet_deinit(
            &packet
        );


        return -1;
    }


    /*
     * ========================================================
     * 8. 第一个Packet：
     *
     * SPS/PPS + H264 Frame
     *
     * 为什么？
     *
     * PC端FFmpeg Decoder必须先获得：
     *
     * SPS
     * PPS
     *
     * 才知道：
     *
     * 分辨率
     * profile
     * level
     * 解码参数
     *
     * 当前main线程只会发送：
     *
     * enc->packet_ptr
     * enc->len
     *
     * 所以最简单的方案就是把header和第一帧拼起来。
     * ========================================================
     */

    if (!enc->header_sent)
    {
        size_t total_len =
            enc->header_len +
            packet_len;


        /*
         * 确保输出buffer足够大。
         */
        if (MPP_Ensure_Packet_Capacity(
                enc,
                total_len) < 0)
        {
            mpp_packet_deinit(
                &packet
            );


            return -1;
        }


        /*
         * SPS / PPS
         */
        if (enc->header_ptr != NULL &&
            enc->header_len > 0)
        {
            memcpy(
                enc->packet_ptr,
                enc->header_ptr,
                enc->header_len
            );
        }


        /*
         * 第一帧H264数据
         */
        memcpy(
            enc->packet_ptr +
                enc->header_len,

            packet_data,

            packet_len
        );


        enc->len =
            total_len;


        enc->header_sent =
            1;


        printf(
            "Send first H264 packet: "
            "header=%zu frame=%zu total=%zu\n",
            enc->header_len,
            packet_len,
            enc->len
        );
    }


    /*
     * ========================================================
     * 9. 后续Packet
     *
     * 直接保存MPP输出。
     * ========================================================
     */
    else
    {
        if (MPP_Ensure_Packet_Capacity(
                enc,
                packet_len) < 0)
        {
            mpp_packet_deinit(
                &packet
            );


            return -1;
        }


        memcpy(
            enc->packet_ptr,
            packet_data,
            packet_len
        );


        enc->len =
            packet_len;
    }


    /*
     * ========================================================
     * 10. MPP Packet内部数据已经复制完毕
     *
     * 可以安全释放。
     *
     * 这也是为什么main中的：
     *
     * TCP_Client_SendH264(
     *     client,
     *     enc->packet_ptr,
     *     enc->len
     * );
     *
     * 是安全的。
     *
     * 因为packet_ptr已经不是MPP内部指针。
     * ========================================================
     */

    mpp_packet_deinit(
        &packet
    );


    return 0;
}


/*
 * ============================================================
 * 释放MPP编码器资源
 * ============================================================
 */
void MPP_Free(
    MPP_Encode_cfg *enc
)
{
    if (enc == NULL)
    {
        return;
    }


    /*
     * ========================================================
     * 1. H264输出Buffer
     * ========================================================
     */
    if (enc->packet_ptr != NULL)
    {
        free(
            enc->packet_ptr
        );


        enc->packet_ptr =
            NULL;
    }


    enc->packet_capacity =
        0;


    enc->len =
        0;


    /*
     * ========================================================
     * 2. SPS / PPS Buffer
     * ========================================================
     */
    if (enc->header_ptr != NULL)
    {
        free(
            enc->header_ptr
        );


        enc->header_ptr =
            NULL;
    }


    enc->header_len =
        0;


    enc->header_sent =
        0;


    /*
     * ========================================================
     * 3. MPP输入Buffer
     * ========================================================
     */
    if (enc->buffer != NULL)
    {
        mpp_buffer_put(
            enc->buffer
        );


        enc->buffer =
            NULL;
    }


    enc->mpp_ptr =
        NULL;


    /*
     * ========================================================
     * 4. MPP Buffer Group
     * ========================================================
     */
    if (enc->buf_group != NULL)
    {
        mpp_buffer_group_put(
            enc->buf_group
        );


        enc->buf_group =
            NULL;
    }


    /*
     * ========================================================
     * 5. MPP Context
     * ========================================================
     */
    if (enc->ctx != NULL)
    {
        mpp_destroy(
            enc->ctx
        );


        enc->ctx =
            NULL;
    }


    enc->mpi =
        NULL;


    enc->frame_size =
        0;


    printf(
        "MPP encoder released\n"
    );
}