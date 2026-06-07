# ELF-RK3588 设备枚举与探测文档 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 目的：记录板端音视频设备枚举结果，作为 V4L2、ALSA、RGA、MPP、RTSP 开发与配置的真实物理依据。

---

## 1. 文档目的

本文档记录 ELF-RK3588 开发板端实际识别、运行的音视频设备信息，固化项目开发调试所用到的核心物理节点，避免在开发或部署中混淆 `/dev/videoX` 与 `hw:X,Y`。

---

## 2. 视频物理设备枚举

板端执行 `v4l2-ctl --list-devices` 输出的物理设备与虚拟节点分配如下：

```text
rk_hdmirx (fdee0000.hdmirx-controller):
        /dev/video20

rkisp-statistics (platform: rkisp):
        /dev/video18
        /dev/video19

rkcif-mipi-lvds (platform:rkcif):
        /dev/media0

rkcif (platform:rkcif-mipi-lvds):
        /dev/video0 ~ /dev/video10

rkisp_mainpath (platform:rkisp0-vir0):
        /dev/video11 ~ /dev/video17
        /dev/media1

C270 HD WEBCAM (usb-fc880000.usb-1.2):
        /dev/video21
        /dev/video22
        /dev/media2
```

---

## 3. 视频节点分类与用途说明

| 节点 | 类型 | 说明 | 是否使用 |
| --- | --- | --- | --- |
| `/dev/media0` | Media Controller | RKCIF / MIPI 物理拓扑 | 用于调试和配置连接 |
| `/dev/video0~10` | RKCIF 节点 | MIPI 物理接收前端 | 不直接读取数据 |
| `/dev/media1` | Media Controller | RKISP 图像处理拓扑 | 用于确认 ISP 与 Sensor 链路 |
| `/dev/video11` | V4L2 Capture | **13855 MIPI CSI 采集主节点** | 是，NV12 30FPS 图像源 |
| `/dev/video12~17` | V4L2 Capture | ISP mainpath/selfpath 备选节点 | 否 |
| `/dev/video18~19` | statistics | ISP 3A 统计数据，非画面节点 | 后台 rkaiq 守护进程读取使用 |
| `/dev/video20` | HDMI RX | 外部 HDMI 输入采集 | 否 |
| `/dev/video21` | V4L2 Capture | **USB C270 采集备用节点** | 是，主节点异常时降级使用 |
| `/dev/video22` | metadata | USB C270 metadata 节点 | 否 |
| `/dev/media2` | Media Controller | USB C270 拓扑 | 否 |

---

## 4. 13855 MIPI 摄像头状态确认
- **拓扑连接**：经由 `/dev/media1` 拓扑确认，13855 Sensor 物理连接至 CSI-2 DPHY，然后绑定到 RKISP 处理管道。
- **采集规格**：`/dev/video11` 具备 MPLANE (多平面) 视频采集能力，稳定输出 NV12 格式帧，规格为 `1280x720`，支持 30 FPS。
- **帧率锁定**：系统通过向 `/dev/v4l-subdev2` 锁定曝光和垂直消隐 (vblank)，彻底解决了弱光下自动曝光导致帧率降到 15 FPS 的问题，锁定帧率为稳定 **30 FPS**。

---

## 5. 音频采集与播放设备确认

### 5.1 采集设备
执行 `arecord -l` 输出：
```text
card 1: rockchipnau8822 [rockchip-nau8822], device 0: dailink-multicodecs nau8822-hifi-0
```
- **配置节点**：`hw:1,0`
- **通道形式**：物理 Main Mic 与 Headset Mic 均接入此声卡，由 ALSA Mixer 调整采集增益与输入偏置。

### 5.2 播放设备
执行 `aplay -l` 输出包含 DP、NAU8822 和 HDMI 播放：
- **`hw:0,0`**：DP 音频输出。
- **`hw:1,0`**：板载 nau8822 耳机或喇叭输出。
- **`hw:2,0`**：HDMI 音频输出。

---

## 6. USB C270 麦克风状态

板端未识别 USB 摄像头麦克风：
- 执行 `lsmod | grep snd_usb_audio` 无输出。
- 执行 `sudo modprobe snd_usb_audio` 提示 `Module snd_usb_audio not found in directory /lib/modules/5.10.209`。
- 结论：内核驱动包中缺失 UVC 音频驱动，系统无法使用 USB 摄像头作为音频输入。音频输入统一只使用板载 NAU8822 (`hw:1,0`)。

---

## 7. 物理节点汇总与最终结论

| 功能 | 物理节点 | 最终选定规格 | 验证状态 |
| --- | --- | --- | --- |
| 视频主输入 | `/dev/video11` | NV12, 1280x720, 30 FPS | 已打通，稳定采集 |
| 视频备输入 | `/dev/video21` | MJPEG, 1280x720, 30 FPS | 已打通，降级支持 |
| 音频主输入 | `hw:1,0` | S16_LE, 48000Hz, 双转单混音 | 已打通，稳定采集 |
| 音频输出 | `hw:1,0` | 板载耳机孔 | 辅助调试通过 |
| 3A 控制服务 | `/dev/video18/19` | 3A 统计与参数反馈 | 后台运行正常 |

在最终版本中，所有设备枚举节点与输入格式均已在配置文件中完成固化，无需手动查找或执行探测脚本。
