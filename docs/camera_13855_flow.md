# 13855 摄像头数据流说明 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 摄像头：13855 MIPI 摄像头 (主) / USB C270 (备)
> 目标：第一视角视频采集输入，经 ISP 优化、RGA 加速与 MPP H.264/H.265 硬件编码进入推流管线

---

## 1. 文档目的

本文档说明 13855 摄像头在 ELF-RK3588 上的完整已实现数据流，包括：
1. Sensor 原始图像数据流；
2. Sensor 控制流 (I2C/V4L2 Control)；
3. MIPI CSI-2 接收流；
4. RKCIF 数据流；
5. RKISP 图像处理流；
6. RKAIQ 3A 控制与固定帧率锁定；
7. V4L2 输出流 (`/dev/video11`)；
8. RGA/MPP 编码推流数据流。

---

## 2. 总体链路

13855 摄像头输出 RAW Bayer 原始图像数据，由 RK3588 的 RKCIF 和 RKISP 接收并进行 3A 去马赛克处理，最终经 V4L2 暴露 NV12 格式给应用层。

```text
[13855 Sensor]
     │ (RAW Bayer over MIPI CSI-2)
     ▼
[CSI-2 DPHY]
     │
     ▼
[RKCIF]
     │
     ▼
[RKISP] <───> [rkaiq_3A_server] (3A 控制闭环)
     │
     ▼
[rkisp_mainpath] (/dev/video11, MPLANE NV12 30FPS)
     │
     ▼
[V4L2 EXPBUF DMA-BUF]
     │
     ▼
[RgaProcessor] (目标尺寸 NV12 直通；必要时 RGA fd-to-fd)
     │
     ▼
[MppEncoder] (H.264/H.265 硬编码，EXT_DMA 输入)
     │
     ├──> [DisplayRenderer] (编码成功后移动同帧 VideoFrame；enable_preview=true)
     │
     ▼
[AvTransport] (RTP / RTMP / WebRTC / RTSP)
```

---

## 3. Sensor 图像原始数据流与控制流

- **原始格式**：13855 Sensor 通过 MIPI CSI-2 向 RK3588 输出 RAW 10-bit Bayer 原始图像数据。
- **控制链路**：CPU 的 Kernel Sensor 驱动与 13855 寄存器之间通过 I2C 接口进行控制通信，包含读取 Sensor ID、开启/关闭视频流 (Stream On/Off) 等操作。

---

## 4. MIPI CSI-2 传输与 RKCIF 接收

- **MIPI 传输**：13855 基于多 Lane MIPI CSI-2 高速物理层发送图像包，包括虚拟通道 (Virtual Channel) 标识与行/帧同步控制字。
- **RKCIF 接收**：内核 `rkcif-mipi-lvds` 驱动接收物理层包，解析数据后送往 RKISP。该段不直接向应用层输出。

---

## 5. RKISP 图像处理与 RKAIQ 3A 闭环

- **RKISP 处理**：RKISP 对 RAW 图像执行黑电平校正、去马赛克 (Demosaicing)、镜头阴影校正、自动白平衡/曝光以及降噪锐化，最终输出正常的 YUV/NV12 格式画面。
- **RKAIQ 3A 服务**：板端后台运行 `rkaiq_3A_server` 服务，监听 statistics 节点（如 `/dev/video18/19`）并计算 ISP 参数，写入 params 节点控制 ISP。
- **固定帧率与防频闪控制 (Fixed-Rate / Anti-Flicker Control)**：
  在室内或弱光环境下，3A 算法默认会自动拉长 vertical blanking（垂直消隐时间）降低帧率到 15 FPS。系统通过在 `VideoCapture` 中调用 `configure_sensor_frame_rate` 对子设备 `/dev/v4l-subdev2` 发送 `V4L2_CID_EXPOSURE`、`V4L2_CID_VBLANK` 和 `V4L2_CID_ANALOGUE_GAIN` 控制项，锁定曝光、消隐和增益参数，从而保证物理输出稳定在 **30 FPS**。
- **13855 当前调参**：默认 `sensor_exposure=1928`、`sensor_vblank=78`、`sensor_analogue_gain=1536`，用于贴近 50Hz 灯光的 20ms 积分窗口，减少滚动快门横向明暗带；同时通过 `crop=0,380,4224x2376` 固定 16:9 输入区域，避免 ISP 沿用不确定的上一次 crop 状态。

---

## 6. V4L2 采集与节点输出

- **采集节点**：已确认 13855 摄像头主通道采集节点为 `/dev/video11` (rkisp_mainpath)。
- **视频能力**：支持 `V4L2_CAP_VIDEO_CAPTURE_MPLANE` (多平面视频采集能力)。
- **输出格式**：输出 1280x720 30FPS 的 NV12 格式帧。
- **采集模式**：V4L2 多平面 MMAP 缓冲出队，使用 `VIDIOC_EXPBUF` 导出 DMA-BUF fd，并用 `CLOCK_MONOTONIC` 单调时钟对出队瞬间打上采集时间戳。
- **生命周期**：DMA-BUF 帧通过 `VideoFrame::dma.release` 归还 V4L2 buffer，避免硬件处理未结束时过早 `VIDIOC_QBUF`。

---

## 7. RGA 转换与 MPP 硬件编码

- **RGA 预处理**：13855 当前输出目标尺寸 NV12，主路径不调用 RGA，直接 `BYPASS-DMA`。只有尺寸或格式不匹配时才通过 RGA fd-to-fd 处理。
- **USB MJPEG 备用路径**：C270 输入先由 MPP MJPEG 解码为 NV12 DMA-BUF；尺寸匹配则直通，尺寸不匹配则进入 RGA fd-to-fd。详见 `docs/mjpeg_decode_flow.md`。
- **MPP 编码**：`MppEncoder` 优先通过 `MPP_BUFFER_TYPE_EXT_DMA` 导入单平面 NV12 DMA-BUF，按 `encoder.video_codec` 执行 H.264/H.265 CBR 编码，完全禁用 B 帧以消除延迟，输出 Annex-B 裸码流。

---

## 8. 已验证的结论与指标

1. **主采集节点**：确定为 `/dev/video11`。
2. **分辨率与帧率**：稳定在 `1280x720 @ 30 FPS`。
3. **帧率防抖**：锁定了 vertical blanking 和 exposure，强弱光下物理帧率维持在 30 FPS。
4. **备份机制**：若 `/dev/video11` MIPI 节点打开失败，程序能自动切换到备用 `/dev/video21` (USB C270)，使用 MJPEG + MPP MJPEG 硬解输出 NV12 DMA-BUF，必要时由 RGA fd-to-fd 处理，保障采集管线不中断。
5. **零拷贝边界**：13855 到 MPP 编码输入主路径为 DMA-BUF；开启预览时仅额外持有同帧 DMA-BUF 句柄并由 RGA fd 输入渲染，不复制整帧 NV12；编码后 packet、显示输出和 RTP/RTMP/WebRTC/RTSP 封包仍为 CPU 可见数据。
