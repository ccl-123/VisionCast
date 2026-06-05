# 13855 摄像头数据流说明

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 摄像头：13855 MIPI 摄像头
> 目标：作为正式第一视角视频输入，进入 RKISP、MPP 和推流管线

---

## 1. 文档目的

本文档说明 13855 摄像头在 ELF-RK3588 上的完整数据流，包括：
1. Sensor 原始图像数据流；
2. Sensor 控制流；
3. MIPI CSI-2 接收流；
4. RKCIF 数据流；
5. RKISP 图像处理流；
6. RKAIQ 3A 控制流；
7. V4L2 输出流；
8. RGA/MPP 编码推流流。

该文档用于指导后续 13855 摄像头适配、V4L2 采集、RGA 转换、MPP 编码与低延迟推流实现。

---

## 2. 总体链路

13855 摄像头不是直接输出 H.264，也不是直接输出 NV12。它首先输出 RAW Bayer 原始图像数据，然后由 RK3588 的 RKCIF 和 RKISP 完成接收与图像处理。

完整链路如下：

```text
13855 Sensor
    ↓ RAW Bayer over MIPI CSI-2
MIPI CSI-2 DPHY
    ↓
RKCIF
    ↓
RKISP
    ↓
rkisp_mainpath / rkisp_selfpath
    ↓
V4L2 /dev/videoX
    ↓
RGA，可选
    ↓
MPP H.264/H.265 Encoder
    ↓
RTSP/RTP/SRT/WebRTC
```

---

## 3. 第一类数据流：Sensor 图像原始数据流

13855 Sensor 负责将光信号转换为 RAW Bayer 图像数据。可能的 RAW 格式包括：

```text
SRGGB10
SBGGR10
SGBRG10
SGRBG10
RAW10
RAW12
```

RAW Bayer 数据特点：
1. 未去马赛克；
2. 未白平衡；
3. 未降噪；
4. 未色彩校正；
5. 不能直接作为正常图像显示；
6. 不能直接送入 MPP 编码器。

因此必须经过 RKISP 处理后，才能形成正常的 YUV/NV12/YUYV 图像。

---

## 4. 第二类数据流：Sensor 控制流

图像数据走 MIPI CSI-2，控制命令走 I2C。

```text
RK3588 CPU / Kernel Sensor Driver    ↔ I2C
13855 Sensor Registers
```

I2C 控制内容包括：
1. Sensor ID 读取；
2. 分辨率配置；
3. 帧率配置；
4. 曝光时间；
5. 模拟增益；
6. 数字增益；
7. MIPI lane 数；
8. RAW bit 深度；
9. 翻转/镜像；
10. Stream On / Stream Off。

如果 I2C 控制失败，常见现象：

```text
probe failed
sensor id mismatch
i2c read failed
media graph 中没有 13855
```

如果 I2C 成功但 MIPI 异常，常见现象：

```text
media graph 有 sensor
stream-on 失败
采集不到帧
画面黑屏
EPIPECSI error
```

---

## 5. 第三类数据流：MIPI CSI-2 传输流

13855 通过 MIPI CSI-2 向 RK3588 输出高速图像数据。

链路：

```text
13855 Sensor
    ↓ MIPI CSI-2 Lane
CSI-2 DPHY
    ↓
RKCIF
```

MIPI CSI-2 负责传输图像数据包，通常包括：
1. Virtual Channel；
2. Data Type；
3. RAW10 / RAW12 payload；
4. Frame Start / Frame End；
5. Line Start / Line End。

VisionCast 不直接操作 MIPI 包，而是通过内核驱动、Media Controller 和 V4L2 使用摄像头输出。

---

## 6. 第四类数据流：RKCIF 接收流

当前板端已经识别：

```text
rkcif-mipi-lvds:
    /dev/media0
rkcif:
    /dev/video0 ~ /dev/video10
```

RKCIF 是 Rockchip 摄像头输入前端，作用包括：
1. 接收 MIPI CSI-2 数据；
2. 解析数据类型；
3. 处理虚拟通道；
4. 与 RKISP 建立链路；
5. 必要时通过 DMA 输出图像数据。

正式推流链路一般不直接使用 RKCIF 输出作为最终图像，而是使用：

```text
RKCIF    → RKISP    → rkisp_mainpath
```

原因是 RKCIF 阶段的数据更接近 RAW 输入侧，尚未完成完整 ISP 处理。

---

## 7. 第五类数据流：RKISP 图像处理流

RKISP 是 13855 正式图像链路的核心处理模块。

当前板端已识别：

```text
rkisp_mainpath:
    /dev/video11 ~ /dev/video17
    /dev/media1
rkisp-statistics:
    /dev/video18
    /dev/video19
```

RKISP 处理内容包括：
1. 黑电平校正；
2. 坏点校正；
3. 镜头阴影校正；
4. 去马赛克；
5. 自动曝光；
6. 自动白平衡；
7. 自动对焦，如果硬件支持；
8. 降噪；
9. 色彩校正；
10. Gamma；
11. 锐化；
12. YUV 转换。

RKISP 处理后，图像才适合显示、编码和推流。

---

## 8. 第六类数据流：RKAIQ 3A 控制流

RKAIQ 与 RKISP 形成 3A 控制闭环。

```text
RKISP    ↓ statistics
RKAIQ    ↓ params
RKISP
```

### 8.1 Statistics 流

`/dev/video18` 和 `/dev/video19` 属于 statistics 节点，用于输出 ISP 统计信息，不是普通画面节点。

统计信息包括：
1. AE 自动曝光统计；
2. AWB 自动白平衡统计；
3. AF 自动对焦统计；
4. Histogram；
5. 亮度统计；
6. 色彩统计。

### 8.2 Params 流

RKAIQ 根据 statistics 计算 ISP 参数，再写回 RKISP。

参数包括：
1. 曝光；
2. 增益；
3. 白平衡；
4. 黑电平；
5. 色彩校正；
6. 降噪；
7. 锐化；
8. Gamma。

没有 RKAIQ 或 IQ 文件不正确时，画面可能偏色、过暗、过曝或噪声异常。

---

## 9. 第七类数据流：V4L2 输出流

最终应用层应从 RKISP mainpath 或 selfpath 获取正常图像。

当前候选节点：

```text
/dev/video11
/dev/video12
/dev/video13
/dev/video14
/dev/video15
/dev/video16
/dev/video17
```

需要逐个确认：

```bash
for i in {11..17}; do
    echo "===== /dev/video$i ====="
    v4l2-ctl -d /dev/video$i -D
    v4l2-ctl -d /dev/video$i --list-formats-ext
done
```

重点寻找：

```text
NV12
YUYV
NV16
1280x720
1920x1080
30fps
```

最适合 MPP 编码的格式：

```text
NV12
```

---

## 10. 第八类数据流：RGA / MPP 编码推流流

如果 V4L2 输出已经是 NV12，且分辨率符合编码要求，可以直接送入 MPP：

```text
V4L2 NV12 Frame    → MPP H.264 Encoder    → RTP/RTSP
```

如果 V4L2 输出格式不是 NV12，或者分辨率需要调整，则通过 RGA：

```text
V4L2 YUYV / NV16 / RGB    → RGA Convert / Resize    → NV12    → MPP H.264/H.265 Encoder    → RTP/RTSP/SRT/WebRTC
```

---

## 11. 首版目标链路

VisionCast 首版建议链路：

```text
13855 Sensor    → RKCIF    → RKISP    → rkisp_mainpath    → V4L2 Capture    → MPP H.264    → RTSP/RTP
```

如果 13855 暂未完全适配，则使用 USB C270 调试：

```text
USB C270 /dev/video21    → V4L2 Capture    → RGA Convert    → MPP H.264    → RTSP/RTP
```

---

## 12. 优化目标链路

后续低延迟优化目标：

```text
13855    → RKISP mainpath    → V4L2 DMA-BUF FD    → RGA Import FD，可选    → MPP Import FD    → H.264/H.265    → SRT / WebRTC
```

优化重点：
1. 减少 memcpy；
2. 使用 DMA-BUF；
3. 控制 V4L2 buffer 数量；
4. 控制编码器缓存；
5. 缩短 GOP；
6. 关闭 B 帧；
7. 降低协议和播放端缓冲。

---

## 13. 当前待确认事项

| 项目                                           | 状态  |
| -------------------------------------------- | --- |
| `media-ctl -p -d /dev/media0` 是否出现 13855     | 待确认 |
| `media-ctl -p -d /dev/media1` 是否有完整 RKISP 链路 | 待确认 |
| `/dev/video11~17` 哪个是最终输出节点                  | 待确认 |
| 输出格式是否支持 NV12                                | 待确认 |
| 是否需要手动启动 RKAIQ                               | 待确认 |
| 是否能稳定输出 720p30                               | 待确认 |
| 是否能稳定接入 MPP                                  | 待确认 |

---

## 14. 当前结论

1. 13855 摄像头应作为 VisionCast 正式视频入口；
2. 13855 输出的是 RAW Bayer，不是 H.264/NV12；
3. RKCIF 负责接收 MIPI CSI-2；
4. RKISP 负责将 RAW 转为正常图像；
5. RKAIQ 负责 AE/AWB/AF 等 3A 控制；
6. `/dev/video18~19` 是 statistics，不是普通画面节点；
7. 应从 `/dev/video11~17` 中确认最终 mainpath 输出；
8. 最终目标是 `13855 → RKISP → NV12 → MPP H.264 → RTSP/SRT/WebRTC`。
