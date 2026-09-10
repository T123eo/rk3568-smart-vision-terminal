# RK3568 Smart Vision Terminal

基于 RK3568 的嵌入式 Linux 实时视频采集、显示、硬件编码与网络传输项目。

系统通过 IMX415 MIPI 摄像头采集 NV12 视频数据，在 RK3568 开发板本地通过 DRM/KMS 显示画面，同时使用 Rockchip MPP 进行 H.264 硬件编码，并通过 TCP 发送至 PC 服务端。PC 端对 TCP 数据流进行协议解析，通过 FFmpeg/libavcodec 解码 H.264 视频，并使用 SDL2 实时显示。

---

## Project Architecture

```text
                         RK3568 Client

                    IMX415 MIPI Camera
                            │
                            ▼
                         V4L2
                            │
                            ▼
                           NV12
                     ┌──────┴──────┐
                     │             │
                     ▼             ▼
                 DRM/KMS      Rockchip MPP
                     │             │
                     ▼             ▼
                    LCD          H.264
                                   │
                                   ▼
                              TCP Client
                                   │
                                   │ Ethernet
                                   ▼

                         PC Linux Server

                              TCP Server
                                   │
                                   ▼
                           H264 Packet Queue
                                   │
                                   ▼
                          FFmpeg / libavcodec
                                   │
                                   ▼
                               AVFrame
                                   │
                                   ▼
                            AVFrame Queue
                                   │
                                   ▼
                                SDL2
                                   │
                                   ▼
                         Real-time Display
