#include <stddef.h>
#include <stdint.h>
#include <rockchip/rk_mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 void test()
 {
    MppCtx ctx=NULL;
    MppApi *mpi=NULL;
    MPP_RET ret=-1;
    ret=mpp_create(&ctx,&mpi);
    if(ret!=0)
    {
        printf("create error\n");
    }
    ret=mpp_init(ctx,MPP_CTX_ENC,MPP_VIDEO_CodingAVC);
    if(ret!=0)
    {
        printf("init error\n");
    }
    MppEncCfg cfg;
   mpp_enc_cfg_set_s32(cfg, "prep:width", 1920);
    mpp_enc_cfg_set_s32(cfg, "prep:height", 1080);

    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", 1920);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", 1080);

    mpp_enc_cfg_set_s32(cfg,"prep:format",MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", 30);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);

    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", 30);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm", 1);

    mpp_enc_cfg_set_s32(cfg, "rc:gop", 60);

    mpp_enc_cfg_set_s32(cfg,"rc:bps_target",4 * 1024 * 1024);
    mpp_enc_cfg_init(&cfg);
    mpi->control(ctx,MPP_ENC_SET_CFG,cfg);
    

    MppBuffer buffer;
    size_t size=1080*1920*3/2;
    mpp_buffer_get(NULL,buffer,size);
    void*mpp_ptr=mpp_buffer_get_ptr(buffer);

    char*str=NULL;//外部的数据指针
    memcpy(mpp_ptr,str,size);


    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, 1920);
    mpp_frame_set_height(frame, 1080);
    mpp_frame_set_hor_stride(frame, 1920);
    mpp_frame_set_ver_stride(frame, 1080);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, buffer);
    mpi->encode_put_frame(ctx, frame);

    MppPacket packet = NULL;
    mpi->encode_get_packet(ctx, &packet);
    void *packet_ptr = mpp_packet_get_pos(packet);
    size_t len = mpp_packet_get_length(packet);
 }
