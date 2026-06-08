# VisionCast 音视频管线设计 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 视频输入：13855 MIPI 摄像头 (主路径 `/dev/video11`) / USB C270 (备用路径 `/dev/video21`)
> 音频输入：NAU8822 `hw:1,0`
> 传输协议：WebRTC WHIP、RTMP、RTP over UDP (均已实现)

---

## 1. 文档目的

本文档详细描述 VisionCast 最终版本的音视频系统管线设计，包括视频采集、音频采集、图像处理、视频编码、音频编码、时间戳同步、封包、网络传输、线程模型以及未来的优化方向。本文档所有内容均对应系统中已实现的功能与架构。

---

## 2. 系统总体架构与数据链路

VisionCast 总体链路分为视频链路、音频链路、本地显示链路和网络推流链路。当前版本已经把摄像头到 H.264 编码器的主路径改为 DMA-BUF/硬件缓冲传递，协议封包仍保留必要的 CPU 可见编码包拷贝。本地预览在视频编码成功返回后异步分支，使用同一个 `VideoFrame` 的 DMA-BUF 句柄，不复制主路径 NV12。

```text
[VideoCapture] (13855 NV12 / C270 MJPEG)
     │
     ▼
[RgaProcessor] (MJPEG 解码、RGA fd/VA、直通或 CPU fallback)
     │
     ▼
[MppEncoder] (MPP H.264 硬编码，优先 EXT_DMA 输入)
     │
     ├──> [DisplayRenderer] (编码成功后移动同帧 VideoFrame；enable_preview=true)
     │
     ▼
[AvTransport] (多协议推送抽象层)
     │
     ├─> [WebRtcPusher] (RTP -> SRTP / WHIP)
     ├─> [RtmpPusher] (Annex-B -> AVCC -> FLV/RTMP)
     └─> [UdpSender] (RTP over UDP 裸流)

[AudioCapture] (NAU8822 hw:1,0) ────> [AudioEncoder] (Opus)
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
- **图像格式**：抓取 MJPEG 压缩帧，先按 `DQBUF.bytesused` 校验并裁剪完整 JPEG，再通过 MPP MJPEG 解码器还原为 NV12。
- **解码输出**：主路径使用 MPP DRM buffer 输出 NV12 DMA-BUF，直接进入 RGA fd 路径或 MPP 编码器 EXT_DMA 输入；只有 MPP 不可用或失败时才走 libjpeg/CPU 可见 fallback。
- **降级保护**：当 13855 摄像头无法打开或采集异常时，系统自动平滑切换至备用摄像头输入。

### 3.3 当前视频主链路

13855 MIPI NV12：

```text
V4L2 EXPBUF fd -> VideoFrame::dma -> RGA 直通 -> MPP EXT_DMA 输入 -> H.264
```

USB C270 MJPEG：

```text
V4L2 bytesused JPEG -> MPP MJPEG 解码输入
  -> MPP 解码输出 NV12 DMA-BUF
  -> RGA 直通或 fd-to-fd resize
  -> MPP EXT_DMA 输入 -> H.264
```

MJPEG 解码的输入、输出、fallback 和性能日志字段见 [mjpeg_decode_flow.md](file:///home/elf/open_project/VisionCast/docs/mjpeg_decode_flow.md)。

---

## 4. 音频输入与处理设计

- **物理设备**：板载 NAU8822 音频 Codec，对应 ALSA 设备 `hw:1,0`。
- **采集配置**：48000Hz 采样率，S16_LE 格式，单次捕获周期 20ms。
- **混音处理**：若硬件无法以单声道采集，自动以双声道抓取数据，并在 `AudioCapture` 底层进行左右声道均值混音，输出推流所需的单声道原始 PCM 数据。

---

## 5. 视频预处理设计 (RGA)

预处理由 `RgaProcessor` 统一调度，但它不等同于纯 RGA 耗时：
1. **MJPEG 解码**：USB C270 输入先进入 MPP MJPEG 解码，解码耗时在性能日志中单独统计为 `MJPEG`。
2. **直通**：如果输入或解码输出已经是目标尺寸 NV12，则不调用 RGA，直接把 DMA-BUF 交给 MPP 编码器。
3. **RGA fd-to-fd**：如果 DMA-BUF NV12 需要缩放，使用 RGA fd 输入和 MPP/DRM fd 输出。
4. **RGA VA / CPU fallback**：仅在不支持 DMA-BUF 或硬件路径失败时使用 CPU 可见数据。

日志中的 `前处理路径(fd/直通/拷贝)` 分别对应 RGA fd-to-fd、DMA/CPU 直通、RGA VA 或 CPU fallback。

---

## 6. 视频硬件编码设计 (MPP)

视频硬编码基于 Rockchip MPP API：
- **编码格式**：H.264 Baseline Profile（完全禁用 B 帧，以消除重排延迟）。
- **码率控制**：固定码率 (CBR) 控制，默认配置为 4Mbps，码率波动范围稳定。
- **GOP 大小**：固定设为 30。
- **低延迟优化**：启用 MPP 内部低延迟模式，视频帧随到随编，即刻输出，编码耗时稳定在 `2.1 ~ 6.3 ms`。
- **输入路径**：优先通过 `MPP_BUFFER_TYPE_EXT_DMA` 导入单平面 NV12 DMA-BUF；不满足条件时才退回 CPU 拷贝输入。

---

## 7. 音频编码设计

音频按传输协议采用两条明确路径：
1. **Opus**：RTP 与 WebRTC 共用 `AudioEncoder`，使用 48 kHz RTP 时钟、动态负载类型 `111` 和带内 FEC。
2. **AAC 软编码**：RTMP 将 PCM 直接交给 FFmpeg AAC 编码器并封装到 FLV。

---

## 8. 全管线时间戳与同步设计

系统基于 `CLOCK_MONOTONIC` 单调微秒级时钟构建同步体系：
1. **采集打标**：视频在 V4L2 驱动出队、音频在 ALSA 读出时瞬间打上原始时间戳。
2. **时间戳传递**：原始帧在 RGA 转换、MJPEG 解码、MPP 编码和音频编码后，严格保留采集时的原始 PTS。
3. **播放端同步**：封包和传输层使用同源 PTS 写入 RTP 或 WebRTC Header，保证播放端完美的音画同步。

---

## 9. 传输协议、数据链路与数据压缩格式设计

### 9.1 WebRTC (WHIP / WHEP)
- **物理实现**：链接 `libdatachannel` 建立 PeerConnection 传输音视频流，客户端通过 HTTP POST 向 WHIP 服务端推送 SDP Offer。
- **回环与握手优化**：本地/回环测试时，将 libdatachannel 套接字强制绑定到 `127.0.0.1` 物理回环；同时，将 SDP Offer 中的 `a=setup:actpass` 强制覆写为 `a=setup:active`，强迫客户端主动发出 `ClientHello` 以进行 DTLS 协商，将 DTLS 协商延时控制在 5ms 级。
- **音视频数据链路**：
  - **视频链路**：`13855 MIPI CSI 采集 (/dev/video11)` -> `DMA-BUF NV12 直通或RGA fd处理` -> `MppEncoder H.264硬编码` -> `RtpPacketizer` -> `libdatachannel以SRTP发送` -> `主机MediaMTX接收` -> `浏览器 WHEP 播放`。
  - **音频链路**：`nau8822 hw:1,0 采集 (48kHz S16_LE)` -> `AudioCapture输出 PCM` -> `AudioEncoder编码为Opus` -> `RtpPacketizer封装(PT=111)` -> `libdatachannel以SRTP加密发送` -> `主机MediaMTX` -> `浏览器 WHEP 播放`。
- **数据压缩规格**：
  - **视频**：H.264 Baseline Profile，完全禁用 B 帧，CBR 控制，GOP=30。
  - **音频**：Opus，RTP 时钟 48000Hz，20ms 帧，目标码率 64 kbps，启用带内 FEC。

### 9.2 RTMP
- **物理实现**：采用 FFmpeg 动态加载库 (`libavformat.so` 等) 建立 RTMP 传输层通道，向指定的 RTMP 服务端发布流。
- **字节流重组**：在发送前由 `RtmpPusher` 拦截 MPP 产生的 Annex-B NAL 字节流，切分并剥离 `0x00000001` 起始码，重组为符合 FLV 规范的 AVCC 4字节长度前缀数据包后写入封装器。当前 NALU 解析使用指针/长度视图，AVCC payload 缓冲区在推流器内复用，避免每帧重复生成 per-NALU 临时 vector。
- **音视频数据链路**：
  - **视频链路**：`13855 MIPI CSI 采集` -> `DMA-BUF NV12 直通或RGA fd处理` -> `MppEncoder H.264编码` -> `RtmpPusher转换为AVCC包` -> `FFmpeg FLV容器封装` -> `RTMP over TCP推送` -> `主机MediaMTX` -> `ffplay拉流`。
  - **音频链路**：`nau8822 hw:1,0 采集` -> `均值混音单声道 PCM` -> `FFmpeg重采样并软编码为AAC-LC` -> `FLV容器封装` -> `RTMP over TCP推送` -> `主机MediaMTX` -> `ffplay拉流`。
- **数据压缩规格**：
  - **视频**：H.264 Baseline Profile，AVCC 格式封装。
  - **音频**：AAC-LC (Advanced Audio Coding Low Complexity)，采样率 48000Hz，单声道，16-bit 深度。

### 9.3 RTP over UDP
- **物理实现**：创建标准 UDP 套接字，将音视频 RTP 封包分别向目标 IP/端口（默认视频端口 5004，音频端口 5006）进行无连接的主动推送。
- **SDP描述文件**：运行时自动在工作目录生成对应的播放描述文件 `test.sdp`，以指明拉流端所需的负载类型和端口。
- **空挂断容错**：在 `UdpSender::send` 中拦截 `ECONNREFUSED` 异常，当拉流端不在线或异常退出时，推流服务依然能保持运行，支持拉流端的随时热插拔。
- **封包优化**：RTP 使用逐包回调接口，包生成后立即发送；FU-A 分片直接构造最终 RTP packet，避免临时 payload vector 和每帧 packet 列表。
- **音视频数据链路**：
  - **视频链路**：`13855 MIPI CSI 采集` -> `DMA-BUF NV12 直通或RGA fd处理` -> `MppEncoder H.264编码` -> `RtpPacketizer封包(按RFC 6184分片为FU-A)` -> `UDP 发送(端口5004)` -> `主机 ffplay(解析sdp)播放`。
  - **音频链路**：`nau8822 hw:1,0 采集` -> `PCM` -> `AudioEncoder编码为Opus` -> `RtpPacketizer封装(Payload Type 111)` -> `UDP 发送(端口5006)` -> `主机 ffplay(解析sdp)播放`。
- **数据压缩规格**：
  - **视频**：H.264 Baseline Profile，RTP 动态负载类型 `96`。
  - **音频**：Opus，RTP 时钟 48000Hz，动态负载类型 `111`。

---

## 10. 线程模型设计

VisionCast 系统采用高度解耦、异步非阻塞的“生产者-消费者”多线程管线模型。各模块间通过线程安全且带丢帧限缓机制的阻塞队列（`BlockingQueue`）连接。

### 10.1 核心工作线程及数量分配
在正常推流运行期间（如 WebRTC 模式），系统至少会有 **6 个主控 C++ 线程** 处于活跃运行状态，另有底层第三方库额外拉起的 **3 ~ 5 个系统级网络 IO 线程**，合并运行共约 **9 ~ 11 个线程**：

1. **主控制线程 (Main Thread)**
   - **执行路径**：`src/main.cpp` 的 `main()`。
   - **职责**：执行配置载入、信号注册（SIGINT/SIGTERM）与子流水线拉起。启动后主线程每 200ms 挂起轮询，监测各个子流水线的健康状况，并在用户请求退出时执行优雅的安全清理机制。
2. **视频采集线程 (Video Capture Thread)**
   - **执行路径**：`src/media/video_capture.cpp` 中的 [VideoCapture::capture_loop](file:///home/elf/open_project/VisionCast/src/media/video_capture.cpp)。
   - **职责**：专职通过 V4L2 Mplane API 从物理摄像头节点 `/dev/video11` 阻塞捞取视频帧，写入视频管线原始帧队列 `raw_queue_` 中。
3. **视频处理与编码线程 (Video Pipeline Thread)**
   - **执行路径**：`src/pipeline/video_pipeline.cpp` 中的 [VideoPipeline::worker_loop](file:///home/elf/open_project/VisionCast/src/pipeline/video_pipeline.cpp)。
   - **职责**：作为消费者从 `raw_queue_` 中提取 NV12 原图。然后调度硬件：
     - 调用 **RGA** 硬件执行缩放与色彩校准；
     - 送入 **MPP** 进行 H.264 视频硬件编码；
     - 若配置 `debug.enable_preview=true`，在编码成功返回后将同一个 NV12 `VideoFrame` 移动到渲染队列中；
     - 最终将编码码流发送至 `AvTransport` 进行网络封包发送。
4. **本地渲染预览线程 (Display Renderer Thread)**
   - **执行路径**：`src/media/display_renderer.cpp` 中的 [DisplayRenderer::render_loop](file:///home/elf/open_project/VisionCast/src/media/display_renderer.cpp)。
   - **职责**：从容量为 1 的预览队列中获取 NV12 图像；桌面环境优先使用 X11 窗口，无桌面环境退回 `/dev/fb0` Framebuffer。CPU 可见帧可直接渲染，DMA-only 帧通过 RGA fd 输入转换到显示目标，避免为了预览把主路径 NV12 大帧复制回用户态。X11 仍需把 RGA 输出的 RGB/BGRA 像素提交给 `XPutImage`，Framebuffer 仍需写入 mmap 显存，因此显示端不是全链路零拷贝。
5. **音频采集线程 (Audio Capture Thread)**
   - **执行路径**：`src/media/audio_capture.cpp` 中的 [AudioCapture::capture_loop](file:///home/elf/open_project/VisionCast/src/media/audio_capture.cpp)。
   - **职责**：专职通过 ALSA API 定期从音频输入端 `hw:1,0` 抓取 20ms 周期的原始双声道 PCM 数据，推入音频管线原始帧队列 `raw_queue_` 中。
6. **音频处理与编码线程 (Audio Pipeline Thread)**
   - **执行路径**：`src/pipeline/audio_pipeline.cpp` 中的 [AudioPipeline::worker_loop](file:///home/elf/open_project/VisionCast/src/pipeline/audio_pipeline.cpp)。
   - **职责**：从音频队列中消费 PCM 数据并交给 `AvTransport`；RTP/WebRTC 由共享 `AudioEncoder` 编码为 Opus，RTMP 由 `RtmpPusher` 编码为 AAC。
7. **第三方协议栈后台网络线程 (Third-Party Network Threads)**
   - 在 WebRTC 传输模式下，`libdatachannel` 和 `libjuice` 会在后台启动 **3 ~ 5 个** 系统线程，来独立进行：
     - **ICE 连接状态与 STUN 打洞包轮询**；
     - **DTLS/SRTP 加密和数据解包**；
     - **SCTP 网络 IO 控制及重传**。

### 10.2 丢帧与防延迟累积策略
为了防止由于网络拥堵、临时发送失败或处理速度下降引发音视频帧的堆积积压：
- 视频和音频流水线中的每个 `BlockingQueue` 均设定了最大缓冲长度限制为 **`2`**。
- 一旦队列填满，后入的待处理帧或编码完成帧将**自动丢弃队列头部最老的帧（Head Dropping）**，然后把最新帧推入队列尾部。
- 这样能物理切断延迟累积效应，确保即便在网络出现剧烈波动甚至网络短暂恢复时，客户端解码渲染的依然是无延迟的最新数据流。

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
│   └── audio_main_mic.json             # 主麦克风采集配置
│
├── include/                            # 系统头文件目录
│   ├── common/                         # 公共组件 (日志、配置、阻塞队列等)
│   ├── media/                          # 媒体设备 (采集、RGA转换、硬编码器)
│   ├── transport/                      # 网络传输 (RTMP、UDP、WebRTC)
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
│   ├── 预研与开发文档.md               # 项目开发总结文档
│   ├── pipeline_design.md              # 音视频流数据管线设计 (本文件)
│   ├── camera_13855_flow.md            # 13855 视频流处理链路
│   ├── audio_nau8822_flow.md           # nau8822 音频捕获链路
│   ├── device_probe.md                 # 板端设备枚举参考
│   ├── performance_test.md             # 性能时延统计日志
│   ├── mjpeg_decode_flow.md            # USB MJPEG 解码链路说明
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
└── mediamtx/                           # 预置 of MediaMTX 服务端 (含配置文件)
```

---

## 12. 有待优化的地方 (Future Optimizations)

1. **编码后 packet view 化**：
   当前已收敛 RTP/WebRTC 逐包发送、RTP H.264 流式 NALU 扫描、RTMP NALU span 解析和音频 PCM offset 消费。剩余主要拷贝是 MPP 输出写入 `EncodedPacket::data`、RTP 最终 packet payload、WebRTC/SRTP 内部发送，以及 RTMP 必需的 AVCC payload 重组。
2. **动态码率与拥塞控制 (Congestion Control)**：
   目前推流仅使用固定的 CBR 码率。未来可引入 WebRTC/RTCP 反馈机制，读取客户端返回的丢包率和 RTT 延迟，实时动态调整 MPP 编码器的 `Bitrate` 目标。
3. **Opus 自适应优化**：
   当前 RTP/WebRTC 已使用 Opus。后续可依据 RTCP 反馈动态调整码率、FEC 和预期丢包率。
4. **H.265 硬件视频编码支持**：
   RK3588 平台的 MPP 具备卓越 H.265 硬件编码性能。可以在配置文件和推流层中新增 H.265 支线，提高弱网下的图像带宽比。
