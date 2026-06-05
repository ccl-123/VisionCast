# VisionCast 音视频管线设计

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 正式视频输入：13855 MIPI 摄像头
> 默认音频输入：NAU8822 `hw:1,0`
> 首版协议：RTSP/RTP
> 后续协议：SRT、WebRTC / WHIP / WHEP

---

## 1. 文档目的

本文档描述 VisionCast 的音视频系统管线设计，包括视频采集、音频采集、图像处理、视频编码、音频编码、时间戳、封包、协议传输、线程模型和低延迟优化策略。该文档面向最终项目目标设计，但允许按阶段逐步实现。

---

## 2. 系统总体架构

VisionCast 总体链路分为视频链路、音频链路、控制链路和监控链路。

```text
Video Input    → Video Capture    → Video Process    → Video Encoder    → Packetizer    → Network Sender
Audio Input    → Audio Capture    → Audio Encoder    → Packetizer    → Network Sender
Control    → Config Manager    → Pipeline Manager    → State Machine    → Monitor / Log
```

---

## 3. 视频输入设计

### 3.1 正式输入：13855

正式视频链路：

```text
13855 MIPI Sensor    → MIPI CSI-2    → RKCIF    → RKISP / RKAIQ    → rkisp_mainpath    → V4L2 Capture    → RGA，可选    → MPP Encoder
```

目标输出格式：

```text
NV12
```

推荐首版规格：

| 参数  | 建议值         |
| --- | ----------- |
| 分辨率 | 1280x720    |
| 帧率  | 30 FPS      |
| 格式  | NV12 / YUYV |
| 编码  | H.264       |
| 协议  | RTSP/RTP    |

### 3.2 调试输入：USB C270

备用调试链路：

```text
USB C270    → /dev/video21    → V4L2 Capture    → RGA / Software Convert    → MPP Encoder
```

C270 用于快速验证，不作为最终产品输入。

---

## 4. 音频输入设计

默认音频设备：

```text
hw:1,0
```

音频链路：

```text
Main Mic / Headset Mic    → NAU8822    → ALSA Capture    → PCM Frame    → Audio Timestamp    → Audio Encoder
```

首版音频格式：

| 参数        | 建议值           |
| --------- | ------------- |
| 采样率       | 48000 Hz      |
| 格式        | S16_LE        |
| 声道        | 1 或 2         |
| 帧长        | 10 ms / 20 ms |
| 首版编码      | PCM / G.711   |
| WebRTC 阶段 | Opus          |

---

## 5. 视频处理设计

视频处理模块主要由 RGA 完成。

RGA 功能：
1. 格式转换；
2. 缩放；
3. 裁剪；
4. 旋转；
5. NV12 输出；
6. 后续可扩展 OSD 叠加。

典型链路：

```text
V4L2 YUYV / NV16 / RGB    → RGA    → NV12    → MPP
```

如果 V4L2 已经输出 NV12，且分辨率符合编码要求，可以跳过 RGA。

---

## 6. 视频编码设计

视频编码使用 Rockchip MPP。

首版编码格式：

```text
H.264
```

H.264 优点：
1. 播放端兼容好；
2. RTSP/RTP 支持成熟；
3. VLC、ffplay 调试方便；
4. 浏览器/WebRTC 后续也可继续使用。

后续可增加 H.265：

```text
H.265
```

H.265 适合低码率场景，但播放端兼容和调试复杂度更高。

### 6.1 推荐编码参数

| 参数           | 首版建议            |
| ------------ | --------------- |
| Codec        | H.264           |
| Profile      | Baseline / Main |
| Resolution   | 1280x720        |
| FPS          | 30              |
| GOP          | 15 或 30         |
| Bitrate      | 2 Mbps ~ 6 Mbps |
| Rate Control | CBR             |
| B Frame      | 关闭              |
| Input Format | NV12            |
| Low Latency  | 开启              |

---

## 7. 音频编码设计

音频编码按阶段推进。

| 阶段    | 编码    | 说明              |
| ----- | ----- | --------------- |
| V1    | PCM   | 最容易调试，带宽高       |
| V1/V2 | G.711 | 实现简单，适合语音       |
| V2/V3 | Opus  | 低延迟，适合 WebRTC   |
| 可选    | AAC   | 通用播放器兼容好，但实时性一般 |

首版建议：

```text
PCM / G.711
```

最终 WebRTC 目标：

```text
Opus
```

---

## 8. 时间戳设计

音视频统一使用：

```c
clock_gettime(CLOCK_MONOTONIC, &ts);
```

转换为微秒：

```c
uint64_t pts_us = ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
```

原则：
1. 采集时打时间戳；
2. 编码后保留采集时间戳；
3. 封包使用同源时间戳；
4. 音频和视频必须同一时钟源；
5. 禁止使用系统实时时钟作为媒体时间基。

---

## 9. 协议设计

### 9.1 V1：RTSP/RTP

首版使用 RTSP/RTP：

```text
H.264 NALU    → RTP Packetizer    → RTSP Server    → VLC / ffplay
```

用途：
1. 局域网调试；
2. 端到端链路验证；
3. 播放端兼容性测试；
4. 基础延迟测试。

### 9.2 V2：SRT

增强版支持 SRT：

```text
H.264/H.265 + Audio    → SRT Sender    → Media Server    → Remote Client
```

用途：
1. 公网推流；
2. 弱网传输；
3. 移动网络场景；
4. 比 RTMP 更低延迟。

### 9.3 V3：WebRTC / WHIP / WHEP

最终版本支持 WebRTC：

```text
RK3588 Device    → WebRTC WHIP    → Media Server    → WHEP / Browser
```

用途：
1. 浏览器低延迟播放；
2. 远程协作；
3. 语音对讲；
4. NAT 穿透；
5. 智能眼镜最终产品形态。

---

## 10. 线程模型设计

首版线程模型：

```text
VideoCaptureThread    → VideoProcessThread    → VideoEncodeThread    → NetworkSendThread
```

音频线程：

```text
AudioCaptureThread    → AudioEncodeThread    → NetworkSendThread
```

控制线程：

```text
MainThread    → ConfigManager    → PipelineManager    → StateMonitor
```

低延迟原则：
1. 队列长度不能过大；
2. 队列满时优先丢旧帧；
3. 编码线程不能阻塞采集线程太久；
4. 网络发送异常不能拖死整个管线；
5. 每个阶段都要记录耗时。

---

## 11. Buffer 策略

### 11.1 首版策略

首版允许使用：

```text
V4L2 MMAP    → CPU Address    → RGA / MPP
```

目标是先跑通链路。

### 11.2 优化策略

后续优化为：

```text
V4L2 Export DMA-BUF FD    → RGA Import DMA-BUF FD    → MPP Import DMA-BUF FD
```

目标：
1. 减少 CPU 拷贝；
2. 降低内存带宽；
3. 降低端到端延迟；
4. 提高稳定帧率。

---

## 12. Pipeline 模块划分建议

模块：

```text
VideoCapture
AudioCapture
RgaProcessor
MppEncoder
AudioEncoder
Packetizer
NetworkSender
MediaClock
PipelineManager
PerfMonitor
```

对应代码结构：

```text
include/media/
include/transport/
include/pipeline/
include/platform/rk3588/

src/media/
src/transport/
src/pipeline/
src/platform/rk3588/
```

---

## 13. 配置文件设计

建议配置项：

```json
{
  "video": {
    "source": "mipi_13855",
    "device": "/dev/videoX",
    "fallback_device": "/dev/video21",
    "width": 1280,
    "height": 720,
    "fps": 30,
    "format": "NV12"
  },
  "audio": {
    "device": "hw:1,0",
    "sample_rate": 48000,
    "channels": 2,
    "format": "S16_LE",
    "frame_ms": 20
  },
  "encoder": {
    "codec": "h264",
    "bitrate": 4000000,
    "gop": 30,
    "b_frames": 0,
    "low_latency": true
  },
  "stream": {
    "protocol": "rtsp",
    "port": 8554,
    "path": "/live"
  }
}
```

---

## 14. 首版验收标准

首版完成标准：
1. 可从 13855 或 C270 获取视频帧；
2. 可从 NAU8822 `hw:1,0` 获取音频帧；
3. 可使用 MPP 输出 H.264；
4. 可通过 RTSP/RTP 在 VLC 或 ffplay 播放；
5. 可打印 FPS、码率、编码耗时；
6. 可运行至少 30 分钟不崩溃；
7. 可通过配置文件切换调试摄像头；
8. 可记录端到端延迟。

---

## 15. 最终目标

最终 VisionCast 管线：

```text
13855 MIPI Sensor    → RKISP / RKAIQ    → V4L2 DMA-BUF    → RGA，可选    → MPP H.264/H.265    → WebRTC / SRT / RTSP
NAU8822 Mic    → ALSA    → Opus / G.711 / PCM    → WebRTC / SRT / RTSP
```

目标是形成可复用的 RK3588 低延迟音视频推流框架。
