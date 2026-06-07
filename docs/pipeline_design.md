# VisionCast 音视频管线设计 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 视频输入：13855 MIPI 摄像头 (主路径 `/dev/video11`) / USB C270 (备用路径 `/dev/video21`)
> 音频输入：NAU8822 `hw:1,0`
> 传输协议：WebRTC WHIP、RTMP、RTSP、RTP over UDP (均已实现)

---

## 1. 文档目的

本文档详细描述 VisionCast 最终版本的音视频系统管线设计，包括视频采集、音频采集、图像处理、视频编码、音频编码、时间戳同步、封包、网络传输、线程模型以及未来的优化方向。本文档所有内容均对应系统中已实现的功能与架构。

---

## 2. 系统总体架构与数据链路

VisionCast 总体链路分为视频链路、音频链路、本地显示链路和网络推流链路。

```text
[VideoCapture] (13855 / C270) ──┬──> [DisplayRenderer] (异步 OpenCV 窗口渲染)
                                └──> [RgaProcessor] (RGA im2d 尺寸/格式转换)
                                          │
                                          ▼
                                     [MppEncoder] (MPP H.264 硬编码)
                                          │
                                          ▼
                                    [AvTransport] (多协议推送抽象层)
                                          │
                                          ├─> [WebRtcPusher] (WebRTC WHIP)
                                          ├─> [RtmpPusher] (FFmpeg RTMP)
                                          ├─> [RtspServer] (内嵌式 RTSP 服务)
                                          └─> [UdpSender] (RTP over UDP 裸流)

[AudioCapture] (NAU8822 hw:1,0) ────> [AudioEncoder] (G.711 PCMA / AAC)
                                          │
                                          ▼
                                    [AvTransport] (多协议推送抽象层)
```

---

## 3. 视频输入设计

### 3.1 核心输入：13855 MIPI CSI 摄像头
- **物理路径**：13855 MIPI Sensor → MIPI CSI-2 → RKCIF → RKISP → rkisp_mainpath (`/dev/video11`)。
- **图像格式**：由 ISP 输出 NV12 格式。
- **采集机制**：采用 V4L2 多平面采集 (MPLANE) 模式，MMAP 方式申请 4 个 Buffer 帧缓冲区。使用 `configure_sensor_frame_rate` 强制锁定垂直消隐 (vblank) 与曝光，确保物理帧率稳定在 30 FPS。

### 3.2 调试与备用输入：USB C270 摄像头
- **物理路径**：USB C270 → `/dev/video21` (UVC)。
- **图像格式**：抓取 MJPEG 画面，通过 MPP MJPEG 解码器或 libjpeg-turbo 软解还原为 NV12 格式。
- **降级保护**：当 13855 摄像头无法打开或采集异常时，系统自动平滑切换至备用摄像头输入。

---

## 4. 音频输入与处理设计

- **物理设备**：板载 NAU8822 音频 Codec，对应 ALSA 设备 `hw:1,0`。
- **采集配置**：48000Hz 采样率，S16_LE 格式，单次捕获周期 20ms。
- **混音处理**：若硬件无法以单声道采集，自动以双声道抓取数据，并在 `AudioCapture` 底层进行左右声道均值混音，输出推流所需的单声道原始 PCM 数据。

---

## 5. 视频预处理设计 (RGA)

预处理由 Rockchip RGA 硬件加速单元完成，避免 CPU 参与格式和像素拷贝：
1. **尺寸缩放**：将输入帧大小统一缩放为 `1280x720`（或配置的分辨率）。
2. **色彩转换**：若是 YUYV/MJPEG 解码出的其他格式，通过 `im2d_api` 转换为 MPP 编码器所需的目标 `NV12`。
3. **性能开销**：im2d 硬件加速耗时稳定在 `1.5 ~ 3.3 ms`。

---

## 6. 视频硬件编码设计 (MPP)

视频硬编码基于 Rockchip MPP API：
- **编码格式**：H.264 Baseline Profile（完全禁用 B 帧，以消除重排延迟）。
- **码率控制**：固定码率 (CBR) 控制，默认配置为 4Mbps，码率波动范围稳定。
- **GOP 大小**：固定设为 30。
- **低延迟优化**：启用 MPP 内部低延迟模式，视频帧随到随编，即刻输出，编码耗时稳定在 `2.1 ~ 6.3 ms`。

---

## 7. 音频编码设计

音频支持两种主流编码器，可根据配置自动适配：
1. **G.711A (PCMA)**：计算极简，极省 CPU 算力，非常适合超低延迟对讲。
2. **AAC 软编码**：基于 FFmpeg libfdk-aac / native aac，提供更好的音质兼容性。

---

## 8. 全管线时间戳与同步设计

系统基于 `CLOCK_MONOTONIC` 单调微秒级时钟构建同步体系：
1. **采集打标**：视频在 V4L2 驱动出队、音频在 ALSA 读出时瞬间打上原始时间戳。
2. **时间戳传递**：原始帧在 RGA 转换、MJPEG 解码、MPP 编码和音频编码后，严格保留采集时的原始 PTS。
3. **播放端同步**：封包和传输层使用同源 PTS 写入 RTP 或 WebRTC Header，保证播放端完美的音画同步。

---

## 9. 传输协议设计

### 9.1 WebRTC (WHIP)
- **实现**：链接 `libdatachannel` 建立 PeerConnection 传输音视频流。
- **环回优化**：本地/回环测试时，将 libdatachannel 套接字强制绑定到 `127.0.0.1` 物理回环；同时，将 SDP Offer 字符串中的 `a=setup:actpass` 强制覆写为 `a=setup:active`，强迫客户端主动发出 `ClientHello` 以进行 DTLS 协商，彻底解决了 Pion/MediaMTX 环境下的 DTLS 握手超时问题。

### 9.2 RTMP
- **实现**：采用 FFmpeg 动态加载库方式，将 H.264 NAL 字节流封装为 AVCC 长度前缀格式，推送至目的流媒体服务器端口。

### 9.3 RTSP
- **实现**：内嵌静态 `librtsp.a` 服务端。在板端本地 `8555` 端口监听，接收播放端连接并分发音视频数据，不依赖外部转发服务器。

### 9.4 RTP over UDP
- **实现**：音视频 RTP 包直接通过裸 UDP 向目标接收端口投递，并在运行时自动在工作目录生成对应的播放描述文件 `test.sdp`。
- **空挂断容错**：在 `UdpSender::send` 中对套接字 `ECONNREFUSED` 异常进行拦截 and 日志屏蔽，使得接收端不在线时，推流服务依然能正常稳定工作。

---

## 10. 线程模型设计

系统运行于多线程异步调度模型中：
- **采集线程** (`VideoCaptureThread` / `AudioCaptureThread`)：专职设备数据读取。
- **主管线线程** (`VideoPipeline` / `AudioPipeline`)：处理解码、RGA 转换、MPP/音频编码和多协议推送。
- **本地渲染线程** (`DisplayRenderer` 线程)：OpenCV 窗口图像异步渲染。
- **丢帧防堆积策略**：在原始和编码队列中采用最大长度为 2 的 `BlockingQueue`。当网络或发送异常引起阻塞时，队列满后自动丢弃头部最老的帧，将最新帧推入尾部，防止延迟累积。

---

## 11. 最终项目目录结构

```text
VisionCast/
├── CMakeLists.txt                      # 顶层 CMake 构建文件
├── README.md                           # 基础运行介绍
├── .gitignore                          # Git 忽略文件配置
│
├── config/                             # 静态 JSON 配置文件
│   ├── visioncast_config.json          # 主程序总控配置
│   ├── video_13855.json                # 13855 MIPI 摄像头采集参数
│   ├── video_usb_c270.json             # USB C270 采集与备用参数
│   ├── audio_main_mic.json             # 主麦克风采集配置
│   └── encoder_h264_low_latency.json   # 编码器参数
│
├── include/                            # 系统头文件目录
│   ├── common/                         # 公共组件 (日志、配置、阻塞队列等)
│   ├── media/                          # 媒体设备 (采集、RGA转换、硬编码器)
│   ├── transport/                      # 网络传输 (RTMP、RTSP、UDP、WebRTC)
│   └── pipeline/                       # 调度管线 (音视频 Pipeline)
│
├── src/                                # 系统源码目录
│   ├── main.cpp                        # 主入口
│   ├── common/
│   ├── media/
│   ├── transport/
│   └── pipeline/
│
├── docs/                               # 核心技术设计及测试指南
│   ├── pipeline_design.md              # 音视频流数据管线设计 (本文件)
│   ├── camera_13855_flow.md            # 13855 视频流处理链路
│   ├── audio_nau8822_flow.md           # nau8822 音频捕获链路
│   ├── device_probe.md                 # 板端设备枚举参考
│   ├── performance_test.md             # 性能时延统计日志
│   ├── issue_record.md                 # 联调过程 Bug 解决记录
│   └── multi_protocol_test_guide.md    # 多协议推拉流实战测试教程
│
├── board/                              # 板端硬件设备适配节点
│   └── elf-rk3588/
│       ├── device_nodes.md             # 核心视频与音频接口枚举
│       ├── media_graph.md              # CSI-2 / ISP0 拓扑链路
│       ├── audio_mixer.md              # 混音通道配置
│       ├── camera_13855.md             # 13855 物理适配参数
│       └── debug_commands.md           # 硬件层调试快捷命令
│
├── scripts/                            # 系统构建与运行脚本
│   ├── board/                          # 板端硬件检测与麦克风配置
│   ├── build/                          # 编译与打包脚本 (device_build.sh等)
│   └── run/                            # 设备推流运行快捷脚本
│
└── mediamtx/                           # 预置的 MediaMTX 服务端 (含配置文件)
```

---

## 12. 有待优化的地方 (Future Optimizations)

1. **DMA-BUF 零拷贝链路打通**：
   当前数据从 V4L2 到 RGA 再到 MPP 的流程中仍包含了用户空间缓冲的映射和复制。未来应利用 `V4L2` 导出的 DMA-BUF FD 直接传递给 `RGA` 导入，再将转换后的 FD 传给 `MPP`。这样可以彻底消除 CPU 的数据拷贝，将端到端延迟降低到 150ms 以内。
2. **动态码率与拥塞控制 (Congestion Control)**：
   目前推流仅使用固定的 CBR 码率。未来可引入 WebRTC/RTCP 反馈机制，读取客户端返回的丢包率和 RTT 延迟，实时动态调整 MPP 编码器的 `Bitrate` 目标。
3. **Opus 高质量音频编码支持**：
   当前 WebRTC 中音频使用 G.711A (PCMA)，虽然开销极低但音质受限。未来可交叉编译静态 `libopus` 并集成到项目中，提供高质量、低延迟的双向音频对讲。
4. **H.265 硬件视频编码支持**：
   RK3588 平台的 MPP 具备卓越 H.265 硬件编码性能。可以在配置文件和推流层中新增 H.265 支线，提高弱网下的图像带宽比。
