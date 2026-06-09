# ELF-RK3588 13855 摄像头适配记录

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 摄像头：13855 MIPI 摄像头 (主)
> 状态：摄像头硬件适配与固定帧率锁定均已成功调通。

---

## 1. 摄像头物理链路

13855 摄像头是本项目的正式图像输入源，用于模拟智能眼镜第一视角采集。

与传统的 USB UVC 摄像头不同，13855 摄像头接入了 ELF-RK3588 的 MIPI CSI 物理总线接口。其完整的数据通路设计如下：

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
[RKISP] <───> [rkaiq_3A_server] (3A 参数调节)
     │
     ▼
[rkisp_mainpath] (/dev/video11)
     │
     ▼
[VideoCapture] (V4L2 多平面采集) ──> [MPP 硬编码]
```

---

## 2. 内核及设备树识别状态

经板端探测确认，13855 摄像头相关内核驱动与拓扑设备已正常加载：
- **Sensor 驱动**：内核已加载 `ov13855` 驱动。
- **I2C 控制总线**：Sensor 芯片挂载于对应 I2C 物理通道下，I2C 寻址与探测 (probe) 状态为 `OK`。
- **采集通道映射**：`/dev/media1` 中映射出的 `rkisp_mainpath` 采集主节点为 **`/dev/video11`**。

---

## 3. 核心功能适配与画质优化

### 3.1 曝光与垂直消隐控制 (锁帧率 30 FPS)
- **挑战**：在默认状态下，随着环境光线变暗，RKISP 后台自动曝光 (AE) 算法会大幅度增加曝光时间并拉长垂直消隐时间 (vertical blanking, vblank)，导致画面物理帧率从 30 FPS 折半滑落至 15 FPS 左右，产生明显卡顿。
- **解决方案**：在 [video_capture.cpp](file:///home/elf/open_project/VisionCast/src/media/video_capture.cpp) 中新增了 `configure_sensor_frame_rate` 控制接口。在采集器启动时，直接打开 `/dev/v4l-subdev2` 对应的 Sensor 子设备文件，通过 `ioctl` 写入固定的曝光限制 (`V4L2_CID_EXPOSURE`) 和垂直消隐上限 (`V4L2_CID_VBLANK`)。将传感器物理帧率锁死在稳定的 **30 FPS**，强光/弱光环境切换均不卡顿。

### 3.2 色彩空间与图像格式NV12
- **输出格式**：`/dev/video11` 最终以 MPLANE 多平面模式提供 NV12 格式帧，完美对齐 Rockchip MPP 硬件编码器的输入格式，消除了 CPU 参与任何格式转换的性能损耗。

---

## 4. 故障退避降级方案 (Backup Plan)

为了应对因硬件松动、排线断开或物理摄像头损坏等突发异常，软件层面实现了无缝的摄像头退避方案：
- 当 MIPI `/dev/video11` 节点打开报错或出帧超时，`VideoCapture` 模块会瞬间捕获该异常，并尝试自动初始化备选节点 `/dev/video21` (USB C270 摄像头)。
- 降级后，系统以 MJPEG 格式抓取帧，通过硬解码转换为 NV12 后送入后续 RGA 处理及 MPP 编码管道，确保推流程序在 MIPI 硬件脱落时依然保持高可用。

---

## 5. 结论

1. 13855 摄像头的硬件链路、曝光锁帧控制已全面走通，在 `/dev/video11` 稳定输出 720p30 NV12 图像。
2. 设备树与 kernel 配置参数已固化于板载系统中，无需人工重新配置即可顺畅开机运行推流。
