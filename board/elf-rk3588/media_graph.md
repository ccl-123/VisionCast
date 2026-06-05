# ELF-RK3588 Media Graph 记录

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 文档作用：记录 RKCIF、RKISP、13855、mainpath、statistics 等 media controller 拓扑关系。

---

## 1. 文档目的

Media Controller 是 Linux 摄像头子系统中用于描述复杂摄像头拓扑的机制。对于 13855 MIPI 摄像头，不能只看 `/dev/videoX`，还必须确认 Sensor、CSI-2 DPHY、RKCIF、RKISP、mainpath 之间的链路。

本文档记录：

1. `/dev/media0` 的 RKCIF 拓扑；
2. `/dev/media1` 的 RKISP 拓扑；
3. 13855 是否进入 media graph；
4. RKISP mainpath 对应哪个 video node；
5. statistics 节点和 params 节点用途；
6. 后续 V4L2 采集节点判断依据。

---

## 2. 当前 media 设备

当前已枚举：

```text
/dev/media0    # rkcif-mipi-lvds
/dev/media1    # rkisp_mainpath
/dev/media2    # USB C270
```

当前判断：

| Media 设备      | 作用                |
| ------------- | ----------------- |
| `/dev/media0` | RKCIF / MIPI 前端拓扑 |
| `/dev/media1` | RKISP 图像处理拓扑      |
| `/dev/media2` | USB C270 拓扑       |

---

## 3. RKCIF 拓扑查询

执行：

```bash
media-ctl -p -d /dev/media0
```

需要重点查看：

1. 是否存在 `13855` 或 `ov13855`；
2. 是否存在 CSI-2 DPHY；
3. Sensor 是否 link 到 CSI-2；
4. CSI-2 是否 link 到 RKCIF；
5. format 是否为 RAW10 / RAW12；
6. 分辨率是否正确；
7. link 是否 enabled。

原始输出记录区：

```text
TODO: 将 media-ctl -p -d /dev/media0 的完整输出粘贴 to 这里。
```

---

## 4. RKISP 拓扑查询

执行：

```bash
media-ctl -p -d /dev/media1
```

需要重点查看：

1. RKISP 输入实体；
2. mainpath 输出实体；
3. selfpath 输出实体；
4. statistics 实体；
5. params 实体；
6. 当前 format；
7. 当前 resolution；
8. mainpath 对应的 `/dev/videoX`。

原始输出记录区：

```text
TODO: 将 media-ctl -p -d /dev/media1 的完整输出粘贴 to 这里。
```

---

## 5. USB C270 拓扑查询

执行：

```bash
media-ctl -p -d /dev/media2
```

用途：

1. 确认 USB C270 的 video entity；
2. 区分 `/dev/video21` 和 `/dev/video22`；
3. 判断哪个节点是视频流，哪个可能是 metadata 或辅助节点。

原始输出记录区：

```text
TODO: 将 media-ctl -p -d /dev/media2 的完整输出粘贴 to 这里。
```

---

## 6. 当前 video 节点分类

| 节点                | 来源       | 类型          | 用途         |
| ----------------- | -------- | ----------- | ---------- |
| `/dev/video0~10`  | RKCIF    | MIPI 输入侧    | 辅助排查       |
| `/dev/video11~17` | RKISP    | mainpath 候选 | 正式采集候选     |
| `/dev/video18~19` | RKISP    | statistics  | 3A 统计，不是画面 |
| `/dev/video20`    | HDMI RX  | HDMI 输入     | 非首版目标      |
| `/dev/video21~22` | USB C270 | UVC         | 调试备用       |

---

## 7. mainpath 节点确认方法

逐个查询：

```bash
for i in {11..17}; do
    echo "===== /dev/video$i ====="
    v4l2-ctl -d /dev/video$i -D
    v4l2-ctl -d /dev/video$i --list-formats-ext
done
```

重点看：

1. Driver Name；
2. Card Type；
3. Bus Info；
4. Capabilities；
5. Pixel Format；
6. Resolution；
7. FPS；
8. 是否支持 Streaming；
9. 是否支持 NV12 / YUYV / NV16。

---

## 8. mainpath 采集测试

确认格式后可测试：

```bash
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=1280,height=720,pixelformat=NV12 \
  --stream-mmap=4 \
  --stream-count=100 \
  --stream-to=/tmp/13855_nv12.yuv
```

如果不支持 NV12，可尝试：

```bash
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=1280,height=720,pixelformat=YUYV \
  --stream-mmap=4 \
  --stream-count=100 \
  --stream-to=/tmp/13855_yuyv.yuv
```

---

## 9. statistics 节点说明

`/dev/video18` 和 `/dev/video19` 属于 RKISP statistics 节点。

用途：

1. AE 自动曝光统计；
2. AWB 自动白平衡统计；
3. AF 自动对焦统计；
4. Histogram；
5. 亮度统计；
6. 色彩统计。

注意：

```text
statistics 节点不是普通图像节点，不能作为推流输入。
```

---

## 10. params 节点说明

RKAIQ 通过 params 节点向 RKISP 写入图像处理参数。

用途：

1. 曝光参数；
2. 白平衡参数；
3. 黑电平；
4. 色彩校正；
5. 降噪；
6. 锐化；
7. Gamma。

VisionCast 首版可以先关注图像能否输出，后续再确认 RKAIQ 是否正常参与 3A 控制。

---

## 11. 目标 media 链路

最终目标链路：

```text
13855 Sensor
    → CSI-2 DPHY
    → RKCIF
    → RKISP
    → mainpath
    → /dev/videoX
    → V4L2 Capture
    → MPP Encoder
```

优化链路：

```text
13855 Sensor
    → RKISP mainpath
    → V4L2 DMA-BUF
    → MPP Import FD
    → H.264/H.265
    → RTSP/SRT/WebRTC
```

---

## 12. 当前待确认表

| 项目                           | 状态   |
| ---------------------------- | ---- |
| `/dev/media0` 是否存在 13855     | TODO |
| `/dev/media1` 是否有完整 RKISP 链路 | TODO |
| mainpath 对应哪个 `/dev/videoX`  | TODO |
| mainpath 输出格式                | TODO |
| 是否支持 NV12                    | TODO |
| 是否需要 RGA 转换                  | TODO |
| RKAIQ 是否正常启动                 | TODO |
| 是否能稳定 720p30                 | TODO |
| 是否能稳定 1080p30                | TODO |

---

## 13. 当前结论

1. 当前系统已枚举 RKCIF 和 RKISP；
2. 13855 正式采集节点还需要通过 media graph 确认；
3. `/dev/video11~17` 是当前最关键的排查范围；
4. `/dev/video18~19` 是 statistics，不是画面输出；
5. media graph 结果应在本文件中长期保存，作为代码开发依据。
