#ifndef __IMX415_H__
#define __IMX415_H__
#define CAMERA_PATH "/dev/video0"
#define CAMERA_BUF_COUNT 4
#define VIDEO_MAX_PLANES 8
struct my_buffer
{
    void *start;
    size_t length;
};
typedef struct
{
    int fd;
    unsigned int width;
    unsigned int height;
    unsigned int pixelformat;
    unsigned int num_planes;
    unsigned int buf_count;
    unsigned int bytesperline[VIDEO_MAX_PLANES];
    unsigned int sizeimage[VIDEO_MAX_PLANES];
    struct my_buffer buffer[CAMERA_BUF_COUNT];

}Camera_handle;

void Imx415_Init(Camera_handle*camera);
#endif