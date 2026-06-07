# ELF-RK3588 Media Graph 记录 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 状态：MIPI 摄像头与 RKISP 数据拓扑已全部确认。

---

## 1. 文档目的

Media Controller 是 Linux 摄像头子系统中描述复杂硬件拓扑的机制。本文档记录板载 13855 MIPI 摄像头从物理 Sensor 经过 DPHY、CIF 接收段、ISP 图像处理段，最终输出到 V4L2 采集节点的完整拓扑关系。

---

## 2. 当前 media 控制设备

系统枚举出的核心 Media 控制器如下：

```text
/dev/media0    # rkcif-mipi-lvds (负责 MIPI 物理接收拓扑)
/dev/media1    # rkisp_mainpath (负责 ISP 图像处理拓扑)
/dev/media2    # USB C270 (负责 USB 备用视频拓扑)
```

---

## 3. RKCIF 接收端拓扑

`/dev/media0` 描述了 13855 MIPI CSI 物理控制接收关系。主要实体与链路状态如下：

- **Sensor 实体**：`ov13855` (13855 Sensor)。
- **物理接口**：MIPI CSI-2 DPHY。
- **物理通路**：`ov13855` [pad 0] ──> `rockchip-mipi-dphy-rx` [pad 0]，状态为 **Enabled**。
- **接收通路**：`rockchip-mipi-dphy-rx` [pad 1] ──> `rkcif-mipi-lvds` [pad 0]，状态为 **Enabled**。
- **数据格式**：RAW 10-bit Bayer 数据包传输。

---

## 4. RKISP 图像处理端拓扑

`/dev/media1` 描述了 ISP 与应用层 V4L2 输出节点的连接关系。

- **输入绑定**：`rkcif-mipi-lvds` 输出经过内部总线输入到 `rkisp-vir0` 核心 ISP 模块。
- **3A 服务连接**：`rkisp-vir0` 图像处理的统计信息（statistics）输出到统计节点 `/dev/video18` 和 `/dev/video19`，由后台自动运行的 `rkaiq_3A_server` 实时读取；经过算法计算出的 ISP 寄存器参数（params）回写至核心 ISP 进行画质实时调节。
- **输出节点映射**：经过去马赛克、白平衡和色彩空间转换后的最终 NV12 多平面数据，由核心 ISP 输出至 `rkisp_mainpath` 实体，映射至 V4L2 采集节点 **`/dev/video11`**。

---

## 5. USB C270 拓扑

`/dev/media2` 描述了 UVC 备用视频设备的连接关系。

- **物理接口**：USB 2.0 Bus 传输。
- **主视频流实体**：`uvcvideo`，映射至 V4L2 节点 **`/dev/video21`**，格式为 MJPEG。
- **Metadata 实体**：映射至 `/dev/video22`，仅输出控制元数据，应用层采集时直接忽略。

---

## 6. 音视频硬件数据链路确认状态表

| 探测项目 | 物理节点/拓扑连接 | 确认状态 | 说明 |
| --- | --- | --- | --- |
| `media0` 中是否存在 13855 | `ov13855` 实体已绑定 | **已确认** | 链路已启用 (Enabled) |
| `media1` 中 RKISP 链路是否完整 | `rkcif` 绑定至 `rkisp-vir0` | **已确认** | 3A 与参数通道畅通 |
| mainpath 输出节点 | `/dev/video11` | **已确认** | 支持多平面视频数据采集 |
| mainpath 输出格式 | NV12 | **已确认** | 适合 MPP 编码，无多余色彩转换 |
| RGA 硬件加速预处理 | `RgaProcessor` 模块 | **已确认** | 格式与尺寸对齐转换仅耗时 2ms |
| rkaiq 守护进程运行 | `rkaiq_3A_server` 服务已启动 | **已确认** | 提供稳定的 AE/AWB 控制 |
| 摄像头帧率稳定性 | `/dev/v4l-subdev2` 曝光控制锁定 | **已确认** | 物理锁死 30 FPS |

---

## 7. 最终结论

1. 13855 摄像头的完整物理拓扑已经调通，主采集通道已固定为 `/dev/video11`。
2. 系统运行在 13855 + RKISP mainpath → V4L2 → RGA → MPP 的完全硬加速链路，具备极佳的吞吐量与极低延迟表现。
