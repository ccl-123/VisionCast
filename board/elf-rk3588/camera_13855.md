# ELF-RK3588 13855 摄像头适配记录

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 摄像头：13855 MIPI 摄像头
> 文档作用：记录 13855 摄像头的识别、media graph、V4L2 输出节点和后续适配方向。

---

## 1. 摄像头定位

13855 摄像头是 VisionCast 的正式视频输入，用于模拟智能眼镜第一视角采集。

它不是 USB 摄像头，而是 MIPI CSI 摄像头。目标链路为：

```text
13855 Sensor
    → MIPI CSI-2
    → RKCIF
    → RKISP
    → rkisp_mainpath
    → V4L2 /dev/videoX
    → RGA / MPP
    → RTSP/SRT/WebRTC
```

---

## 2. 当前系统状态

当前 `v4l2-ctl --list-devices` 已枚举出：

```text
rkcif-mipi-lvds:
    /dev/media0

rkcif:
    /dev/video0 ~ /dev/video10

rkisp_mainpath:
    /dev/video11 ~ /dev/video17
    /dev/media1

rkisp-statistics:
    /dev/video18
    /dev/video19
```

说明：

1. Rockchip MIPI 摄像头框架已出现；
2. RKCIF 节点已存在；
3. RKISP mainpath 候选节点已存在；
4. 需要进一步确认 13855 sensor 是否已经成功 probe；
5. 需要确认最终采集节点是 `/dev/video11~17` 中哪一个。

---

## 3. 13855 数据流

13855 输出的是 RAW Bayer 数据，不是 H.264，也不是 NV12。

完整链路：

```text
13855 Sensor
    ↓ RAW Bayer over MIPI CSI-2
CSI-2 DPHY
    ↓
RKCIF
    ↓
RKISP
    ↓ NV12 / YUYV / NV16
V4L2 mainpath
    ↓
RGA，可选
    ↓
MPP H.264 / H.265
    ↓
RTSP/RTP/SRT/WebRTC
```

---

## 4. Sensor 控制流

13855 的控制命令通过 I2C 完成：

```text
RK3588 Kernel Driver
    ↔ I2C
13855 Sensor Registers
```

控制内容：

1. Sensor ID 读取；
2. 分辨率设置；
3. 帧率设置；
4. 曝光；
5. 增益；
6. MIPI lane 数；
7. RAW bit 深度；
8. Stream On / Off。

如果 I2C 失败，常见日志包括：

```text
probe failed
sensor id mismatch
i2c read failed
```

---

## 5. RKCIF 链路

当前 RKCIF 相关节点：

```text
/dev/media0
/dev/video0 ~ /dev/video10
```

RKCIF 作用：

1. 接收 MIPI CSI-2；
2. 解析数据包；
3. 处理 Virtual Channel；
4. 与 RKISP 建立链路；
5. 可通过 DMA 输出部分数据。

VisionCast 正式推流不优先直接使用 RKCIF 原始节点，而是使用：

```text
RKCIF → RKISP → mainpath
```

---

## 6. RKISP 链路

当前 RKISP 相关节点：

```text
/dev/media1
/dev/video11 ~ /dev/video17
/dev/video18
/dev/video19
```

其中：

| 节点                | 作用                     |
| ----------------- | ---------------------- |
| `/dev/video11~17` | mainpath 候选输出          |
| `/dev/video18~19` | statistics 统计数据，不是普通画面 |
| `/dev/media1`     | RKISP media graph      |

RKISP 负责：

1. 黑电平校正；
2. 坏点校正；
3. 去马赛克；
4. 自动曝光；
5. 自动白平衡；
6. 降噪；
7. 色彩校正；
8. Gamma；
9. 锐化；
10. YUV 转换。

---

## 7. RKAIQ 关系

RKAIQ 负责 ISP 的 3A 控制：

```text
RKISP statistics
    → RKAIQ
    → RKISP params
```

如果 RKAIQ 未正常工作，可能出现：

1. 画面过暗；
2. 画面过曝；
3. 画面偏绿；
4. 白平衡异常；
5. 噪声大；
6. 图像质量差。

后续正式使用 13855 时，需要确认是否存在对应 IQ 文件。

---

## 8. 当前关键排查命令

### 8.1 查看内核日志

```bash
dmesg | grep -Ei "13855|ov13855|camera|mipi|csi|rkcif|rkisp"
```

### 8.2 查看 I2C 设备

```bash
for f in /sys/bus/i2c/devices/*/name; do
    echo "$f: $(cat $f)"
done | grep -Ei "ov|13855|camera|imx|gc"
```

### 8.3 查看 RKCIF 拓扑

```bash
media-ctl -p -d /dev/media0
```

### 8.4 查看 RKISP 拓扑

```bash
media-ctl -p -d /dev/media1
```

### 8.5 查看 mainpath 候选节点

```bash
for i in {11..17}; do
    echo "===== /dev/video$i ====="
    v4l2-ctl -d /dev/video$i -D
    v4l2-ctl -d /dev/video$i --list-formats-ext
done
```

---

## 9. 目标输出格式

MPP 编码器最适合接收：

```text
NV12
```

---

## 10. 适配目标

| 项目          | 目标                 |
| ----------- | ------------------ |
| Sensor 驱动   | 正常 probe           |
| Media Graph | 出现 13855 / ov13855 |
| RKCIF 链路    | 正常连接               |
| RKISP 链路    | 正常连接               |
| 输出节点        | 明确 `/dev/videoX`   |
| 输出格式        | 优先 NV12            |
| 输出分辨率       | 720p30 / 1080p30   |
| RKAIQ       | 正常工作               |
| MPP 输入      | 可直接或经 RGA 接入       |
| 推流协议        | 首版 RTSP/RTP        |

---

## 11. 可能问题与判断

| 现象                   | 可能原因                    |
| -------------------- | ----------------------- |
| dmesg 无 13855        | 设备树未启用或驱动未加载            |
| sensor probe failed  | I2C 地址、供电、时钟、reset 异常   |
| media graph 无 sensor | 驱动或设备树链路异常              |
| stream-mmap 失败       | 节点错误或链路未配置              |
| 黑屏                   | MIPI link、曝光、RKAIQ、格式异常 |
| 偏色                   | Bayer 顺序或 IQ 文件异常       |
| MPP 编码失败             | 输入格式不匹配                 |
| 帧率不稳                 | buffer、ISP、编码、CPU 调度问题  |

---

## 12. 首版目标

首版目标：

```text
13855
    → RKISP mainpath
    → V4L2
    → MPP H.264
    → RTSP/RTP
```

如果 13855 暂未打通，则使用 C270 先验证软件管线：

```text
USB C270
    → V4L2
    → RGA
    → MPP H.264
    → RTSP/RTP
```

---

## 13. 当前待补充内容

以下内容需要后续实际命令输出补充：

```text
TODO: media-ctl -p -d /dev/media0 输出
TODO: media-ctl -p -d /dev/media1 输出
TODO: /dev/video11~17 格式能力
TODO: 13855 dmesg 日志
TODO: 13855 最终采集节点
TODO: RKAIQ IQ 文件路径
```

---

## 14. 当前结论

1. 13855 是 VisionCast 的正式摄像头输入；
2. 当前系统已出现 RKCIF/RKISP 相关节点；
3. 还需要确认 sensor 是否成功进入 media graph；
4. 最终应从 `/dev/video11~17` 中确定 mainpath 输出；
5. MPP 最理想输入格式是 NV12；
6. 若格式不匹配，需要 RGA 做转换；
7. 首版协议使用 RTSP/RTP。
