# 13855 摄像头数据流说明 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 摄像头：13855 MIPI 摄像头 (主) / USB C270 (备)
> 目标：第一视角视频采集输入，经 ISP 优化、RGA 加速与 MPP H.264 硬件编码进入推流管线

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
     ├─────────────────────────────────────────┐
     ▼ (推流管线)                                ▼ (本地显示管线)
[RgaProcessor] (im2d 硬件色彩/大小转换)     [DisplayRenderer] (异步线程)
     │                                         │
     ▼                                         ▼
[MppEncoder] (H.264 Baseline 硬编码)        [OpenCV HighGUI] (X11 窗口渲染)
     │
     ▼
[AvTransport] (多协议推送抽象层)
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
- **固定帧率控制 (Fixed-Rate Control)**：
  在室内或弱光环境下，3A 算法默认会自动拉长 vertical blanking（垂直消隐时间）降低帧率到 15 FPS。系统通过在 `VideoCapture` 中调用 `configure_sensor_frame_rate` 对子设备 `/dev/v4l-subdev2` 发送 `V4L2_CID_EXPOSURE` 和 `V4L2_CID_VBLANK` 控制项，锁定曝光与消隐参数，从而保证了物理输出稳定在 **30 FPS**，不受环境光强弱影响。

---

## 6. V4L2 采集与节点输出

- **采集节点**：已确认 13855 摄像头主通道采集节点为 `/dev/video11` (rkisp_mainpath)。
- **视频能力**：支持 `V4L2_CAP_VIDEO_CAPTURE_MPLANE` (多平面视频采集能力)。
- **输出格式**：输出 1280x720 30FPS 的 NV12 格式帧。
- **采集模式**：V4L2 多平面 MMAP 缓冲出队，使用 `CLOCK_MONOTONIC` 单调时钟对出队瞬间打上采集时间戳。

---

## 7. RGA 转换与 MPP 硬件编码

- **RGA 预处理**：`RgaProcessor` 封装了 `im2d_api` 硬件加速接口。即使 `/dev/video11` 采集的不是标准 NV12 格式（或备用 USB 摄像头输入 YUYV），RGA 均能在 2ms 内将其高效缩放并色彩空间转换为标准 NV12，提供给 MPP。
- **MPP 编码**：`MppEncoder` 接收 NV12 帧，执行 H.264 Baseline CBR 编码，完全禁用 B 帧以消除延迟，输出 Annex-B 裸 H.264 码流。

---

## 8. 已验证的结论与指标

1. **主采集节点**：确定为 `/dev/video11`。
2. **分辨率与帧率**：稳定在 `1280x720 @ 30 FPS`。
3. **帧率防抖**：锁定了 vertical blanking 和 exposure，强弱光下物理帧率维持在 30 FPS。
4. **备份机制**：若 `/dev/video11` MIPI 节点打开失败，程序能自动切换到备用 `/dev/video21` (USB C270)，使用 MJPEG + MPP MJPEG 硬解或 libjpeg-turbo 软解，再由 RGA 转为 NV12，保障采集管线不中断。
