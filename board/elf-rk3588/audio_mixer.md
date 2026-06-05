# ELF-RK3588 NAU8822 Mixer 记录

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> Codec：rockchip-nau8822
> ALSA 设备：`hw:1,0`
> 文档作用：记录 NAU8822 的输入源、mixer 控制项和调试方法。

---

## 1. 当前音频结构

当前 ELF-RK3588 只有一个 ALSA 采集 PCM 设备：

```text
hw:1,0
```

该设备对应：

```text
card 1: rockchip-nau8822
```

虽然系统可能存在 Main Mic、Headset Mic 等多个物理输入源，但它们不会显示为多个 `arecord` 设备，而是通过 NAU8822 内部 mixer 切换。

逻辑结构：

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

## 2. 当前采集设备

执行：

```bash
arecord -l
```

输出：

```text
card 1: rockchipnau8822 [rockchip-nau8822], device 0: dailink-multicodecs nau8822-hifi-0
```

采集设备：

```text
hw:1,0
```

---

## 3. 当前播放设备

执行：

```bash
aplay -l
```

其中 NAU8822 播放设备为：

```text
card 1: rockchipnau8822 [rockchip-nau8822], device 0
```

播放设备：

```text
hw:1,0
```

该设备通常对应耳机、板载喇叭或模拟音频输出。

---

## 4. Mixer 控制项

执行：

```bash
amixer -c 1 scontrols
```

当前已识别控制项包括：

```text
Headphone
Headphone ZC
Speaker
Speaker ZC
PCM
I2STDM Digital Loopback Mode
Aux Boost
ADC
ADC 128x Oversampling
ADC Companding
ADC Inversion
ALC Attack
ALC Decay
ALC Enable
ALC Hold
ALC Max Gain
ALC Min Gain
ALC Mode
ALC Noise Gate
ALC Noise Gate Threshold
ALC Target
DAC 128x Oversampling
DAC Companding
DAC Inversion
DAC Limiter
Digital Loopback
EQ Function
Headset Mic
High Pass Cut Off
High Pass Filter
L2/R2 Boost
Left Input Mixer L2
Left Input Mixer MicN
Left Input Mixer MicP
Main Mic
PGA
PGA Boost
PGA ZC
Right Input Mixer MicN
Right Input Mixer MicP
Right Input Mixer R2
Transmit SDO0 Source Select
Transmit SDO1 Source Select
Transmit SDO2 Source Select
Transmit SDO3 Source Select
Receive PATH0 Source Select
Receive PATH1 Source Select
Receive PATH2 Source Select
Receive PATH3 Source Select
```

---

## 5. 采集相关重点控件

| 控件                       | 作用            |
| ------------------------ | ------------- |
| `Main Mic`               | 主麦克风输入        |
| `Headset Mic`            | 耳机麦克风输入       |
| `ADC`                    | 模拟转数字采集通道     |
| `PGA`                    | 前级模拟增益        |
| `PGA Boost`              | 麦克风增益增强       |
| `Left Input Mixer MicP`  | 左声道 MicP 输入路径 |
| `Left Input Mixer MicN`  | 左声道 MicN 输入路径 |
| `Right Input Mixer MicP` | 右声道 MicP 输入路径 |
| `Right Input Mixer MicN` | 右声道 MicN 输入路径 |
| `ALC Enable`             | 自动电平控制        |
| `High Pass Filter`       | 高通滤波          |

---

## 6. Main Mic 调试

### 6.1 查询状态

```bash
amixer -c 1 sget 'Main Mic'
amixer -c 1 sget 'ADC'
amixer -c 1 sget 'PGA'
amixer -c 1 sget 'PGA Boost'
```

### 6.2 录音测试

```bash
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/main_mic.wav
```

### 6.3 播放测试

```bash
aplay -D hw:1,0 /tmp/main_mic.wav
```

### 6.4 判断方法

1. 录音时对板载麦克风位置讲话；
2. 播放后确认是否有声音；
3. 若声音过小，检查 `ADC`、`PGA`、`PGA Boost`；
4. 若无声音，切换 `Main Mic`、`Headset Mic` 或输入 mixer。

---

## 7. Headset Mic 调试

### 7.1 查询状态

```bash
amixer -c 1 sget 'Headset Mic'
amixer -c 1 sget 'ADC'
amixer -c 1 sget 'PGA'
amixer -c 1 sget 'PGA Boost'
```

### 7.2 录音测试

```bash
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/headset_mic.wav
```

### 7.3 播放测试

```bash
aplay -D hw:1,0 /tmp/headset_mic.wav
```

### 7.4 注意事项

如果插入的是普通三段式耳机，没有麦克风，则 Headset Mic 不会有录音输入。

如果插入的是四段式耳麦，也可能由于接口标准或硬件接线不同导致麦克风不可用。

---

## 8. alsamixer 调试方法

进入：

```bash
alsamixer -c 1
```

操作：

```text
F4      切到 Capture 页面
左右键  选择控件
M       静音 / 取消静音
上下键  调节音量
Esc     退出
```

重点检查：

```text
Main Mic
Headset Mic
ADC
PGA
PGA Boost
Left Input Mixer MicP
Left Input Mixer MicN
Right Input Mixer MicP
Right Input Mixer MicN
```

---

## 9. VisionCast 默认音频策略

首版默认使用：

```text
ALSA device: hw:1,0
Sample rate: 48000
Format: S16_LE
Channels: 1 或 2
Frame duration: 10 ms / 20 ms
```

首版音频链路：

```text
NAU8822 Main Mic / Headset Mic
    → ALSA Capture hw:1,0
    → PCM Frame
    → Audio PTS
    → PCM / G.711
    → RTP / RTSP
```

后续 WebRTC 阶段：

```text
NAU8822
    → ALSA
    → Opus
    → WebRTC
```

---

## 10. USB C270 麦克风结论

当前 C270 麦克风不可用。

原因：

```text
snd_usb_audio 模块不存在
```

命令结果：

```text
modprobe: FATAL: Module snd_usb_audio not found in directory /lib/modules/5.10.209
```

因此首版不考虑 USB C270 麦克风。

---

## 11. 当前结论

1. `hw:1,0` 是当前唯一录音 PCM 设备；
2. 该设备对应 NAU8822 Codec；
3. Main Mic 和 Headset Mic 都挂在 NAU8822 内部；
4. 需要通过 mixer 切换物理输入源；
5. VisionCast 默认使用 `hw:1,0` 作为音频输入；
6. 后续需要编写 `setup_main_mic.sh` 和 `setup_headset_mic.sh` 脚本固化 mixer 配置。
