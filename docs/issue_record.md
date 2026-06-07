# VisionCast 开发问题与 Bug 解决记录 (已完工版)

本文详细总结了 **VisionCast 智能眼镜音视频实时低延迟推流系统** 联调与开发集成期间遇到的核心问题、产生原因以及最终版本中所采用的已实现解决方案。

---

## 1. 自动曝光导致 MIPI 摄像头帧率折半问题

- **现象**：13855 MIPI 摄像头在光线较弱或室内场景下，实际采集帧率会从 30 FPS 跌至 15 FPS 左右，产生严重的画面拖影。
- **原因**：板载 3A 控制服务 `rkaiq_3A_server` 在检测到暗光环境时，为了增加进光量，会自动调长曝光时间，并自动拉长垂直消隐时间 (vertical blanking, vblank)，导致物理帧输出受限。
- **已实现的解决办法**：在 `VideoCapture` 模块初始化时新增了 `configure_sensor_frame_rate` 控制逻辑。通过直接操作传感器控制子设备 `/dev/v4l-subdev2`，将曝光参数 (`V4L2_CID_EXPOSURE`) 和消隐参数 (`V4L2_CID_VBLANK`) 锁定在稳定输出 30 FPS 的固定值上，从而在强弱光变化时强制锁定 **30 FPS** 帧率。

---

## 2. V4L2 多平面采集 (MPLANE) 兼容问题

- **现象**：主摄像头节点 `/dev/video11` (rkisp_mainpath) 在尝试以单平面 `V4L2_BUF_TYPE_VIDEO_CAPTURE` 启动时，内核返回 invalid argument 报错，无法申请缓冲区及出帧。
- **原因**：Rockchip ISP 驱动所支持的视频采集能力为多平面接口模式 (`V4L2_CAP_VIDEO_CAPTURE_MPLANE` 以及 `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`)。
- **已实现的解决办法**：重构了 `VideoCapture` 底层代码，支持根据设备节点能力自动判定并自适应匹配单平面与多平面采集模式。在 `/dev/video11` 下自动采用 `MPLANE` 形式执行 mmap 映射与 epoll 轮询，保证了 MIPI 摄像头的顺畅采集。

---

## 3. WebRTC WHIP 本地回环 DTLS 握手超时问题

- **现象**：在进行 WebRTC (WHIP) 本地回环测试时，连接始终在 DTLS 握手步骤挂起，10 秒后连接被踢掉超时，浏览器拉流提示 `stream not found`。
- **原因**：默认 libdatachannel 生成的 Offer 中 DTLS 角色设置为 `a=setup:actpass`，这会导致 Pion (MediaMTX) 强行协商服务器作为 active 发起端，但在回环接口路由策略下，服务器主动发送握手包往往受挫丢包；同时，环回接口可能存在多个候选网卡地址冲突。
- **已实现的解决办法**：
  1. 在 `WebrtcPusher` 中，若检测到目标 URL 为 localhost 或 127.0.0.1，强制配置 `config.bindAddress` 绑定至 `127.0.0.1` 环回物理接口。
  2. 在 SDP 交换前，对生成的 Offer 字符串做动态正则替换，将 `a=setup:actpass` 强制重写为 `a=setup:active`。这会强迫 MediaMTX 成为 passive 接收端，迫使 WHIP 客户端主动发出 `ClientHello` 握手。该修改将 DTLS 建连协商降到了 5ms 级。

---

## 4. RTP over UDP 空挂断导致程序崩溃问题

- **现象**：在启动 UDP 协议推流时，如果拉流端 (VLC/ffplay) 还没有上线开启监听，或者拉流端中途关闭，推流主程序会由于网络发送返回 `ECONNREFUSED` (连接被拒) 错误而直接崩溃。
- **原因**：`UdpSender` 内部使用了 `connect()` 绑定了目的套接字。在无监听状态下发送数据，系统协议栈会收到 ICMP 端口不可达消息，导致下一个 `send` 调用抛出 `ECONNREFUSED` 致命错误。
- **已实现的解决办法**：在 `UdpSender::send` 发送逻辑中拦截了 `ECONNREFUSED` 报错，记录警告日志但允许其安全返回 `true`。避免了无连接协议因为拉流端离线而拖垮推流主管线，实现了接收端的无缝热插拔。

---

## 5. RTMP 画面写入 FLV Muxer 格式错误

- **现象**：RTMP 推流日志正常，但是拉流端渲染黑屏或提示格式解析失败。
- **原因**：MPP 硬编码输出的是 Annex-B 字节流（包含 3 字节或 4 字节的 0x000001 / 0x00000001 起始码），但是 FLV/RTMP 封装要求使用 AVCC 格式（即 4 字节大端 NAL 长度前缀），直接写入起始码会导致播放端反初始化解码器失败。
- **已实现的解决办法**：在 `RtmpPusher` 的发送逻辑中，实现了一个 Annex-B 到 AVCC 的转换器。拦截 MPP 的输出帧，剥离起始码并提取为 SPS/PPS 组成 AVC decoder configuration record 首帧发送，后续帧均重组为长度前缀格式写入 FFmpeg 容器。

---

## 6. Ubuntu 桌面环境下本地预览不可见问题 (DisplayRenderer)

- **现象**：当 ELF-RK3588 上开启了图形桌面环境时，程序向 `/dev/fb0` (Framebuffer) 写入的像素直接被 X11/GNOME 窗口管理器的桌面覆盖，无法显示本地画面。
- **原因**：桌面窗口系统的合成管理器 (Compositor) 独占了屏幕绘制权限，导致直接写显存被瞬间覆盖。
- **已实现的解决办法**：在 `DisplayRenderer` 中实现了自适应的渲染后端。程序优先检测 X11 环境变量。在有桌面环境下，自动动态 dlopen `libX11.so.6` 并在一个独立的异步 X11 窗口中利用 RGA 加速渲染视频流；当在纯命令行环境（无 X11 图形界面）下，则自动退避到原有的 `/dev/fb0` Framebuffer 渲染逻辑中。
