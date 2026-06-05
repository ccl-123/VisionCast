
# VisionCast 性能测试文档

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 目标：记录音视频采集、处理、编码、推流的性能指标和测试方法

---

## 1. 文档目的

本文档用于定义 VisionCast 的性能测试指标、测试场景、日志格式和验收标准。

测试重点包括：

1. 视频采集帧率；
2. 音频采集稳定性；
3. RGA 处理耗时；
4. MPP 编码耗时；
5. 网络发送耗时；
6. 端到端延迟；
7. CPU 占用；
8. 内存占用；
9. 温度；
10. 队列积压情况。

---

## 2. 测试平台

| 项目       | 内容                   |
| -------- | -------------------- |
| 开发板      | ELF-RK3588           |
| 系统       | Ubuntu 22.04 aarch64 |
| 正式摄像头    | 13855 MIPI 摄像头       |
| 调试摄像头    | USB C270             |
| 音频 Codec | NAU8822              |
| 音频设备     | `hw:1,0`             |
| 视频编码     | Rockchip MPP         |
| 图像处理     | RGA                  |
| 首版协议     | RTSP/RTP             |
| 后续协议     | SRT / WebRTC         |

---

## 3. 测试指标

| 指标           | 说明                  |
| ------------ | ------------------- |
| Capture FPS  | V4L2 实际采集帧率         |
| Encode FPS   | MPP 实际编码帧率          |
| Push FPS     | 网络发送帧率              |
| RGA Cost     | 图像转换/缩放耗时           |
| MPP Cost     | 单帧编码耗时              |
| Packet Cost  | RTP/SRT/WebRTC 封包耗时 |
| Send Cost    | 网络发送耗时              |
| E2E Latency  | 端到端显示延迟             |
| CPU Usage    | 进程和系统 CPU 占用        |
| Memory Usage | 进程 RSS 内存           |
| Bitrate      | 实际输出码率              |
| Temperature  | SoC 温度              |
| Queue Length | 各阶段队列长度             |
| Drop Frames  | 丢帧数量                |

---

## 4. 测试场景

### 4.1 场景一：USB C270 调试链路

```text
USB C270
    → V4L2
    → RGA
    → MPP H.264
    → RTSP/RTP
```

目的：

1. 快速验证 V4L2；
2. 快速验证 MPP；
3. 快速验证 RTSP/RTP；
4. 在 13855 未完全适配时继续推进软件管线。

---

### 4.2 场景二：13855 正式链路

```text
13855
    → RKCIF
    → RKISP
    → V4L2 mainpath
    → MPP H.264
    → RTSP/RTP
```

目的：

1. 验证正式摄像头输入；
2. 验证 RKISP 输出；
3. 验证 mainpath 输出格式；
4. 验证 MPP 编码兼容性。

---

### 4.3 场景三：RGA 转换链路

```text
V4L2 YUYV / NV16
    → RGA
    → NV12
    → MPP H.264
```

目的：

1. 测试 RGA 转换耗时；
2. 验证非 NV12 输入时的处理路径；
3. 评估 CPU 占用变化。

---

### 4.4 场景四：DMA-BUF 优化链路

```text
V4L2 DMA-BUF
    → RGA DMA-BUF
    → MPP DMA-BUF
    → RTSP/RTP
```

目的：

1. 减少 memcpy；
2. 降低内存带宽；
3. 降低端到端延迟；
4. 提高帧率稳定性。

---

### 4.5 场景五：SRT 公网链路

```text
MPP H.264/H.265
    → SRT
    → Media Server
```

目的：

1. 测试公网推流；
2. 测试弱网恢复；
3. 测试远程观看稳定性。

---

### 4.6 场景六：WebRTC 最终链路

```text
MPP H.264 + Opus
    → WebRTC WHIP
    → Browser
```

目的：

1. 测试浏览器低延迟播放；
2. 测试远程协作；
3. 测试 NAT 环境下连接；
4. 为智能眼镜最终产品形态验证协议路线。

---

## 5. 推荐日志格式

统一日志格式：

```text
[Module] key=value key=value key=value
```

示例：

```text
[VideoCapture] fps=30.0 pts=123456789 fmt=NV12 size=1280x720
[RGA] cost_ms=2.1 input=YUYV output=NV12
[MPP] cost_ms=5.6 size=42130 type=P
[RTP] packet_count=32 cost_ms=0.3
[Network] send_cost_ms=0.5 bitrate_kbps=4012
[AudioCapture] frame_ms=20 sample_rate=48000 channels=2
[System] cpu=31.2 mem=86MB temp=61.5
[Pipeline] e2e_latency_ms=120 queue_raw=1 queue_enc=0
```

---

## 6. 分段耗时统计

建议每帧记录以下时间点：

| 时间点               | 含义        |
| ----------------- | --------- |
| `t_capture`       | V4L2 出帧时间 |
| `t_process_start` | RGA 开始    |
| `t_process_end`   | RGA 结束    |
| `t_encode_start`  | MPP 编码开始  |
| `t_encode_end`    | MPP 编码结束  |
| `t_packet_start`  | 封包开始      |
| `t_packet_end`    | 封包结束      |
| `t_send`          | 网络发送完成    |

计算：

```text
RGA Cost = t_process_end - t_process_start
MPP Cost = t_encode_end - t_encode_start
Packet Cost = t_packet_end - t_packet_start
Pipeline Cost = t_send - t_capture
```

---

## 7. 端到端延迟测量

推荐方法一：摄像头拍秒表

1. 摄像头拍摄毫秒级秒表；
2. 接收端播放画面；
3. 使用手机慢动作录像；
4. 对比真实秒表和播放端画面；
5. 计算端到端显示延迟。

推荐方法二：LED 闪烁测试

1. 使用 GPIO 控制 LED 闪烁；
2. 摄像头拍摄 LED；
3. 接收端显示；
4. 用高速摄像记录物理 LED 和屏幕显示；
5. 计算延迟。

推荐方法三：OSD 时间戳

1. 在视频帧上叠加采集时间戳；
2. 接收端读取显示时间；
3. 要求采集端和接收端时间同步；
4. 更适合自动化测试。

---

## 8. 队列延迟控制

实时推流不能无限缓存。

推荐队列长度：

| 队列                    | 建议长度    |
| --------------------- | ------- |
| Raw Video Queue       | 2 ~ 3 帧 |
| Processed Video Queue | 2 ~ 3 帧 |
| Encoded Video Queue   | 2 ~ 3 包 |
| Audio Queue           | 2 ~ 5 包 |
| Network Queue         | 尽量短     |

队列满时策略：

```text
丢弃旧帧，保留最新帧。
```

不建议策略：

```text
无限等待
无限缓存
为了不丢帧而持续增加延迟
```

低延迟场景下，宁可丢帧，也不要延迟不断堆积。

---

## 9. CPU / 内存 / 温度测试

### 9.1 CPU

```bash
top
htop
pidstat -p <pid> 1
```

### 9.2 内存

```bash
free -h
cat /proc/<pid>/status | grep -E "VmRSS|VmSize"
```

### 9.3 温度

不同系统路径可能不同，可先查询：

```bash
find /sys/class/thermal -name temp -exec sh -c 'echo "$1: $(cat $1)"' _ {} \;
```

### 9.4 网络码率

```bash
ifstat
nload
sar -n DEV 1
```

---

## 10. 首版验收指标

| 指标     | 首版目标            |
| ------ | --------------- |
| 视频分辨率  | 1280x720        |
| 视频帧率   | 30 FPS          |
| 视频编码   | H.264           |
| 音频采集   | 48kHz S16_LE    |
| 推流协议   | RTSP/RTP        |
| 播放端    | VLC / ffplay    |
| 稳定性    | 连续运行 30 分钟      |
| 端到端延迟  | 500 ms 以内作为首版目标 |
| CPU 占用 | 尽量低于 50%        |
| 崩溃     | 无崩溃、无死锁         |

---

## 11. 优化版目标

| 指标     | 优化目标            |
| ------ | --------------- |
| 端到端延迟  | 200 ms ~ 300 ms |
| 视频输入   | 13855 mainpath  |
| Buffer | DMA-BUF         |
| 编码     | MPP low latency |
| 协议     | SRT / WebRTC    |
| 音频编码   | Opus            |
| 队列     | 可控、可统计          |
| 日志     | 可定位瓶颈           |

---

## 12. 测试记录模板

每次测试建议记录：

```text
测试日期：
测试版本：
视频输入：
音频输入：
分辨率：
帧率：
编码格式：
码率：
协议：
播放端：
运行时长：

Capture FPS：
Encode FPS：
Push FPS：
RGA Cost：
MPP Cost：
End-to-End Latency：
CPU：
Memory：
Temperature：
异常现象：
结论：
```

---

## 13. 当前结论

VisionCast 性能优化应分阶段推进：

1. 先跑通采集、编码、推流；
2. 再建立完整日志；
3. 再定位瓶颈；
4. 再引入 RGA / DMA-BUF；
5. 再优化 MPP 参数；
6. 最后切换到 SRT / WebRTC 等先进协议。

首版不要过度追求零拷贝，先保证系统稳定闭环。
