# ELF-RK3588 设备枚举文档

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 目的：记录当前板端音视频设备枚举结果，作为后续 V4L2、ALSA、RGA、MPP、RTSP 开发依据

---

## 1. 文档目的

本文档记录 ELF-RK3588 板端已经识别到的真实音视频设备，包括：1. MIPI 摄像头链路；2. USB 摄像头节点；3. RKISP / RKCIF 节点；4. 音频采集设备；5. 音频播放设备；6. 当前可用输入源；7. 当前不可用设备和原因。

该文档用于固定项目基础事实，避免后续开发中混淆 `/dev/videoX`、`hw:X,Y`、mainpath、statistics 等节点。

---

## 2. 视频设备枚举

板端执行：

```bash
v4l2-ctl --list-devices
```

当前输出：

```text
rk_hdmirx (fdee0000.hdmirx-controller):
        /dev/video20

rkisp-statistics (platform: rkisp):
        /dev/video18
        /dev/video19

rkcif-mipi-lvds (platform:rkcif):
        /dev/media0

rkcif (platform:rkcif-mipi-lvds):
        /dev/video0
        /dev/video1
        /dev/video2
        /dev/video3
        /dev/video4
        /dev/video5
        /dev/video6
        /dev/video7
        /dev/video8
        /dev/video9
        /dev/video10

rkisp_mainpath (platform:rkisp0-vir0):
        /dev/video11
        /dev/video12
        /dev/video13
        /dev/video14
        /dev/video15
        /dev/video16
        /dev/video17
        /dev/media1

C270 HD WEBCAM (usb-fc880000.usb-1.2):
        /dev/video21
        /dev/video22
        /dev/media2
```

---

## 3. 视频节点分类

| 节点                | 类型                  | 说明             | 是否首版重点 |
| ----------------- | ------------------- | -------------- | ------ |
| `/dev/media0`     | Media Controller    | RKCIF 拓扑       | 是      |
| `/dev/video0~10`  | RKCIF 节点            | 靠近 MIPI 输入侧    | 辅助排查   |
| `/dev/media1`     | Media Controller    | RKISP 拓扑       | 是      |
| `/dev/video11~17` | RKISP mainpath 候选节点 | 13855 正式输出候选   | 是      |
| `/dev/video18~19` | RKISP statistics    | 3A 统计数据，不是普通画面 | 否      |
| `/dev/video20`    | HDMI RX             | HDMI 输入        | 否      |
| `/dev/video21~22` | USB C270            | 调试备用摄像头        | 是，备用   |

---

## 4. 13855 摄像头判断

当前板端接有 13855 摄像头。它应属于 MIPI CSI 摄像头链路：

```text
13855 Sensor    → MIPI CSI-2    → RKCIF    → RKISP    → rkisp_mainpath    → /dev/videoX
```

当前已经枚举出：

```text
/dev/media0
/dev/media1
/dev/video11 ~ /dev/video17
```

这说明 Rockchip 摄像头框架已加载，但还不能直接断定 `/dev/video11` 就是最终输出节点。需要继续通过 `media-ctl` 和 `v4l2-ctl` 确认。

---

## 5. USB C270 摄像头

当前 USB C270 已识别为：

```text
/dev/video21
/dev/video22
/dev/media2
```

C270 用途：
1. 快速验证 V4L2 采集；
2. 快速验证 RGA 转换；
3. 快速验证 MPP 编码；
4. 在 13855 未完全适配前作为调试备用输入。

C270 不作为最终方案，因为其可能输出 MJPEG 或 YUYV，不一定适合最终低延迟硬件链路。

---

## 6. 音频采集设备

板端执行：

```bash
arecord -l
```

当前输出：

```text
**** List of CAPTURE Hardware Devices ****
card 1: rockchipnau8822 [rockchip-nau8822], device 0: dailink-multicodecs nau8822-hifi-0 [dailink-multicodecs nau8822-hifi-0]
  Subdevices: 1/1
  Subdevice #0: subdevice #0
```

因此当前唯一录音设备为：

```text
hw:1,0
```

---

## 7. 音频播放设备

板端执行：

```bash
aplay -l
```

当前输出：

```text
**** List of PLAYBACK Hardware Devices ****
card 0: rockchipdp0 [rockchip,dp0], device 0: rockchip,dp0 spdif-hifi-0
card 1: rockchipnau8822 [rockchip-nau8822], device 0: dailink-multicodecs nau8822-hifi-0
card 2: rockchiphdmi0 [rockchip-hdmi0], device 0: rockchip-hdmi0 i2s-hifi-0
```

播放设备映射：

| 设备       | 说明              |
| -------- | --------------- |
| `hw:0,0` | DP 音频输出         |
| `hw:1,0` | NAU8822，耳机/板载音频 |
| `hw:2,0` | HDMI 音频输出       |

---

## 8. ALSA 声卡

板端执行：

```bash
cat /proc/asound/cards
```

当前输出：

```text
0 [rockchipdp0    ]: rockchip_dp0 - rockchip,dp0
                      rockchip,dp0
1 [rockchipnau8822]: rockchip-nau882 - rockchip-nau8822
                      rockchip-nau8822
2 [rockchiphdmi0  ]: rockchip-hdmi0 - rockchip-hdmi0
                      rockchip-hdmi0
```

当前音频结论：

```text
采集：hw:1,0
耳机/板载播放：hw:1,0
HDMI 播放：hw:2,0
DP 播放：hw:0,0
```

---

## 9. USB 摄像头麦克风状态

当前 USB C270 的视频部分已经识别，但音频麦克风未识别。

检查命令：

```bash
lsmod | grep snd_usb_audio
sudo modprobe snd_usb_audio
```

结果：

```text
modprobe: FATAL: Module snd_usb_audio not found in directory /lib/modules/5.10.209
```

说明当前内核模块目录中没有 `snd_usb_audio`，因此 C270 麦克风暂不可用。首版不使用 USB 摄像头麦克风。

---

## 10. 当前默认设备表

| 功能         | 默认设备                    |
| ---------- | ----------------------- |
| 正式视频输入     | 13855 MIPI 摄像头          |
| 正式视频输出候选   | `/dev/video11~17`       |
| 调试视频输入     | USB C270 `/dev/video21` |
| 音频采集       | NAU8822 `hw:1,0`        |
| 耳机播放       | NAU8822 `hw:1,0`        |
| HDMI 播放    | `hw:2,0`                |
| DP 播放      | `hw:0,0`                |
| USB 摄像头麦克风 | 当前不可用                   |

---

## 11. 下一步需要确认

### 11.1 13855 media graph

```bash
media-ctl -p -d /dev/media0
media-ctl -p -d /dev/media1
```

目标确认：
1. 是否存在 `13855` 或 `ov13855`；
2. Sensor 是否连接到 CSI-2 DPHY；
3. RKCIF 是否连接到 RKISP；
4. RKISP mainpath 对应哪个 video node；
5. 输出格式和分辨率。

### 11.2 RKISP mainpath 节点能力

```bash
for i in {11..17}; do
    echo "===== /dev/video$i ====="
    v4l2-ctl -d /dev/video$i -D
    v4l2-ctl -d /dev/video$i --list-formats-ext
done
```

重点找：

```text
NV12
YUYV
NV16
1280x720
1920x1080
30fps
```

### 11.3 NAU8822 录音源

```bash
amixer -c 1 sget 'Main Mic'
amixer -c 1 sget 'Headset Mic'
amixer -c 1 sget 'ADC'
amixer -c 1 sget 'PGA'
```

---

## 12. 当前结论

VisionCast 当前硬件基础已经具备：
1. MIPI 摄像头框架已出现 RKCIF/RKISP 节点；
2. 13855 应作为正式视频输入继续适配；
3. USB C270 可作为备用调试摄像头；
4. NAU8822 `hw:1,0` 是当前唯一音频采集设备；
5. USB 摄像头麦克风暂不可用；
6. 下一步核心任务是确认 13855 的最终 V4L2 输出节点。
