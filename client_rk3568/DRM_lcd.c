
#include "DRM_LCD.h"
static void handle_error(char*ptr)
{
    perror(ptr);
    exit(EXIT_FAILURE);
}
void DRM_LCD_Init(LCD_handle*lcd)
{
    memset(lcd,0,sizeof(*lcd));
    lcd->fd=-1;
    lcd->fd=open(LCD_PATH,O_RDWR);
    if(lcd->fd<0)handle_error("open");
    lcd->res=drmModeGetResources(lcd->fd);
    if(lcd->res==NULL)handle_error("DRM");


    for (int i = 0; i < lcd->res->count_connectors; i++)
    {
        drmModeConnector *temp=drmModeGetConnector(lcd->fd,lcd->res->connectors[i]);
        if(temp==NULL)continue;
        if(temp->connection==DRM_MODE_CONNECTED&&temp->count_modes>0)
        {
            lcd->conn=temp;
            lcd->mode=temp->modes[0];
            break;
        }
        drmModeFreeConnector(temp);
    }
    
    if(lcd->conn==NULL)handle_error("connectorFailed");
    printf("conn:%d\n",lcd->conn->connector_id);
    printf("mode:%dx%d @%dHz\n",lcd->mode.hdisplay,lcd->mode.vdisplay,lcd->mode.vrefresh);

    uint32_t crtc_id;
    if(lcd->conn->encoder_id)lcd->enc=drmModeGetEncoder(lcd->fd,lcd->conn->encoder_id);
    if(lcd->enc==NULL)handle_error("Encoder");
    crtc_id=lcd->enc->crtc_id;
    printf("enc:%d\n",lcd->enc->encoder_id);
    printf("crtc:%d\n",crtc_id);

    lcd->old_crtc=drmModeGetCrtc(lcd->fd, crtc_id);

    lcd->dumbbuffer.width=lcd->mode.hdisplay;
    lcd->dumbbuffer.height=lcd->mode.vdisplay;
    lcd->dumbbuffer.bpp=32;
    if(ioctl(lcd->fd,DRM_IOCTL_MODE_CREATE_DUMB,&lcd->dumbbuffer)<0)handle_error("dumb");
    printf("pitch         = %u\n", lcd->dumbbuffer.pitch);
    printf("size          = %llu\n",(unsigned long long)lcd->dumbbuffer.size);
    printf("handle        = %u\n", lcd->dumbbuffer.handle);


   
    if(drmModeAddFB(lcd->fd,
        lcd->mode.hdisplay,
        lcd->mode.vdisplay,
        24,
        32,
        lcd->dumbbuffer.pitch,
        lcd->dumbbuffer.handle,
        &lcd->framebuffer_id)<0)handle_error("framebuffer");
    printf("frame:%d\n",lcd->framebuffer_id);

    struct drm_mode_map_dumb map_buf={0};
    map_buf.handle=lcd->dumbbuffer.handle;
    if(ioctl(lcd->fd,DRM_IOCTL_MODE_MAP_DUMB,&map_buf)<0)handle_error("map_buf");
    printf("map offset:%llu\n", (unsigned long long)map_buf.offset);

    lcd->map=mmap(NULL,lcd->dumbbuffer.size,PROT_WRITE|PROT_READ,MAP_SHARED,lcd->fd,map_buf.offset);
    if(lcd->map==MAP_FAILED)handle_error("mmap");


    for (size_t y = 0; y < lcd->dumbbuffer.height; y++)
    {
        uint32_t*row=(uint32_t*)((uint8_t*)lcd->map+y*lcd->dumbbuffer.pitch);
        for (size_t x = 0; x < lcd->dumbbuffer.width; x++)
        {
            row[x]=0x00ff0000;
        }
    }
    printf("buffer filled with black\n");
    if(drmModeSetCrtc(lcd->fd,crtc_id,lcd->framebuffer_id,0,0,&lcd->conn->connector_id,1,&lcd->mode)<0)handle_error("setCRTC");
}

void DRM_LCD_destroy(LCD_handle*lcd)
{
if (lcd->old_crtc)
    {
        drmModeSetCrtc(lcd->fd,
                       lcd->old_crtc->crtc_id,
                       lcd->old_crtc->buffer_id,
                       lcd->old_crtc->x,
                       lcd->old_crtc->y,
                       &lcd->conn->connector_id,
                       1,
                       &lcd->old_crtc->mode);
    }
    munmap(lcd->map,lcd->dumbbuffer.size);
    drmModeRmFB(lcd->fd,lcd->framebuffer_id);
    struct drm_mode_destroy_dumb dreq={0};
    dreq.handle=lcd->dumbbuffer.handle;
    ioctl(lcd->fd,DRM_IOCTL_MODE_DESTROY_DUMB,&dreq);
    if(lcd->old_crtc)drmModeFreeCrtc(lcd->old_crtc);
    drmModeFreeEncoder(lcd->enc);
    drmModeFreeConnector(lcd->conn);
    drmModeFreeResources(lcd->res);
    close(lcd->fd);
}

void NV12_To_RGB( unsigned char*camera_data,int width,int height,void*lcd_map,int lcd_pitch)
{
    unsigned char*Y_data=camera_data;
    unsigned char*UV_data=camera_data+height*width;
    for (int y = 0; y < height; y++)
    {
        uint32_t *lcd_line =(uint32_t *)((unsigned char *)lcd_map+ y * lcd_pitch);
        for (int x = 0; x < width; x++)
        {
            int Y=Y_data[y*width+x];
            int uv_index=y/2*width+(x&~1);
            int U=UV_data[uv_index];
            int V=UV_data[uv_index+1];
            int C = Y - 16;
            int D = U - 128;
            int E = V - 128;

            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;
            if (R < 0) R = 0;
            else if (R > 255) R = 255;

            if (G < 0) G = 0;
            else if (G > 255) G = 255;

            if (B < 0) B = 0;
            else if (B > 255) B = 255;


            uint32_t pixel =(R << 16) |(G << 8)  |B;
            lcd_line[x] = pixel;
        }
        
    }
}