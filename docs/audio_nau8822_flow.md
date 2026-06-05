# NAU8822 音频链路说明

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 音频 Codec：rockchip-nau8822
> 默认采集设备：`hw:1,0`
> 默认目标：Main Mic / Headset Mic 音频采集，参与 RTSP/RTP、SRT、WebRTC 推流

---

## 1. 文档目的

本文档用于说明 ELF-RK3588 板端 NAU8822 音频 Codec 的采集与播放链路，包括 ALSA 设备、物理输入源、mixer 控制、音频采集参数、音频编码路线以及 VisionCast 项目中的音频处理方式。

当前项目中，NAU8822 是首版默认音频输入来源。USB C270 摄像头虽然自带麦克风，但当前系统未识别 USB Audio，因此暂不作为默认音频输入。

---

## 2. 当前 ALSA 音频设备

### 2.1 采集设备

板端执行：

```bash
arecord -l
```

当前输出显示唯一采集设备为：

```text
card 1: rockchipnau8822 [rockchip-nau8822], device 0: dailink-multicodecs nau8822-hifi-0
```

因此 VisionCast 默认音频采集设备为：

```text
hw:1,0
```

### 2.2 播放设备

板端执行：

```bash
aplay -l
```

当前播放设备包括：

| Card     | 设备                 | 说明               |
| -------- | ------------------ | ---------------- |
| `card 0` | `rockchipdp0`      | DP 音频输出          |
| `card 1` | `rockchip-nau8822` | 板载 Codec，耳机/喇叭输出 |
| `card 2` | `rockchip-hdmi0`   | HDMI 音频输出        |

耳机或板载播放默认使用：

```text
hw:1,0
```

---

## 3. Main Mic 与 Headset Mic 的关系

`arecord -l` 只显示 ALSA PCM 采集设备，不会把每个物理麦克风单独显示成一个声卡。NAU8822 内部可能存在多个输入源，例如：

```text
Main Mic
Headset Mic
Line In / Aux
ADC
PGA
```

但它们最终都进入同一个 ALSA 采集设备：

```text
Main Mic / Headset Mic
    ↓
NAU8822 Input Mixer
    ↓
PGA
    ↓
ADC
    ↓
ALSA Capture PCM hw:1,0
    ↓
VisionCast AudioCapture
```

---

## 4. 当前 mixer 控制项

板端执行：

```bash
amixer -c 1 scontrols
```

当前已发现关键控制项包括：

```text
Headset Mic
Main Mic
ADC
PGA
PGA Boost
Left Input Mixer MicN
Left Input Mixer MicP
Right Input Mixer MicN
Right Input Mixer MicP
Headphone
Speaker
PCM
```

其中与采集相关的重点项是：

| 控件                                 | 作用         |
| ---------------------------------- | ---------- |
| `Main Mic`                         | 主麦克风输入开关   |
| `Headset Mic`                      | 耳机麦克风输入开关  |
| `ADC`                              | 模拟转数字采集通道  |
| `PGA`                              | 模拟前级增益     |
| `PGA Boost`                        | 麦克风增强      |
| `Left/Right Input Mixer MicP/MicN` | 左右声道输入混音路径 |

---

## 5. USB C270 麦克风状态

当前 USB C270 摄像头已识别为视频设备：

```text
/dev/video21
/dev/video22
```

但未识别为 ALSA 音频采集设备。

执行：

```bash
lsmod | grep snd_usb_audio
sudo modprobe snd_usb_audio
```

结果：

```text
modprobe: FATAL: Module snd_usb_audio not found in directory /lib/modules/5.10.209
```

说明当前内核模块目录中缺少 `snd_usb_audio`，因此 C270 自带麦克风暂不可用。

首版结论：

```text
视频调试可以使用 USB C270；
音频采集不使用 USB C270 麦克风；
音频默认使用 NAU8822 hw:1,0。
```

---

## 6. VisionCast 音频链路

首版音频链路：

```text
Main Mic / Headset Mic
    ↓
NAU8822 Codec
    ↓
ALSA hw:1,0
    ↓
PCM Frame
    ↓
Audio Timestamp
    ↓
PCM / G.711 / Opus
    ↓
RTP / RTSP / SRT / WebRTC
```

### 6.1 首版推荐

首版建议先使用：

```text
ALSA PCM Capture    → PCM / G.711    → RTP/RTSP
```

原因：
1. 实现简单；
2. 延迟低；
3. 调试方便；
4. 适合先验证音视频同步。

### 6.2 后续增强

后续进入 WebRTC 阶段后，建议使用：

```text
Opus
```

Opus 更适合实时语音、弱网和 WebRTC 场景。

---

## 7. 推荐音频参数

| 参数   | 首版建议              |
| ---- | ----------------- |
| 采样率  | `48000 Hz`        |
| 声道数  | `1` 或 `2`         |
| 采样格式 | `S16_LE`          |
| 帧长   | `10 ms` 或 `20 ms` |
| 首版编码 | PCM / G.711       |
| 后续编码 | Opus / AAC        |
| 默认设备 | `hw:1,0`          |

---

## 8. 音频时间戳设计

VisionCast 中，音频和视频必须使用同一个时钟源。

推荐使用：

```c
clock_gettime(CLOCK_MONOTONIC, &ts);
```

统一转换为微秒：

```c
uint64_t pts_us = ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
```

原则：
1. 音频采集时立即打 PTS；
2. 视频采集时立即打 PTS；
3. 不使用 system 实时时钟；
4. 不允许音频和视频各自使用不同时间基；
5. 编码、封包、发送阶段都传递采集 PTS。

---

## 9. 调试命令

### 9.1 查询采集设备

```bash
arecord -l
```

### 9.2 查询播放设备

```bash
aplay -l
```

### 9.3 查询声卡

```bash
cat /proc/asound/cards
cat /proc/asound/pcm
```

### 9.4 查询 mixer

```bash
amixer -c 1 scontrols
amixer -c 1 contents
```

### 9.5 图形化 mixer

```bash
alsamixer -c 1
```

进入后：

```text
F4      切到 Capture 页面
左右键  选择控件
M       静音 / 取消静音
上下键  调整音量
```

### 9.6 录音测试

```bash
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/audio_test.wav
```

### 9.7 播放测试

```bash
aplay -D hw:1,0 /tmp/audio_test.wav
```

---

## 10. 项目实现建议

VisionCast 中建议封装为：

```text
AudioCapture
    ↓
AlsaAudioCapture
    ↓
AudioFrame
    ↓
AudioEncoder
    ↓
Packetizer
```

建议文件：

```text
include/media/audio_capture.h
include/platform/rk3588/rk_alsa_device.h
src/media/audio_capture.cpp
src/platform/rk3588/rk_alsa_device.cpp
```

---

## 11. 当前结论

1. 当前系统只有一个 ALSA 采集 PCM 设备：`hw:1,0`；
2. `hw:1,0` 对应 NAU8822 Codec；
3. Main Mic 和 Headset Mic 不是独立声卡，而是 NAU8822 内部输入源；
4. 物理输入源需要通过 `amixer` 或 `alsamixer` 切换；
5. USB C270 麦克风当前不可用；
6. VisionCast 首版默认使用 NAU8822 `hw:1,0` 作为音频采集设备；
7. 后续 WebRTC 阶段推荐引入 Opus 编码。
