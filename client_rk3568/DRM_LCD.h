#ifndef __DRM_LCD_H__
#define __DRM_LCD_H__
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_mode.h>
#include<stdio.h>
#include <string.h>
#define LCD_PATH "/dev/dri/card0"
typedef struct
{
    int fd;  
    drmModeRes *res;
    drmModeConnector *conn;
    drmModeEncoder*enc;
    drmModeCrtc *old_crtc;
    drmModeModeInfo mode;

    uint32_t framebuffer_id;
    struct drm_mode_create_dumb dumbbuffer;


    void *map;
}LCD_handle;
void DRM_LCD_Init(LCD_handle*lcd);
void DRM_LCD_destroy(LCD_handle*lcd);
void NV12_To_RGB( unsigned char*camera_data,int width,int height,void*lcd_map,int lcd_pitch);
#endif