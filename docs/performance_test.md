# VisionCast 性能日志与测试说明

> 平台：ELF-RK3588
> 目标：说明运行时性能日志的统计口径、链路分段耗时和板端验证方法。

## 1. 测试命令

```bash
./scripts/build/device_build.sh
cd install/visioncast/scripts/run
./run_13855_camera.sh webrtc
./run_13855_camera.sh rtmp
./run_usb_camera.sh rtp
```

运行前确认没有旧进程占用设备：

```bash
pgrep -af visioncast
```

## 2. 视频日志格式

视频流水线每秒输出一条窗口统计：

```text
[视频流水线]
  概览: 窗口帧=30 帧率=29.99fps 码率=4000.00kbps 平均=16.30KB
  路径: 前处理(fd/直通/拷贝)=0/30/0 MPP输入(DMA/COPY)=30/0 MJPEG(hw/sw/dma输出)=30/0/30
  耗时: 排队=0.20ms 前处理=1.30ms MJPEG=1.25ms 图像处理=0.05ms 编码=3.45ms 传输=0.10ms 总=5.10ms
  状态: 处理失败=0 编码无输出=0 丢帧=0 原始队列=0
```

字段含义：

- `窗口帧`：本统计窗口内成功送到传输层的视频帧数。
- `帧率`：按窗口时间计算的实际发送帧率，单位直接跟在数值后，例如 `29.99fps`。
- `前处理路径(fd/直通/拷贝)`：RGA fd-to-fd、DMA 直通、CPU/VA 拷贝路径计数。
- `MPP输入(DMA/COPY)`：MPP 编码输入是 DMA-BUF 导入还是 CPU 拷贝。
- `MJPEG(hw/sw/dma输出)`：MJPEG 硬解、软解、解码输出 DMA-BUF 的帧数。
- `码率`：编码后进入传输层的视频 payload 码率，单位直接跟在数值后，例如 `4000.00kbps`。
- `平均`：单个编码包平均大小，单位直接跟在数值后，例如 `16.30KB`。
- `处理失败`：前处理失败次数，包含无效 MJPEG、RGA/解码失败等。
- `编码无输出`：MPP 本次编码未返回 packet 的帧数。
- `丢帧`：原始队列主动丢弃的累计帧数。

## 3. 视频耗时口径

`耗时` 行使用 `字段=值ms` 格式，所有值来自同一个 `CLOCK_MONOTONIC` 微秒时钟。

| 字段 | 起止点 | 说明 |
| --- | --- | --- |
| `排队` | 采集 DQBUF 打点 -> 视频工作线程开始处理 | 反映 raw 队列等待。 |
| `前处理` | `RgaProcessor::process()` 入口 -> 返回 | 包含 MJPEG 解码、RGA、直通判断和 CPU fallback。 |
| `MJPEG` | `MjpegDecoder::decode()` 入口 -> 返回 | 仅 MJPEG 输入计入；13855 NV12 路径为 0。 |
| `图像处理` | `前处理 - MJPEG` | 表示解码后的 RGA fd/VA、CPU 转换或直通开销。 |
| `编码` | `MppEncoder::encode()` 入口 -> 返回 | 包含 MPP 输入导入、硬编码和 packet 取回。 |
| `传输` | `AvTransport::send_video()` 入口 -> 返回 | 包含 RTP/RTMP/WebRTC 本地封包和发送调用，不是远端网络延迟。 |
| `总` | 采集 DQBUF 打点 -> `send_video()` 返回 | 本地端到发送完成的总延迟。 |

## 4. 当前视频链路状态

13855 MIPI NV12 主路径：

```text
V4L2 EXPBUF fd
  -> VideoFrame::dma
  -> RGA 直通
  -> MPP H.264/H.265 EXT_DMA 输入
  -> EncodedPacket
  -> RTP / RTMP / WebRTC
```

USB C270 MJPEG 路径：

```text
V4L2 bytesused JPEG payload
  -> MPP MJPEG 解码输入 buffer
  -> MPP 解码输出 NV12 DMA-BUF
  -> RGA 直通或 fd-to-fd resize
  -> MPP H.264/H.265 EXT_DMA 输入
  -> EncodedPacket
  -> RTP / RTMP / WebRTC
```

摄像头到编码器主路径已经避免解码后整帧 NV12 用户态拷贝。仍然存在的拷贝是压缩 JPEG 输入写入 MPP、编码后 packet 提取、以及协议封包。

本地桌面预览由 `debug.enable_preview` 控制。开启时，视频线程在 MPP 编码成功返回后把同一个 `VideoFrame` 移动到 `DisplayRenderer`，不是从采集端复制一份 NV12。DMA-only 帧使用 RGA fd 输入渲染，预览队列容量为 1，最多短暂多持有约 1 到 2 个 DMA-BUF 句柄；关闭时不启动显示线程，纯推流路径不会额外持有预览帧。

预览不会影响后续协议发送的数据正确性，因为 RTP/RTMP/WebRTC 使用的是已经生成的 `EncodedPacket`。但显示端不是全链路零拷贝：X11 需要 `NV12 DMA-BUF -> RGA RGB/BGRA -> XPutImage`，Framebuffer 需要 `NV12 DMA-BUF -> RGA -> mmap fb`，因此开启预览会增加少量 RGA/内存带宽和显示提交开销。

编码后封包阶段已经做了低风险收敛：

- RTP/WebRTC：`RtpPacketizer::packetize_each()` 逐包回调发送，不再为每帧先保存完整 `vector<RtpPacket>` 后再遍历发送。
- RTP/WebRTC H.264/H.265：实时发送路径按 Annex-B NALU 流式扫描，不再先构造每帧 NALU offset 列表。
- RTP FU-A：直接构造最终 RTP packet，避免每个分片先生成临时 payload vector。
- RTMP：Annex-B NALU 解析仅用于提取 H.264/H.265 参数集，视频 packet 交给内置 FFmpeg 8.1.1 FLV muxer 写出。
- RTP/WebRTC 音频：Opus 输入 PCM 缓存使用 offset 消费，并复用 Opus 输入 scratch buffer，避免每帧前端 `erase` 和临时 PCM vector 分配。
- RTMP 音频：AAC 输入 PCM 缓存使用 offset 消费，只在缓存全部消费或超过半数已消费时压缩一次。
- RTMP H.265 依赖 Enhanced RTMP/HEVC 支持；服务端或播放器不支持时需切回 `encoder.video_codec=h264`。

## 5. 音频日志格式

音频流水线日志：

```text
[音频流水线]
  概览: 帧数=50 码率=768.00kbps 平均=1.88KB
  耗时: 排队=0.10ms 发送=0.55ms 总=0.65ms
  状态: 丢帧=0 原始队列=0
```

字段含义：

- `帧数`：窗口内发送的音频帧数，20ms 帧通常为 50 或 51。
- `码率`：当前统计仍按 PCM 输入字节计算，用于观察采集节奏，不等同于 Opus/AAC 码率。
- `排队`：ALSA 读取打点到音频工作线程开始发送的等待时间。
- `发送`：`AvTransport::send_audio()` 本地调用耗时。RTP/WebRTC 包含 Opus 编码和 RTP 封包；RTMP 包含推给 FFmpeg RTMP 队列。
- `总`：ALSA 读取打点到 `send_audio()` 返回的本地总延迟。

## 6. 健康状态判断

- 13855 WebRTC：`前处理路径(fd/直通/拷贝)=0/30/0`，`MPP输入(DMA/COPY)=30/0`，`MJPEG(...)=0/0/0`。
- USB RTP：`MJPEG(hw/sw/dma输出)=30/0/30`，`前处理路径(fd/直通/拷贝)=0/30/0`，`MPP输入(DMA/COPY)=30/0`。
- 任一路径：`处理失败=0`、`丢帧=0`、`原始队列=0` 表示处理和发送没有形成积压。

## 7. 2026-06-08 板端验证记录

- USB C270 RTP：稳定 30fps，`MJPEG(hw/sw/dma输出)=30/0/30`，`MPP输入(DMA/COPY)=30/0`，本地总耗时约 5.0ms，处理失败 0。
- 13855 WebRTC：WHIP/DTLS 连接成功，稳定 30fps，`前处理路径(fd/直通/拷贝)=0/30/0`，`MPP输入(DMA/COPY)=30/0`，传输约 1.1-1.3ms。
- 13855 RTMP：稳定 30fps，`MPP输入(DMA/COPY)=30/0`，RTMP 传输约 0.3-0.4ms，AAC 音频发送约 0.8-0.9ms；启动阶段可能因 RTMP/AAC 初始化出现少量视频丢帧，稳定后不再增长。
