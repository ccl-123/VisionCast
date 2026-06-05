# ELF-RK3588 设备节点记录

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 文档作用：记录板端实际枚举到的音视频设备节点，作为后续 V4L2、ALSA、MPP、RGA、RTSP 开发依据。
> 当前状态：13855 MIPI 摄像头作为正式视频入口，USB C270 作为调试备用输入，NAU8822 `hw:1,0` 作为默认音频输入。

---

## 1. 设备节点总览

当前 ELF-RK3588 已识别出以下几类媒体设备：

1. 13855 MIPI 摄像头相关链路：`rkcif`、`rkisp_mainpath`；
2. USB C270 摄像头：`/dev/video21`、`/dev/video22`；
3. HDMI RX 输入：`/dev/video20`；
4. ISP statistics 节点：`/dev/video18`、`/dev/video19`；
5. NAU8822 音频 Codec：`hw:1,0`；
6. DP / HDMI / NAU8822 播放设备。

---

## 2. 视频设备枚举

执行命令：

```bash
v4l2-ctl --list-devices
```

当前输出记录：

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

| 节点                | 类型                  | 说明                   | 是否重点 |
| ----------------- | ------------------- | -------------------- | ---- |
| `/dev/media0`     | Media Controller    | RKCIF / MIPI 前端拓扑    | 是    |
| `/dev/video0~10`  | RKCIF 节点            | MIPI 输入侧相关节点         | 辅助排查 |
| `/dev/media1`     | Media Controller    | RKISP 拓扑             | 是    |
| `/dev/video11~17` | RKISP mainpath 候选节点 | 13855 正式输出候选         | 是    |
| `/dev/video18~19` | RKISP statistics    | ISP 3A 统计数据，不是普通图像   | 否    |
| `/dev/video20`    | HDMI RX             | HDMI 输入采集            | 否    |
| `/dev/video21~22` | USB C270            | USB 摄像头调试输入          | 是，备用 |
| `/dev/media2`     | Media Controller    | USB C270 media graph | 备用   |

---

## 4. 13855 摄像头节点判断

板端已接入 13855 摄像头。其目标链路应为：

```text
13855 Sensor
    → MIPI CSI-2
    → RKCIF
    → RKISP
    → rkisp_mainpath
    → /dev/videoX
```

当前已出现：

```text
/dev/media0
/dev/media1
/dev/video11 ~ /dev/video17
```

说明 Rockchip 摄像头框架已加载，但仍需确认：

1. `media-ctl -p -d /dev/media0` 中是否出现 13855 / ov13855；
2. `media-ctl -p -d /dev/media1` 中 RKISP 链路是否完整；
3. `/dev/video11~17` 哪个节点能正常输出画面；
4. 输出格式是否支持 NV12 / YUYV / NV16；
5. 是否需要启动 RKAIQ 才能获得正常图像。

---

## 5. USB C270 摄像头节点

当前 USB C270 摄像头已识别：

```text
/dev/video21
/dev/video22
/dev/media2
```

用途：

1. 快速验证 V4L2 采集；
2. 快速验证 RGA 格式转换；
3. 快速验证 MPP H.264 编码；
4. 在 13855 适配未完成前作为备用调试输入。

注意：

```text
USB C270 不作为最终正式输入。
```

原因：

1. C270 可能输出 MJPEG，需要额外解码；
2. YUYV 模式下分辨率和帧率可能受限；
3. 不符合最终智能眼镜 MIPI 摄像头形态；
4. 低延迟链路最终应回到 13855 → RKISP → MPP。

---

## 6. 音频采集设备

执行命令：

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

当前采集设备：

```text
hw:1,0
```

结论：

1. 当前系统只有一个 ALSA Capture PCM 设备；
2. 该设备对应 NAU8822 Codec；
3. Main Mic 和 Headset Mic 不会在 `arecord -l` 中显示为两个设备；
4. 物理输入源需要通过 `amixer` / `alsamixer` 切换。

---

## 7. 音频播放设备

执行命令：

```bash
aplay -l
```

当前输出：

```text
**** List of PLAYBACK Hardware Devices ****
card 0: rockchipdp0 [rockchip,dp0], device 0: rockchip,dp0 spdif-hifi-0 [rockchip,dp0 spdif-hifi-0]
  Subdevices: 1/1
  Subdevice #0: subdevice #0

card 1: rockchipnau8822 [rockchip-nau8822], device 0: dailink-multicodecs nau8822-hifi-0 [dailink-multicodecs nau8822-hifi-0]
  Subdevices: 1/1
  Subdevice #0: subdevice #0

card 2: rockchiphdmi0 [rockchip-hdmi0], device 0: rockchip-hdmi0 i2s-hifi-0 [rockchip-hdmi0 i2s-hifi-0]
  Subdevices: 1/1
  Subdevice #0: subdevice #0
```

播放设备映射：

| 设备       | 作用              |
| -------- | --------------- |
| `hw:0,0` | DP 音频输出         |
| `hw:1,0` | NAU8822，耳机/板载播放 |
| `hw:2,0` | HDMI 音频输出       |

---

## 8. ALSA 声卡列表

执行命令：

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

---

## 9. USB 摄像头麦克风状态

当前 C270 视频设备已识别，但其 USB 麦克风没有出现在 `arecord -l` 中。

检查命令：

```bash
lsmod | grep snd_usb_audio
sudo modprobe snd_usb_audio
```

当前结果：

```text
modprobe: FATAL: Module snd_usb_audio not found in directory /lib/modules/5.10.209
```

结论：

1. 当前内核模块目录中没有 `snd_usb_audio`；
2. USB C270 麦克风暂不可用；
3. 首版不使用 USB 摄像头麦克风；
4. 首版音频统一使用 NAU8822 `hw:1,0`。

---

## 10. 当前默认设备映射

| 功能           | 默认设备                    |
| ------------ | ----------------------- |
| 正式视频输入       | 13855 MIPI 摄像头          |
| 正式视频输出候选     | `/dev/video11~17`       |
| 调试视频输入       | USB C270 `/dev/video21` |
| 调试视频辅助节点     | `/dev/video22`          |
| 音频采集         | NAU8822 `hw:1,0`        |
| 耳机/板载播放      | NAU8822 `hw:1,0`        |
| HDMI 音频播放    | `hw:2,0`                |
| DP 音频播放      | `hw:0,0`                |
| USB C270 麦克风 | 当前不可用                   |

---

## 11. 下一步排查命令

### 11.1 查询 13855 / MIPI / ISP 日志

```bash
dmesg | grep -Ei "13855|ov13855|camera|mipi|csi|rkcif|rkisp"
```

### 11.2 查询 media graph

```bash
media-ctl -p -d /dev/media0
media-ctl -p -d /dev/media1
```

### 11.3 查询 RKISP mainpath 候选节点

```bash
for i in {11..17}; do
    echo "===== /dev/video$i ====="
    v4l2-ctl -d /dev/video$i -D
    v4l2-ctl -d /dev/video$i --list-formats-ext
done
```

### 11.4 查询 C270 节点

```bash
v4l2-ctl -d /dev/video21 -D
v4l2-ctl -d /dev/video21 --list-formats-ext
v4l2-ctl -d /dev/video22 -D
v4l2-ctl -d /dev/video22 --list-formats-ext
```

---

## 12. 当前结论

ELF-RK3588 当前已具备 VisionCast 项目的基础媒体设备条件：

1. MIPI/RKCIF/RKISP 框架已枚举；
2. 13855 摄像头应作为正式视频输入继续适配；
3. USB C270 可作为 V4L2 调试备用；
4. NAU8822 `hw:1,0` 是当前唯一音频采集设备；
5. USB C270 麦克风当前不可用；
6. 下一步核心任务是确认 13855 最终对应哪个 `/dev/videoX`。
