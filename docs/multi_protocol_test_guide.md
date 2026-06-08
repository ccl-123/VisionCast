# VisionCast 多协议推拉流测试指南

本指南详细介绍了如何在 RK3588 开发板端及同局域网跨设备上，对 VisionCast 系统的三大流媒体传输协议（WebRTC WHIP/WHEP、RTMP、RTP over UDP）进行闭环推拉流测试。本教程将作为后续开发和系统集成测试的重要参考。

---

## 1. 测试前准备工作

### 1.1 部署运行环境
在开始任何推流测试前，需要确保相关的运行库已就位：
- 系统安装包已就位在 `install/visioncast/`。
- 本地启动测试需配置 `LD_LIBRARY_PATH` 环境变量，以正确加载安装包目录下的 RGA、MPP、OpenSSL 以及 libdatachannel 等共享库。
```bash
export LD_LIBRARY_PATH=/home/elf/open_project/VisionCast/install/visioncast/lib
```

### 1.2 设备硬件状态检查
确保摄像头及麦克风硬件工作正常，且被独占使用：
- **视频源**：通常为板载 MIPI 摄像头（如 `/dev/video11`，格式为 NV12）或 USB 摄像头。
- **音频源**：ALSA 采集卡号（通常为 `hw:1,0`）。
可以使用随附的探测工具检测设备：
```bash
./install/visioncast/scripts/board/probe_video.sh
./install/visioncast/scripts/board/probe_audio.sh
```

### 1.3 启动 MediaMTX 媒体服务器
对于 **WebRTC (WHIP)** 和 **RTMP**，需要运行 MediaMTX 服务端来分发流。
- **服务端位置**：`/home/elf/open_project/VisionCast/mediamtx/`。
- **运行命令**：
  ```bash
  cd /home/elf/open_project/VisionCast/mediamtx
  ./mediamtx
  ```
- **服务端口配置**（已在 `mediamtx.yml` 中自定义以防冲突）：
  - **RTMP**：`1936`
  - **WebRTC HTTP/WHIP/WHEP**：`8891`
  - **WebRTC UDP/ICE**：`8191`
  - **HLS**：`8890`

---

## 2. 三大协议推拉流测试步骤

### 2.1 WebRTC WHIP / WHEP 模式（超低延迟）
WebRTC 依赖于 WHIP（推送）和 WHEP（拉取/播放）协议，经过 DTLS/SRTP 进行加密传输。

#### 2.1.1 推流测试命令
进入 VisionCast 根目录，摄像头由运行脚本决定，脚本后的第一个参数选择协议：
- **13855 摄像头 WebRTC 推流**：
  ```bash
  ./install/visioncast/scripts/run/run_13855_camera.sh webrtc
  ```
- **USB C270 摄像头 WebRTC 推流**：
  ```bash
  ./install/visioncast/scripts/run/run_usb_camera.sh webrtc
  ```

#### 2.1.2 浏览器拉流播放
MediaMTX 提供内置的 WebRTC 网页播放器。
- **在本机测试拉流**：
  使用板端 Chromium 浏览器访问 `http://127.0.0.1:8891/live/stream`。
- **在跨设备测试拉流（同局域网 PC 等）**：
  在电脑浏览器中访问 `http://192.168.137.202:8891/live/stream` 即可播放画面。

#### 2.1.3 核心技术保障点（故障排查参考）
> [!IMPORTANT]
> - **DTLS 握手超时问题解决**：在本地环回测试中，如果 SDP Offer 的 DTLS `setup` 角色为默认的 `actpass`，通常会导致 Pion (MediaMTX) 反馈 `setup:active` 要求服务器发起握手，但这在本地环回接口下可能会丢包或超时。
> - **解决方案机制**：VisionCast 已在代码中实现 Offer 的 SDP 修改，强制将 `a=setup:actpass` 替换为 `a=setup:active`。这确保了客户端自身作为 DTLS 客户端发起 `ClientHello` 握手，使连接在数毫秒内建立，彻底消除了 "stream not found" 的挂起报错。
> - **回环口绑定 (bindAddress)**：在回环测试时，`libdatachannel` 会自动解析并绑定到 `127.0.0.1` 物理回环，避免局域网广播风暴或防火墙路由拦截。

---

### 2.2 RTMP 模式
RTMP 具有广泛的传统播放器兼容性，通过 TCP 建立稳定的推流管道。

#### 2.2.1 推流测试命令
- **13855 摄像头推流到默认 RTMP 地址**：
  ```bash
  ./install/visioncast/scripts/run/run_13855_camera.sh rtmp
  ```
- **USB C270 摄像头推流到默认 RTMP 地址**：
  ```bash
  ./install/visioncast/scripts/run/run_usb_camera.sh rtmp
  ```

#### 2.2.2 播放拉流命令
使用 `VLC`、`ffplay` 或 `mpv` 等播放器进行拉流测试。
- 默认配置为 H.264 RTMP；若在 `config/visioncast_config.json` 中将 `encoder.video_codec` 切换到 `h265`，播放端和服务端需要支持 Enhanced RTMP/HEVC。
- **使用 ffplay 拉流播放**：
  ```bash
  ffplay rtmp://192.168.137.202:1936/live/stream
  ```

---

### 2.3 RTP over UDP 模式（无协议开销，极低延迟）
RTP over UDP 是一种纯裸流单向推送机制，完全剔除了握手及确认交互。

#### 2.3.1 推流测试命令
程序会将 H.264/H.265 视频和 Opus 音频 RTP 包分别推送到目标地址的两个 UDP 端口上。
- **推流到本机测试**：
  目标 IP 设置为 `127.0.0.1`，视频端口 `5004`，音频端口 `5006`：
  ```bash
  env LD_LIBRARY_PATH=./install/visioncast/lib ./install/visioncast/bin/visioncast --protocol rtp --server-ip 127.0.0.1 --video-port 5004 --audio-port 5006
  ```
- **跨设备推流测试**（将流主动推送到 PC 主机 `192.168.137.1`）：
  ```bash
  env LD_LIBRARY_PATH=./install/visioncast/lib ./install/visioncast/bin/visioncast --protocol rtp --server-ip 192.168.137.1 --video-port 5004 --audio-port 5006
  ```

#### 2.3.2 核心技术保障点（故障排查参考）
> [!TIP]
> - **空挂断容错**：在以往的 RTP over UDP 设计中，若接收端（播放器）不在线就启动推流，内核返回的 `ECONNREFUSED`（连接被拒）ICMP 报文会导致推流程序抛出网络错误并崩溃。
> - **解决方案**：VisionCast 已经在底层修复了此异常。即使播放器不在线，UDP 推流仍会安静地、源源不断地进行，等待随时上线的播放器进行接收。

#### 2.3.3 使用 SDP 描述文件播放
UDP 裸流没有信令协商，播放器必须根据 `SDP` 描述文件来识别音视频的负载类型及端口。
系统在运行 RTP 模式时会在工作目录下自动生成 `test.sdp` 文件。文件内容示例如下：
```text
v=0
o=- 0 0 IN IP4 127.0.0.1
s=VisionCast RTP Stream
c=IN IP4 127.0.0.1
t=0 0
m=video 5004 RTP/AVP 96
a=rtpmap:96 H264/90000
a=fmtp:96 packetization-mode=1;profile-level-id=42e01f
m=audio 5006 RTP/AVP 111
a=rtpmap:111 opus/48000/2
a=fmtp:111 minptime=10;maxaveragebitrate=64000;stereo=0;sprop-stereo=0;useinbandfec=1
```
若 `encoder.video_codec` 配置为 `h265`，视频 SDP 会改为 `a=rtpmap:96 H265/90000` 并写入 H.265 fmtp。
**跨设备播放步骤**：
1. 将板端生成的 `test.sdp` 拷贝到拉流 PC 端。
2. 用文本编辑器打开并将 `c=IN IP4 127.0.0.1` 修改为板端推流的实际目的地 IP（若为 PC 接收，则改写为 PC 本机 IP，例如 `192.168.137.1`）。
3. 使用播放器打开该 sdp 文件播放：
   ```bash
   ffplay -protocol_whitelist file,rtp,udp test.sdp
   ```

---

## 3. 常见故障自查清单

1. **推流提示 "device busy"**：
   - 检查摄像头是否已被其他进程占用（可以使用 `fuser -v /dev/video*` 或 `lsof` 排查）。
2. **WebRTC 拉流黑屏但显示 Connected**：
   - 确认播放端是否支持当前 `encoder.video_codec`。H.265 WebRTC 解码能力依赖浏览器、系统和硬件支持；兼容性优先时使用 H.264 配置。
   - 检查局域网防火墙是否放行了 ICE UDP 端口（例如 `8191`）。
3. **音视频不同步**：
   - 确认板端时间戳是否开启绝对时间基准。
   - 检查音频采集是否发生断续（可在配置中开启 `enable_perf_log` 查看流水线 FPS 状态）。
