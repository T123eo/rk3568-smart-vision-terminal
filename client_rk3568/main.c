#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include "imx415.h"
#include "DRM_LCD.h"
#include "mpp.h"
#include "tcp_client.h"


/*
 * ============================================================
 * PC服务端地址
 *
 * 后面根据你PC实际IPv4地址修改
 * ============================================================
 */
#define SERVER_IP       "192.168.1.100"
#define SERVER_PORT     8088


/*
 * ============================================================
 * 三线程共享结构
 * ============================================================
 */
typedef struct
{
    /* 当前V4L2帧 */
    void *data;

    size_t bytesused;

    unsigned int index;


    /*
     * 当前是否已经发布新帧
     */
    int frame_ready;


    /*
     * LCD和MPP完成标志
     */
    int lcd_done;

    int mpp_done;


    /*
     * 线程同步
     */
    pthread_mutex_t mutex;

    pthread_cond_t cond;


    /*
     * 各模块控制结构
     */
    Camera_handle *camera;

    LCD_handle *lcd;

    MPP_Encode_cfg *enc;

    /*
     * TCP客户端
     */
    TCP_Client *client;

} ThreadShare;


/*
 * 线程函数声明
 */
void *imx415_thread(void *arg);

void *lcd_thread(void *arg);

void *mpp_thread(void *arg);


/*
 * ============================================================
 * main
 * ============================================================
 */
int main()
{
    /*
     * --------------------------------------------------------
     * 1. 创建各模块控制结构
     * --------------------------------------------------------
     */

    Camera_handle imx415;

    LCD_handle lcd;

    MPP_Encode_cfg enc;

    TCP_Client client;


    /*
     * MPP结构体清零
     */
    memset(
        &enc,
        0,
        sizeof(enc)
    );


    /*
     * --------------------------------------------------------
     * 2. 初始化IMX415
     * --------------------------------------------------------
     */

    Imx415_Init(
        &imx415
    );


    /*
     * --------------------------------------------------------
     * 3. 初始化DRM LCD
     * --------------------------------------------------------
     */

    DRM_LCD_Init(
        &lcd
    );


    /*
     * --------------------------------------------------------
     * 4. 根据摄像头实际参数初始化MPP
     * --------------------------------------------------------
     */

    enc.width =
        imx415.width;

    enc.height =
        imx415.height;

    /*
     * NV12每一行实际占用字节数
     */
    enc.hor_stride =
        imx415.bytesperline[0];

    enc.ver_stride =
        imx415.height;


    if (MPP_Init(&enc) < 0)
    {
        printf(
            "MPP_Init failed\n"
        );

        DRM_LCD_destroy(
            &lcd
        );

        return -1;
    }


    /*
     * --------------------------------------------------------
     * 5. 初始化TCP客户端
     *
     * RK3568主动连接PC服务端
     * --------------------------------------------------------
     */

    if (TCP_Client_Init(
            &client,
            SERVER_IP,
            SERVER_PORT) < 0)
    {
        printf(
            "TCP_Client_Init failed\n"
        );

        DRM_LCD_destroy(
            &lcd
        );

        return -1;
    }


    /*
     * 如果成功，这里应该打印：
     *
     * TCP connected to 192.168.1.100:8088
     */


    /*
     * --------------------------------------------------------
     * 6. 初始化线程共享结构
     * --------------------------------------------------------
     */

    ThreadShare share;

    memset(
        &share,
        0,
        sizeof(share)
    );


    /*
     * 各模块地址传给共享结构
     */
    share.camera =
        &imx415;

    share.lcd =
        &lcd;

    share.enc =
        &enc;

    share.client =
        &client;


    /*
     * 当前还没有摄像头帧
     */
    share.data = NULL;

    share.bytesused = 0;

    share.index = 0;


    /*
     * 初始化互斥锁
     */
    pthread_mutex_init(
        &share.mutex,
        NULL
    );


    /*
     * 初始化条件变量
     */
    pthread_cond_init(
        &share.cond,
        NULL
    );


    /*
     * --------------------------------------------------------
     * 7. 创建线程
     * --------------------------------------------------------
     */

    pthread_t imx415_tid;

    pthread_t lcd_tid;

    pthread_t mpp_tid;


    /*
     * 摄像头采集线程
     */
    pthread_create(
        &imx415_tid,
        NULL,
        imx415_thread,
        &share
    );


    /*
     * LCD显示线程
     */
    pthread_create(
        &lcd_tid,
        NULL,
        lcd_thread,
        &share
    );


    /*
     * MPP编码线程
     */
    pthread_create(
        &mpp_tid,
        NULL,
        mpp_thread,
        &share
    );


    /*
     * --------------------------------------------------------
     * 8. 等待线程退出
     * --------------------------------------------------------
     */

    pthread_join(
        imx415_tid,
        NULL
    );

    pthread_join(
        lcd_tid,
        NULL
    );

    pthread_join(
        mpp_tid,
        NULL
    );


    /*
     * --------------------------------------------------------
     * 9. 资源释放
     * --------------------------------------------------------
     */

    TCP_Client_Close(
        &client
    );


    pthread_mutex_destroy(
        &share.mutex
    );

    pthread_cond_destroy(
        &share.cond
    );


    DRM_LCD_destroy(
        &lcd
    );


    return 0;
}


/*
 * ============================================================
 * IMX415采集线程
 * ============================================================
 */
void *imx415_thread(void *arg)
{
    ThreadShare *share =
        (ThreadShare *)arg;


    Camera_handle *camera =
        share->camera;


    while (1)
    {
        struct v4l2_buffer buf =
            {0};

        struct v4l2_plane
            planes[VIDEO_MAX_PLANES] =
            {0};


        /*
         * ----------------------------------------------------
         * V4L2 Buffer配置
         * ----------------------------------------------------
         */

        buf.type =
            V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        buf.memory =
            V4L2_MEMORY_MMAP;

        buf.m.planes =
            planes;

        buf.length =
            camera->num_planes;


        /*
         * ----------------------------------------------------
         * 1. 从V4L2驱动取一帧
         * ----------------------------------------------------
         */

        if (ioctl(
                camera->fd,
                VIDIOC_DQBUF,
                &buf) < 0)
        {
            perror(
                "VIDIOC_DQBUF"
            );

            break;
        }


        /*
         * ----------------------------------------------------
         * 2. 发布当前帧
         * ----------------------------------------------------
         */

        pthread_mutex_lock(
            &share->mutex
        );


        /*
         * 当前NV12 buffer地址
         */
        share->data =
            camera->buffer[
                buf.index
            ].start;


        /*
         * 当前buffer实际有效长度
         */
        share->bytesused =
            planes[0].bytesused;


        /*
         * 保存V4L2 Buffer index
         */
        share->index =
            buf.index;


        /*
         * 新的一帧：
         *
         * LCD还没有处理
         * MPP还没有处理
         */
        share->lcd_done = 0;

        share->mpp_done = 0;


        /*
         * 标记有新帧
         */
        share->frame_ready = 1;


        /*
         * 同时唤醒LCD线程
         * 和MPP线程
         */
        pthread_cond_broadcast(
            &share->cond
        );


        /*
         * ----------------------------------------------------
         * 3. 等待LCD和MPP全部完成
         * ----------------------------------------------------
         */

        while (
            !share->lcd_done ||
            !share->mpp_done
        )
        {
            pthread_cond_wait(
                &share->cond,
                &share->mutex
            );
        }


        /*
         * 当前帧处理结束
         */
        share->frame_ready = 0;


        pthread_mutex_unlock(
            &share->mutex
        );


        /*
         * ----------------------------------------------------
         * 4. LCD和MPP都已经不用这块buffer
         *
         * 再归还V4L2驱动
         * ----------------------------------------------------
         */

        if (ioctl(
                camera->fd,
                VIDIOC_QBUF,
                &buf) < 0)
        {
            perror(
                "VIDIOC_QBUF"
            );

            break;
        }
    }


    return NULL;
}


/*
 * ============================================================
 * LCD线程
 * ============================================================
 */
void *lcd_thread(void *arg)
{
    ThreadShare *share =
        (ThreadShare *)arg;


    Camera_handle *camera =
        share->camera;


    LCD_handle *lcd =
        share->lcd;


    while (1)
    {
        void *data = NULL;


        /*
         * ----------------------------------------------------
         * 1. 等待摄像头发布新帧
         * ----------------------------------------------------
         */

        pthread_mutex_lock(
            &share->mutex
        );


        while (
            !share->frame_ready ||
            share->lcd_done
        )
        {
            pthread_cond_wait(
                &share->cond,
                &share->mutex
            );
        }


        /*
         * 获取当前NV12地址
         */
        data =
            share->data;


        /*
         * 只获取地址，
         * 不要在RGB转换过程中持有mutex
         */
        pthread_mutex_unlock(
            &share->mutex
        );


        /*
         * ----------------------------------------------------
         * 2. NV12 -> RGB
         *
         * 写入DRM dumbbuffer
         * ----------------------------------------------------
         */

        NV12_To_RGB(
            data,
            camera->width,
            camera->height,
            lcd->map,
            lcd->dumbbuffer.pitch
        );


        /*
         * ----------------------------------------------------
         * 3. LCD使用完当前V4L2 Buffer
         * ----------------------------------------------------
         */

        pthread_mutex_lock(
            &share->mutex
        );


        share->lcd_done = 1;


        /*
         * 唤醒正在等待的IMX线程
         */
        pthread_cond_broadcast(
            &share->cond
        );


        pthread_mutex_unlock(
            &share->mutex
        );
    }


    return NULL;
}


/*
 * ============================================================
 * MPP编码 + TCP发送线程
 * ============================================================
 */
void *mpp_thread(void *arg)
{
    ThreadShare *share =
        (ThreadShare *)arg;


    Camera_handle *camera =
        share->camera;


    MPP_Encode_cfg *enc =
        share->enc;


    TCP_Client *client =
        share->client;


    while (1)
    {
        void *data = NULL;

        size_t bytesused = 0;


        /*
         * ----------------------------------------------------
         * 1. 等待摄像头发布NV12新帧
         * ----------------------------------------------------
         */

        pthread_mutex_lock(
            &share->mutex
        );


        while (
            !share->frame_ready ||
            share->mpp_done
        )
        {
            pthread_cond_wait(
                &share->cond,
                &share->mutex
            );
        }


        /*
         * 获取V4L2 NV12地址
         */
        data =
            share->data;


        /*
         * 获取有效数据长度
         */
        bytesused =
            share->bytesused;


        pthread_mutex_unlock(
            &share->mutex
        );


        /*
         * ----------------------------------------------------
         * 2. Rockchip MPP H264编码
         * ----------------------------------------------------
         */

        int ret =
            MPP_Encode(
                enc,
                data,
                bytesused,
                camera->bytesperline[0]
            );


        if (ret < 0)
        {
            printf(
                "MPP_Encode failed\n"
            );
        }
        else
        {
            /*
             * ------------------------------------------------
             * 3. 获取MPP H264 packet
             *
             * enc->packet_ptr
             *      ↓
             * H264数据地址
             *
             * enc->len
             *      ↓
             * H264数据长度
             * ------------------------------------------------
             */

            if (
                enc->packet_ptr != NULL &&
                enc->len > 0
            )
            {
                /*
                 * --------------------------------------------
                 * 4. 通过TCP发送H264 packet
                 *
                 * TCP_Client_SendH264内部会：
                 *
                 * 构造16字节协议头
                 *
                 * magic
                 * version
                 * type
                 * payload_len
                 * sequence
                 *
                 * 然后发送：
                 *
                 * Header + H264 packet
                 * --------------------------------------------
                 */

                ret =
                    TCP_Client_SendH264(
                        client,
                        enc->packet_ptr,
                        enc->len
                    );


                if (ret < 0)
                {
                    printf(
                        "TCP send H264 failed\n"
                    );
                }
            }
        }


        /*
         * ----------------------------------------------------
         * 5. MPP已经不再使用当前V4L2 buffer
         * ----------------------------------------------------
         */

        pthread_mutex_lock(
            &share->mutex
        );


        share->mpp_done = 1;


        /*
         * 通知IMX线程
         */
        pthread_cond_broadcast(
            &share->cond
        );


        pthread_mutex_unlock(
            &share->mutex
        );
    }


    return NULL;
}