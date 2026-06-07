# VisionCast 开发问题记录

本文记录本轮完成 `最后一个版本开发提示词文档.md` 期间遇到的主要问题、原因、处理方式和剩余验证边界，便于后续板端调试和交付追踪。

## 1. C270 采集格式与节点配置错误

- 现象：USB C270 被配置为 `/dev/video21` 的 `YUYV`，但设备实况要求 1280x720@30 使用 `MJPEG`；`/dev/video22` 是 metadata 节点，不能作为采集节点。
- 原因：旧配置没有区分 UVC 视频节点和 metadata 节点，也没有按设备能力选择 MJPEG。
- 处理：将 USB C270 配置改为 `/dev/video21`、`1280x720@30`、`MJPEG`；`VideoCapture` 支持 MJPEG FOURCC，并在后续链路中统一解码/转换为 NV12。

## 2. OV13855 使用 V4L2 多平面采集

- 现象：OV13855 主节点 `/dev/video11` 是 `rkisp_mainpath`，能力为 `V4L2_CAP_VIDEO_CAPTURE_MPLANE`，旧代码只支持 `V4L2_BUF_TYPE_VIDEO_CAPTURE`。
- 原因：旧采集实现固定使用单平面 V4L2 buffer type，导致多平面节点无法 `S_FMT`、`REQBUFS`、`DQBUF`。
- 处理：`VideoCapture` 根据设备能力选择单平面或多平面 MMAP 流程，并在出队瞬间使用 `CLOCK_MONOTONIC` 记录 PTS。

## 3. MJPEG 解码路径缺失

- 现象：C270 的 MJPEG 压缩帧不能直接送入 RGA/MPP H.264 编码器。
- 原因：旧管线只处理原始 YUYV/NV12，没有 MJPEG 到 NV12 的转换步骤。
- 处理：增加 `MjpegDecoder`。RK3588 构建优先使用 MPP MJPEG 硬解，主机/降级路径使用 libjpeg-turbo 软件解码，再进入 RGA/NV12 统一预处理。

## 4. 项目内第三方依赖版本不一致

- 现象：交叉构建切到项目内 `3rdparty` 后，MPP/RGA 头文件与已验证库版本不一致，曾出现 MPP 头文件缺失和 API 类型不匹配。
- 原因：参考工程中的精简头文件与 VisionCast 链接的 RK3588 运行库不是同一套版本。
- 处理：用已验证通过的 Rockchip SDK 头文件和 aarch64 库覆盖项目内 `3rdparty/mpp`、`3rdparty/rga`，并在 `3rdparty/README.md` 记录来源与部署约束。

## 5. RTMP H.264 写入格式不正确

- 现象：RTMP 模块已从首个 H.264 帧提取 SPS/PPS 并生成 AVC extradata，但视频包仍按 MPP 输出的 Annex-B 起始码格式写入 FLV muxer。
- 原因：FLV/RTMP 中的 AVC video packet 应使用 4 字节大端长度前缀加 NAL 的 AVCC sample 格式，不能直接写 Annex-B start code。
- 处理：在 `RtmpPusher` 写视频包前，将 Annex-B NAL 切分并转换为 AVCC 长度前缀负载，再交给 FFmpeg FLV muxer。
- 注意：RTMP header 仍依赖首个视频关键帧包含 SPS/PPS；MPP 已配置 IDR 带头，板端如仍遇到首帧无 SPS/PPS，应检查编码器输出配置和 IDR 请求逻辑。

## 6. FFmpeg RTMP 间接依赖未完整打包

- 现象：主程序通过 `dlopen()` 加载 FFmpeg 直接库，ELF 的直接 `NEEDED` 看不到 FFmpeg；仅打包 `libavformat/libavcodec/libavutil/libswresample` 时，板端可能缺 GnuTLS、XML2、编解码库等间接依赖。
- 原因：FFmpeg 动态库本身有大量 `NEEDED` 依赖，旧安装脚本只复制直接使用的库。
- 处理：`scripts/build/cross_build.sh` 增加递归 `readelf -d` 依赖收集，自动从 aarch64 系统库目录复制非 glibc 基础运行库到 `install/visioncast/lib/`。

## 7. WebRTC WHIP 依赖构建问题

- 现象一：交叉编译 libdatachannel 时 OpenSSL 头文件 `opensslconf.h` 缺失。
- 原因：`libssl-dev:arm64` 的架构相关头文件不在通用 include 目录。
- 处理：`scripts/build/build_webrtc_deps.sh` 下载并解出 `libssl-dev:arm64`，显式加入 `usr/include/aarch64-linux-gnu`。

- 现象二：CMake 3.22 交叉模式找到了 OpenSSL 变量，但未创建 `OpenSSL::SSL` / `OpenSSL::Crypto` imported targets。
- 原因：上游 libdatachannel/libsrtp CMake 依赖 imported target，交叉 FindOpenSSL 结果不完整。
- 处理：构建脚本给 libdatachannel 和 libsrtp 的 CMakeLists 注入 OpenSSL imported target shim。

- 现象三：`libssl.so` 是开发包中的悬空符号链接，链接阶段提示没有规则制作 `libssl.so`。
- 原因：只解出了 `libssl-dev:arm64`，没有解出提供真实 `libssl.so.3` / `libcrypto.so.3` 的 `libssl3:arm64`。
- 处理：脚本同时下载 `libssl3:arm64`，并让 CMake 指向真实 `.so.3` 文件；安装时复制 `libssl.so.3`、`libcrypto.so.3` 到 `3rdparty/webrtc/lib/aarch64/`。

## 8. WebRTC WHIP 实现边界

- 已实现：配置 `protocol=webrtc`、`webrtc_url`，WHIP HTTP POST SDP 交换，libdatachannel PeerConnection，H.264 RTP/PCMA RTP 通过 WebRTC Track 发送，交叉构建启用 `VISIONCAST_ENABLE_WEBRTC=1`。
- 当前限制：WHIP helper 只支持明文 `http://` URL，尚未实现 HTTPS；提示词示例和默认配置均为 `http://192.168.137.1:8889/live/stream`。
- 待板端验证：需要 MediaMTX/SRS 等 WHIP 服务端和真实 RK3588 摄像头/音频设备，才能验证 ICE/DTLS/SRTP 建连、浏览器播放延迟和 30 分钟稳定性。

## 9. 本地/板端硬件及推流接收链路验证已全部走通

- **验证状态**：在 RK3588 实物板卡与虚拟机之间成功完成了流媒体接收和本地渲染的完整全链路验证。
- **协议推流验证**：
  - **RTMP 模式**：通过 SSH 远程端口映射，将板端的 RTMP 推流经由 `127.0.0.1:1935` 成功发送至虚拟机上运行的 MediaMTX 服务端，日志报告 `is publishing to path 'live/stream'`。
  - **WebRTC 模式**：通过 WHIP 信令在 `127.0.0.1:8889` 建立会话，PeerConnection 成功连接，流状态在线，VM 端成功接收到 H.264/G.711 音视频流。

## 10. Ubuntu 桌面环境下本地预览不可见问题 (DisplayRenderer)

- **现象**：在板端启动脚本后，Ubuntu 图形界面桌面上没有任何本地预览画面，直接写入 `/dev/fb0` 没有反应。
- **原因**：当系统运行着 X11/Wayland (GDM/GNOME) 桌面管理器时，桌面的渲染会不断重绘并覆盖直接写入帧缓冲区 `/dev/fb0` 的像素。
- **处理**：在 `DisplayRenderer` 中实现 `X11Window` 后端。使用 `dlopen("libX11.so.6")` 和 `dlsym` 动态加载 X11 API，在 `DISPLAY` 环境变量可用时，自动创建一个 X11 窗口并在其中显示画面，同时集成了 RGA 硬件缩放和 NV12->RGB 格式转换，运行效率高且保持了对非 GUI 终端的兼容（无 X11 时自动退回 `/dev/fb0` Framebuffer 渲染）。

## 11. OV13855 强光/弱光环境帧率下降问题 (Fixed-Rate V4L2 Control)

- **现象**：MIPI 摄像头 `/dev/video11` 默认配置为 30 FPS，但实际运行时一直保持在 15 FPS 左右，调整 `VIDIOC_S_PARM` 返回 Inappropriate ioctl for device 警告。
- **原因**：自动曝光算法控制服务 `rkaiq_3A_server` 在室内较暗环境下会自动调节曝光时间并大幅度拉长 vertical blanking（垂直消隐时间），导致物理输出帧率降到 15 FPS。
- **处理**：在 `VideoCapture` 中新增了 `configure_sensor_frame_rate` 函数。该函数直接打开配置的传感器子设备(`/dev/v4l-subdev2`)，在启动时将 `V4L2_CID_EXPOSURE` 曝光参数和 `V4L2_CID_VBLANK` 垂直消隐锁定在固定值（例如曝光 3000，vblank 78），从而锁定了 sensor 物理帧率在 30 FPS，不受外部环境光线强弱影响。
