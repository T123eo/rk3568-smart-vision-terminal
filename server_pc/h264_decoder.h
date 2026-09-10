#ifndef H264_DECODER_H
#define H264_DECODER_H

#include <stdint.h>
#include <stddef.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>


/*
 * ============================================================
 * 解码成功一帧后的回调函数
 *
 * frame：
 *      FFmpeg解码得到的AVFrame
 *
 * frame_index：
 *      当前是第几帧
 *
 * opaque：
 *      用户自定义参数
 *
 *
 * 返回：
 *
 *      0   继续解码
 *     非0  中止当前解码流程
 *
 *
 * 注意：
 *
 * frame属于decoder内部复用的AVFrame。
 *
 * 如果以后要把frame放进FrameQueue交给SDL线程，
 * 不能简单保存这个指针。
 *
 * 必须使用：
 *
 *      av_frame_clone()
 *
 * 或：
 *
 *      av_frame_ref()
 *
 * 建立独立引用。
 * ============================================================
 */
typedef int (*H264_Frame_Callback)(
    const AVFrame *frame,
    uint64_t frame_index,
    void *opaque
);


/*
 * ============================================================
 * H264 Decoder控制结构
 * ============================================================
 */
typedef struct
{
    /*
     * H264解码器
     */
    const AVCodec *codec;


    /*
     * FFmpeg解码上下文
     */
    AVCodecContext *codec_ctx;


    /*
     * H264 parser
     *
     * 用于处理连续H264字节流，
     * 不依赖TCP packet边界。
     */
    AVCodecParserContext *parser;


    /*
     * 送入decoder的压缩数据
     */
    AVPacket *packet;


    /*
     * decoder输出的原始视频帧
     */
    AVFrame *frame;


    /*
     * 已经成功解码出的帧数量
     */
    uint64_t frame_count;


    /*
     * 是否已经初始化
     */
    int initialized;

} H264_Decoder;


/*
 * ============================================================
 * 初始化H264 Decoder
 *
 * 内部完成：
 *
 * avcodec_find_decoder
 * av_parser_init
 * avcodec_alloc_context3
 * avcodec_open2
 * av_packet_alloc
 * av_frame_alloc
 *
 *
 * 返回：
 *
 *      0   成功
 *     -1   失败
 * ============================================================
 */
int H264_Decoder_Init(
    H264_Decoder *decoder
);


/*
 * ============================================================
 * 输入一段H264数据进行解码
 *
 * data：
 *      H264压缩数据
 *
 * len：
 *      数据长度
 *
 * callback：
 *      每成功得到一个AVFrame就调用一次
 *
 * opaque：
 *      传给callback的用户参数
 *
 *
 * 返回：
 *
 *      >=0 当前输入数据产生的AVFrame数量
 *
 *      <0  解码错误
 * ============================================================
 */
int H264_Decoder_Decode(
    H264_Decoder *decoder,
    const uint8_t *data,
    size_t len,
    H264_Frame_Callback callback,
    void *opaque
);


/*
 * ============================================================
 * Flush
 *
 * 当TCP连接结束、H264数据发送结束以后调用。
 *
 * 作用：
 *
 * 把parser和decoder内部缓存的最后几帧全部输出。
 *
 *
 * 注意：
 *
 * Flush之后不要再继续调用Decode。
 *
 * 返回：
 *
 *      >=0 flush产生的AVFrame数量
 *
 *      <0  错误
 * ============================================================
 */
int H264_Decoder_Flush(
    H264_Decoder *decoder,
    H264_Frame_Callback callback,
    void *opaque
);


/*
 * ============================================================
 * 销毁Decoder
 * ============================================================
 */
void H264_Decoder_Destroy(
    H264_Decoder *decoder
);


#endif