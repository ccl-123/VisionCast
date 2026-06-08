# NAU8822 音频链路说明 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 音频 Codec：rockchip-nau8822
> 默认采集设备：`hw:1,0`
> 默认目标：板载 Main Mic / Headset Mic 音频采集，参与多协议推流分发

---

## 1. 文档目的

本文档说明 ELF-RK3588 板端 NAU8822 音频 Codec 的已实现采集与播放链路，包括 ALSA 设备、混音控制、音频采集参数、音频编码以及 VisionCast 项目中的音频处理设计。

---

## 2. ALSA 音频设备状况

### 2.1 采集设备
板端执行 `arecord -l` 识别到的唯一硬件采集设备为：
```text
card 1: rockchipnau8822 [rockchip-nau8822], device 0: dailink-multicodecs nau8822-hifi-0
```
应用层默认音频采集设备已固定为：
```text
hw:1,0
```

### 2.2 播放设备
板端执行 `aplay -l` 识别到耳机与板载播放对应的设备为：
```text
card 1: rockchipnau8822 [rockchip-nau8822], device 0
```
播放对应设备为 `hw:1,0`。

---

## 3. Main Mic 与 Headset Mic 混音控制

物理麦克风（板载主麦克风与耳机接口麦克风）共享同一个 ALSA 采集 PCM 设备。
在系统上，已预置脚本控制 Mixer 状态。使用 `amixer` 设置以下核心采集参数：
- `Main Mic` / `Headset Mic` 控制开/关。
- `ADC` 转换增益。
- `PGA` 和 `PGA Boost` 控制麦克风前级放大与增强。
- `Left/Right Input Mixer MicP/MicN` 调节左右声道混音路径。

项目随附在 `scripts/board/` 目录下的麦克风配置脚本进行硬件初始化。

---

## 4. USB 摄像头麦克风不可用状态说明

板端 USB C270 摄像头不具备音频输入能力：
- 内核中未打包并启用 `snd_usb_audio` 驱动模块。
- 执行 `sudo modprobe snd_usb_audio` 提示 `Module snd_usb_audio not found`。
- 音频输入链路统一仅采用板载 NAU8822 接口，排除了 UVC 麦克风输入。

---

## 5. VisionCast 音频链路与编码实现

音频数据从硬件捕获到网络传输的流程已全面跑通：
- **混音降采样**：由于 ALSA 硬件层在某些驱动配置下拒绝直接开启单声道 (Mono) 采集，`AudioCapture` 在启动时自动开启双声道 (Stereo) 采集，并在 PCM 帧读取出队后，进行 **左右声道均值混音**，平滑合并为一路单声道 PCM 帧。
- **采集时钟**：在 ALSA PCM 读取完成的瞬间打上 `CLOCK_MONOTONIC` 微秒级单调时钟 PTS。
- **编码器实现**：
  - **Opus 编码**：RTP 与 WebRTC 共用 `AudioEncoder`，使用 20ms 帧、48 kHz RTP 时钟和带内 FEC。
  - **AAC 编码**：RTMP 使用 FFmpeg AAC 编码器并封装为 FLV。

```text
[Main Mic / Headset Mic]
     │
     ▼
[NAU8822 Codec]
     │
     ▼
[ALSA hw:1,0]
     │ (S16_LE 双声道采集)
     ▼
[AudioCapture] (左右声道均值混音 -> 单声道 PCM)
     │ (打 CLOCK_MONOTONIC PTS)
     ▼
[AvTransport]
     ├─ RTP/WebRTC -> [AudioEncoder] Opus -> RTP/SRTP
     └─ RTMP       -> [RtmpPusher] AAC -> FLV
```

---

## 6. 音频采集与编码参数

| 参数 | 实际配置值 |
| --- | --- |
| 采样率 | `48000 Hz` |
| 声道数 | `1` (双声道采集，应用层降采样单声道输出) |
| 采样格式 | `S16_LE` |
| 帧周期 | `20 ms` |
| 实现编码 | RTP/WebRTC: Opus；RTMP: AAC |
| 默认设备 | `hw:1,0` |

---

## 7. 性能日志字段

音频流水线每秒输出窗口统计：

```text
[音频流水线]
  概览: 帧数=50 码率=768.00kbps 平均=1.88KB
  耗时: 排队=0.10ms 发送=0.55ms 总=0.65ms
  状态: 丢帧=0 原始队列=0
```

- `排队`：ALSA 读取完成打点到音频工作线程开始处理的等待时间。
- `发送`：`AvTransport::send_audio()` 本地调用耗时。RTP/WebRTC 包含 Opus 编码、RTP 封包和发送调用；RTMP 包含推给 FFmpeg RTMP 链路。
- `总`：ALSA 读取完成打点到 `send_audio()` 返回的本地总延迟。
- `码率`：当前按 PCM 输入字节计算，用于确认采集节奏，不等同于 Opus/AAC 实际网络码率。

RTP/WebRTC 的 Opus 编码缓存采用 offset 消费并复用 Opus 输入 scratch buffer，避免每个 20ms 包都搬移剩余 PCM 或重新分配转换缓冲。RTMP 的 AAC 输入缓存同样采用 offset 消费，只在缓存已消费完或超过半数时压缩一次。

---

## 8. 模块文件分布

与音频采集和编码直接相关的模块已归档为如下文件：
- 头文件：
  - [audio_capture.h](file:///home/elf/open_project/VisionCast/include/media/audio_capture.h)
  - [audio_encoder.h](file:///home/elf/open_project/VisionCast/include/media/audio_encoder.h)
- 源码文件：
  - [audio_capture.cpp](file:///home/elf/open_project/VisionCast/src/media/audio_capture.cpp)
  - [audio_encoder.cpp](file:///home/elf/open_project/VisionCast/src/media/audio_encoder.cpp)

> **注**：原规划中的 empty 文件 `src/platform/rk3588/rk_alsa_device.cpp` 和对应的头文件目录均已被清理删除。

---

## 9. 已验证的结论

1. **唯一物理录音通道**：板载 NAU8822 的 `hw:1,0` 节点是当前系统唯一可用的 PCM 录音设备。
2. **多声道混音适配**：系统已具备极佳的 ALSA 硬件兼容性，能够自动处理 Mono/Stereo 的降级协商并保证单声道 PCM 的顺畅输出。
3. **音画同步保真**：基于 `CLOCK_MONOTONIC` 微秒级 PTS 的透传，配合视频硬编码管道，端到端推流的音视频完全同步。
