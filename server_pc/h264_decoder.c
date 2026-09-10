#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <libavcodec/avcodec.h>

#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "h264_decoder.h"


/*
 * ============================================================
 * 打印FFmpeg错误
 * ============================================================
 */
static void print_ffmpeg_error(
    const char *msg,
    int error_code
)
{
    char error_buf[AV_ERROR_MAX_STRING_SIZE];

    memset(
        error_buf,
        0,
        sizeof(error_buf)
    );


    if (av_strerror(
            error_code,
            error_buf,
            sizeof(error_buf)) < 0)
    {
        fprintf(
            stderr,
            "%s: FFmpeg error=%d\n",
            msg,
            error_code
        );

        return;
    }


    fprintf(
        stderr,
        "%s: %s\n",
        msg,
        error_buf
    );
}


/*
 * ============================================================
 * 从Decoder中取出所有当前已经可以输出的AVFrame
 *
 * 为什么是while？
 *
 * 因为：
 *
 * avcodec_send_packet()
 *
 * 和
 *
 * avcodec_receive_frame()
 *
 * 并不是严格一包对应一帧。
 *
 * 一次send以后可能：
 *
 *      0帧
 *      1帧
 *      多帧
 *
 * 所以必须循环receive。
 *
 *
 * 返回：
 *
 *      >=0 当前取出的帧数量
 *
 *      <0  错误
 * ============================================================
 */
static int receive_frames(
    H264_Decoder *decoder,
    H264_Frame_Callback callback,
    void *opaque
)
{
    int output_frames = 0;


    while (1)
    {
        /*
         * 从decoder取一个解码完成的AVFrame
         */
        int ret =
            avcodec_receive_frame(
                decoder->codec_ctx,
                decoder->frame
            );


        /*
         * ----------------------------------------------------
         * EAGAIN
         *
         * 表示当前已经没有更多输出帧。
         *
         * 需要继续send新的H264数据。
         * ----------------------------------------------------
         */
        if (ret == AVERROR(EAGAIN))
        {
            break;
        }


        /*
         * ----------------------------------------------------
         * EOF
         *
         * decoder已经flush完成。
         * ----------------------------------------------------
         */
        if (ret == AVERROR_EOF)
        {
            break;
        }


        /*
         * ----------------------------------------------------
         * 真正的解码错误
         * ----------------------------------------------------
         */
        if (ret < 0)
        {
            print_ffmpeg_error(
                "avcodec_receive_frame failed",
                ret
            );

            return -1;
        }


        /*
         * 成功得到一帧
         */
        decoder->frame_count++;

        output_frames++;


        /*
         * ----------------------------------------------------
         * 如果上层提供callback，
         * 把AVFrame交给上层。
         * ----------------------------------------------------
         */
        if (callback != NULL)
        {
            int callback_ret =
                callback(
                    decoder->frame,
                    decoder->frame_count,
                    opaque
                );


            if (callback_ret != 0)
            {
                return callback_ret;
            }
        }
    }


    return output_frames;
}


/*
 * ============================================================
 * 将一个已经经过parser处理的H264 packet送入Decoder
 * ============================================================
 */
static int send_packet_to_decoder(
    H264_Decoder *decoder,
    const uint8_t *data,
    int size,
    H264_Frame_Callback callback,
    void *opaque
)
{
    if (size <= 0 ||
        data == NULL)
    {
        return 0;
    }


    /*
     * 复用AVPacket之前先清理旧引用。
     */
    av_packet_unref(
        decoder->packet
    );


    /*
     * --------------------------------------------------------
     * 给AVPacket创建自己的buffer。
     *
     * 这里故意进行一次copy。
     *
     * 好处：
     *
     * AVPacket拥有独立的数据生命周期，
     * 不依赖parser输出buffer和TCP Queue。
     *
     * 现阶段优先保证正确性。
     * --------------------------------------------------------
     */

    int ret =
        av_new_packet(
            decoder->packet,
            size
        );


    if (ret < 0)
    {
        print_ffmpeg_error(
            "av_new_packet failed",
            ret
        );

        return -1;
    }


    memcpy(
        decoder->packet->data,
        data,
        (size_t)size
    );


    /*
     * --------------------------------------------------------
     * 将压缩packet送入H264 decoder
     * --------------------------------------------------------
     */

    while (1)
    {
        ret =
            avcodec_send_packet(
                decoder->codec_ctx,
                decoder->packet
            );


        /*
         * ----------------------------------------------------
         * 如果send返回EAGAIN：
         *
         * decoder内部还有输出帧没有取完。
         *
         * 先receive，
         * 然后重新send当前packet。
         * ----------------------------------------------------
         */
        if (ret == AVERROR(EAGAIN))
        {
            int frame_ret =
                receive_frames(
                    decoder,
                    callback,
                    opaque
                );


            if (frame_ret < 0)
            {
                av_packet_unref(
                    decoder->packet
                );

                return frame_ret;
            }


            continue;
        }


        if (ret < 0)
        {
            print_ffmpeg_error(
                "avcodec_send_packet failed",
                ret
            );


            av_packet_unref(
                decoder->packet
            );


            return -1;
        }


        break;
    }


    /*
     * packet已经成功送入decoder。
     *
     * avcodec_send_packet返回之后，
     * 可以释放当前AVPacket引用。
     */
    av_packet_unref(
        decoder->packet
    );


    /*
     * --------------------------------------------------------
     * 尝试取出所有可以输出的帧
     * --------------------------------------------------------
     */

    return receive_frames(
        decoder,
        callback,
        opaque
    );
}


/*
 * ============================================================
 * H264_Decoder_Init
 * ============================================================
 */
int H264_Decoder_Init(
    H264_Decoder *decoder
)
{
    if (decoder == NULL)
    {
        fprintf(
            stderr,
            "H264_Decoder_Init: decoder is NULL\n"
        );

        return -1;
    }


    /*
     * 整个结构体清零
     */
    memset(
        decoder,
        0,
        sizeof(*decoder)
    );


    /*
     * --------------------------------------------------------
     * 1. 查找H264 Decoder
     * --------------------------------------------------------
     */

    decoder->codec =
        avcodec_find_decoder(
            AV_CODEC_ID_H264
        );


    if (decoder->codec == NULL)
    {
        fprintf(
            stderr,
            "H264 decoder not found\n"
        );

        return -1;
    }


    /*
     * --------------------------------------------------------
     * 2. 创建H264 Parser
     *
     * 作用：
     *
     * TCP收到的是连续H264字节数据。
     *
     * Parser负责从字节流中识别真正可以交给
     * decoder的数据边界。
     * --------------------------------------------------------
     */

    decoder->parser =
        av_parser_init(
            AV_CODEC_ID_H264
        );


    if (decoder->parser == NULL)
    {
        fprintf(
            stderr,
            "av_parser_init H264 failed\n"
        );

        H264_Decoder_Destroy(
            decoder
        );

        return -1;
    }


    /*
     * --------------------------------------------------------
     * 3. 创建AVCodecContext
     * --------------------------------------------------------
     */

    decoder->codec_ctx =
        avcodec_alloc_context3(
            decoder->codec
        );


    if (decoder->codec_ctx == NULL)
    {
        fprintf(
            stderr,
            "avcodec_alloc_context3 failed\n"
        );

        H264_Decoder_Destroy(
            decoder
        );

        return -1;
    }


    /*
     * --------------------------------------------------------
     * 4. 打开H264 Decoder
     * --------------------------------------------------------
     */

    int ret =
        avcodec_open2(
            decoder->codec_ctx,
            decoder->codec,
            NULL
        );


    if (ret < 0)
    {
        print_ffmpeg_error(
            "avcodec_open2 failed",
            ret
        );


        H264_Decoder_Destroy(
            decoder
        );


        return -1;
    }


    /*
     * --------------------------------------------------------
     * 5. 创建AVPacket
     * --------------------------------------------------------
     */

    decoder->packet =
        av_packet_alloc();


    if (decoder->packet == NULL)
    {
        fprintf(
            stderr,
            "av_packet_alloc failed\n"
        );


        H264_Decoder_Destroy(
            decoder
        );


        return -1;
    }


    /*
     * --------------------------------------------------------
     * 6. 创建AVFrame
     * --------------------------------------------------------
     */

    decoder->frame =
        av_frame_alloc();


    if (decoder->frame == NULL)
    {
        fprintf(
            stderr,
            "av_frame_alloc failed\n"
        );


        H264_Decoder_Destroy(
            decoder
        );


        return -1;
    }


    decoder->frame_count = 0;

    decoder->initialized = 1;


    printf(
        "H264 decoder initialized: %s\n",
        decoder->codec->name
    );


    return 0;
}


/*
 * ============================================================
 * H264_Decoder_Decode
 * ============================================================
 */
int H264_Decoder_Decode(
    H264_Decoder *decoder,
    const uint8_t *data,
    size_t len,
    H264_Frame_Callback callback,
    void *opaque
)
{
    if (decoder == NULL ||
        data == NULL ||
        len == 0)
    {
        return -1;
    }


    if (!decoder->initialized ||
        decoder->codec_ctx == NULL ||
        decoder->parser == NULL)
    {
        fprintf(
            stderr,
            "H264 decoder is not initialized\n"
        );

        return -1;
    }


    /*
     * av_parser_parse2的buf_size是int。
     *
     * 你的TCP单packet最大限制目前只有8MB，
     * 正常不会超过INT_MAX。
     */
    if (len > INT_MAX)
    {
        fprintf(
            stderr,
            "H264 input too large: %zu\n",
            len
        );

        return -1;
    }


    /*
     * --------------------------------------------------------
     * FFmpeg parser要求输入buffer尾部存在：
     *
     * AV_INPUT_BUFFER_PADDING_SIZE
     *
     * 字节的安全padding。
     *
     * Queue里的buffer只有真实payload长度，
     * 所以这里重新创建：
     *
     * len + AV_INPUT_BUFFER_PADDING_SIZE
     *
     * 并把padding区域清零。
     * --------------------------------------------------------
     */

    if (len >
        SIZE_MAX - AV_INPUT_BUFFER_PADDING_SIZE)
    {
        return -1;
    }


    uint8_t *input_buffer =
        (uint8_t *)av_malloc(
            len +
            AV_INPUT_BUFFER_PADDING_SIZE
        );


    if (input_buffer == NULL)
    {
        fprintf(
            stderr,
            "av_malloc input buffer failed\n"
        );

        return -1;
    }


    memcpy(
        input_buffer,
        data,
        len
    );


    memset(
        input_buffer + len,
        0,
        AV_INPUT_BUFFER_PADDING_SIZE
    );


    uint8_t *input =
        input_buffer;


    int input_size =
        (int)len;


    int total_frames = 0;


    /*
     * --------------------------------------------------------
     * Parser循环解析当前输入数据
     * --------------------------------------------------------
     */

    while (input_size > 0)
    {
        uint8_t *parsed_data =
            NULL;


        int parsed_size =
            0;


        /*
         * av_parser_parse2返回：
         *
         * 当前消耗了多少输入字节。
         */
        int consumed =
            av_parser_parse2(
                decoder->parser,
                decoder->codec_ctx,

                &parsed_data,
                &parsed_size,

                input,
                input_size,

                AV_NOPTS_VALUE,
                AV_NOPTS_VALUE,

                0
            );


        if (consumed < 0)
        {
            fprintf(
                stderr,
                "av_parser_parse2 failed\n"
            );


            av_free(
                input_buffer
            );


            return -1;
        }


        /*
         * 输入指针前移
         */
        input += consumed;

        input_size -= consumed;


        /*
         * ----------------------------------------------------
         * Parser已经解析出完整数据。
         * ----------------------------------------------------
         */
        if (parsed_size > 0)
        {
            int frames =
                send_packet_to_decoder(
                    decoder,
                    parsed_data,
                    parsed_size,
                    callback,
                    opaque
                );


            if (frames < 0)
            {
                av_free(
                    input_buffer
                );


                return frames;
            }


            total_frames +=
                frames;
        }


        /*
         * 理论上parser应该不断消费输入。
         *
         * 这个判断用于防止异常情况下死循环。
         */
        if (consumed == 0 &&
            parsed_size == 0)
        {
            break;
        }
    }


    av_free(
        input_buffer
    );


    return total_frames;
}


/*
 * ============================================================
 * H264_Decoder_Flush
 *
 * TCP数据流结束以后调用。
 * ============================================================
 */
int H264_Decoder_Flush(
    H264_Decoder *decoder,
    H264_Frame_Callback callback,
    void *opaque
)
{
    if (decoder == NULL ||
        !decoder->initialized)
    {
        return -1;
    }


    int total_frames = 0;


    /*
     * ========================================================
     * 第一步：
     *
     * Flush parser
     *
     * buf_size = 0表示EOF，
     * 让parser把最后缓存的数据吐出来。
     * ========================================================
     */

    uint8_t *parsed_data =
        NULL;


    int parsed_size =
        0;


    int consumed =
        av_parser_parse2(
            decoder->parser,
            decoder->codec_ctx,

            &parsed_data,
            &parsed_size,

            NULL,
            0,

            AV_NOPTS_VALUE,
            AV_NOPTS_VALUE,

            0
        );


    if (consumed < 0)
    {
        fprintf(
            stderr,
            "Flush av_parser_parse2 failed\n"
        );

        return -1;
    }


    /*
     * parser还有最后一个packet。
     */
    if (parsed_size > 0)
    {
        int frames =
            send_packet_to_decoder(
                decoder,
                parsed_data,
                parsed_size,
                callback,
                opaque
            );


        if (frames < 0)
        {
            return frames;
        }


        total_frames += frames;
    }


    /*
     * ========================================================
     * 第二步：
     *
     * 给Decoder发送NULL packet
     *
     * 表示：
     *
     * “输入数据已经全部结束，
     *  请输出内部剩余的缓存帧。”
     * ========================================================
     */

    int ret;


    while (1)
    {
        ret =
            avcodec_send_packet(
                decoder->codec_ctx,
                NULL
            );


        /*
         * 还有输出没有取完。
         */
        if (ret == AVERROR(EAGAIN))
        {
            int frames =
                receive_frames(
                    decoder,
                    callback,
                    opaque
                );


            if (frames < 0)
            {
                return frames;
            }


            total_frames += frames;

            continue;
        }


        /*
         * 已经flush过。
         */
        if (ret == AVERROR_EOF)
        {
            return total_frames;
        }


        if (ret < 0)
        {
            print_ffmpeg_error(
                "Flush avcodec_send_packet failed",
                ret
            );

            return -1;
        }


        break;
    }


    /*
     * ========================================================
     * 第三步：
     *
     * 取出decoder内部剩余所有AVFrame
     * ========================================================
     */

    int frames =
        receive_frames(
            decoder,
            callback,
            opaque
        );


    if (frames < 0)
    {
        return frames;
    }


    total_frames += frames;


    return total_frames;
}


/*
 * ============================================================
 * H264_Decoder_Destroy
 * ============================================================
 */
void H264_Decoder_Destroy(
    H264_Decoder *decoder
)
{
    if (decoder == NULL)
    {
        return;
    }


    /*
     * --------------------------------------------------------
     * 1. Parser
     * --------------------------------------------------------
     */

    if (decoder->parser != NULL)
    {
        av_parser_close(
            decoder->parser
        );


        decoder->parser =
            NULL;
    }


    /*
     * --------------------------------------------------------
     * 2. AVPacket
     * --------------------------------------------------------
     */

    if (decoder->packet != NULL)
    {
        av_packet_free(
            &decoder->packet
        );
    }


    /*
     * --------------------------------------------------------
     * 3. AVFrame
     * --------------------------------------------------------
     */

    if (decoder->frame != NULL)
    {
        av_frame_free(
            &decoder->frame
        );
    }


    /*
     * --------------------------------------------------------
     * 4. AVCodecContext
     * --------------------------------------------------------
     */

    if (decoder->codec_ctx != NULL)
    {
        avcodec_free_context(
            &decoder->codec_ctx
        );
    }


    decoder->codec =
        NULL;


    decoder->frame_count =
        0;


    decoder->initialized =
        0;
}