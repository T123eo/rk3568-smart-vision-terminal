#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <linux/videodev2.h>
#include<sys/mman.h>
#include <string.h>
#include "imx415.h"

void handle_error(char *ptr ,int fd)
{
    if(fd<0)
    {
        perror(ptr);
        exit(EXIT_FAILURE);
    }
}
void Imx415_Init(Camera_handle*camera)
{
    int temp=-1;
    camera->fd=open(CAMERA_PATH,O_RDWR);
    handle_error("open",camera->fd);

    struct v4l2_capability cap={0};
    temp=ioctl(camera->fd,VIDIOC_QUERYCAP,&cap);
    handle_error("querycap",temp);
    /*
    相比于usb uvc的单平面采集接口,现在的imx415采用多平面采集接口是buf描述一帧整体是什么，具体是由plane
    */
    if(cap.capabilities&V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        printf("支持摄像头采集\n");
    }
    if(cap.capabilities&V4L2_CAP_STREAMING)
    {
        printf("支持流采集\n");
    }

    /*查看设备支持的输出格式*/
    /*这里的type指的是指定数据输出的格式*/
    struct v4l2_fmtdesc fmtdesc={0};
    fmtdesc.index=0;
    fmtdesc.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    unsigned char* p=(unsigned char*)&fmtdesc.pixelformat;
    for (fmtdesc.index=0;;fmtdesc.index++)
    {
        if(ioctl(camera->fd,VIDIOC_ENUM_FMT,&fmtdesc)<0)break;
        printf("index%d   pixel:%c%c%c%c\n",fmtdesc.index,p[0],p[1],p[2],p[3]);
    }


    /*获取对应输出格式的像素*/
    /*这里的type是指的是像素可能支持的类型 离散 步进 连续*/
    __u32 select_format=V4L2_PIX_FMT_NV12;
    struct v4l2_frmsizeenum frmsize={0};
    frmsize.pixel_format=select_format;
    frmsize.index=0;
    for (frmsize.index=0;;frmsize.index++)
    {
        if(ioctl(camera->fd,VIDIOC_ENUM_FRAMESIZES,&frmsize)<0)break;
        switch (frmsize.type)
        {
            case V4L2_FRMSIZE_TYPE_DISCRETE:
            {
                printf("index%d size:%dx%d\n",frmsize.index,frmsize.discrete.width,frmsize.discrete.height);
            break;
            }
            case V4L2_FRMSIZE_TYPE_STEPWISE:
            {
                printf("stepwise\n");
                printf("width :%d~%d  step:%d\n",frmsize.stepwise.min_width,frmsize.stepwise.max_width,frmsize.stepwise.step_width);
                printf("height :%d~%d  step:%d\n",frmsize.stepwise.min_height,frmsize.stepwise.max_height,frmsize.stepwise.step_height);
            break;
            }
            case V4L2_FRMSIZE_TYPE_CONTINUOUS:
            {
                printf("continuous\n");
            break;
            }
            default:break;
        }
    }

    /*像素设置*/
    struct v4l2_format fmt={0};
    fmt.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width=1080;
    fmt.fmt.pix_mp.height=1920;
    fmt.fmt.pix_mp.pixelformat=V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field=V4L2_FIELD_ANY;
    temp=ioctl(camera->fd,VIDIOC_S_FMT,&fmt);
    handle_error("setfmt",temp);
    camera->num_planes=fmt.fmt.pix_mp.num_planes;
    camera->width=fmt.fmt.pix_mp.width;
    camera->height=fmt.fmt.pix_mp.height;
    camera->pixelformat=fmt.fmt.pix_mp.pixelformat;
    printf("actual width:%d\n",camera->width);
    printf("actual height:%d\n",camera->height);
    printf("num_plane:%d\n",camera->num_planes);
    for (size_t i = 0; i < camera->num_planes; i++)
    {
        camera->bytesperline[i]=fmt.fmt.pix_mp.plane_fmt[i].bytesperline;
        camera->sizeimage[i]=fmt.fmt.pix_mp.plane_fmt[i].sizeimage;
    }
    

    /*申请内核空间*/
    struct v4l2_requestbuffers req_buf={0};
    req_buf.count=CAMERA_BUF_COUNT;
    req_buf.memory=V4L2_MEMORY_MMAP;
    req_buf.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    temp=ioctl(camera->fd,VIDIOC_REQBUFS,&req_buf);
    handle_error("reqbuf",temp);
    camera->buf_count=req_buf.count;
    printf("申请内涵空间数:%d\n",req_buf.count);

    /*获取已经申请好的内核信息并映射*/
    for (size_t i = 0; i < req_buf.count; i++)
    {
        struct v4l2_buffer buf={0};
        struct v4l2_plane plane[camera->num_planes];
        memset(plane, 0, sizeof(plane));

        buf.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.index=i;
        buf.memory=V4L2_MEMORY_MMAP;

        buf.m.planes=plane;
        buf.length=camera->num_planes;
        temp=ioctl(camera->fd,VIDIOC_QUERYBUF,&buf);
        handle_error("querybuf",temp);
        printf("index:%ld length:%d offset:%d\n",i,plane[0].length,plane->m.mem_offset);
        camera->buffer[i].length=plane->length;
        camera->buffer[i].start=mmap(NULL,plane[0].length,PROT_READ|PROT_WRITE,MAP_SHARED,camera->fd,plane[0].m.mem_offset);
        if(camera->buffer[i].start==MAP_FAILED)handle_error("mapfailed",-1);
    }

    /*将申请好的空间放到驱动队列里面*/
    for (size_t i = 0; i < req_buf.count; i++)
    {
        struct v4l2_buffer buf={0};
        struct v4l2_plane plane[camera->num_planes];
        memset(plane, 0, sizeof(plane));
        buf.type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.index=i;
        buf.memory=V4L2_MEMORY_MMAP;

        buf.m.planes=plane;
        buf.length=camera->num_planes;
        temp=ioctl(camera->fd,VIDIOC_QBUF,&buf);
        handle_error("Qbuf",temp);
    }
    /*启动数据流采集*/
    enum v4l2_buf_type type=V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;;
    temp=ioctl(camera->fd,VIDIOC_STREAMON,&type);
    handle_error("VIDIOC_STREAMON", temp);
}



   