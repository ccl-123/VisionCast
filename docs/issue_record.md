# VisionCast 开发问题与 Bug 解决记录

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

## 3. 13855 暗光下条纹与画面不清晰问题

- **现象**：13855 画面在当前环境下出现条纹、噪点和清晰度下降。
- **原因**：排查时 `/dev/v4l-subdev2` 的 `analogue_gain` 被 3A 拉到最大值 `1984`，同时低增益原始 NV12 帧的 Y 均值只有约 `2/255`，说明实际进光不足。最大模拟增益会放大暗电流噪声和固定模式噪声；固定 `exposure=3000` 又不贴近室内灯光频闪周期，容易出现滚动快门横向明暗带。dmesg 未见 MIPI/CSI ECC、CRC 或 overflow 错误，基本排除传输链路损坏。
- **已实现的解决办法**：新增 `sensor_analogue_gain` 与 V4L2 crop 配置项。13855 默认锁定 `sensor_exposure=1928`、`sensor_vblank=78`、`sensor_analogue_gain=1536`，并设置居中 16:9 crop `0,380,4224x2376`。该组合用于贴近 50Hz 灯光的 20ms 积分窗口、限制最大增益噪声并避免沿用 ISP 上一次 crop 状态。若现场光线仍不足，需要补光、检查镜头遮挡/脏污/焦距，或继续在亮度和条纹之间微调 exposure/gain。

---

## 4. WebRTC WHIP 本地回环 DTLS 握手超时问题

- **现象**：在进行 WebRTC (WHIP) 本地回环测试时，连接始终在 DTLS 握手步骤挂起，10 秒后连接被踢掉超时，浏览器拉流提示 `stream not found`。
- **原因**：默认 libdatachannel 生成的 Offer 中 DTLS 角色设置为 `a=setup:actpass`，这会导致 Pion (MediaMTX) 强行协商服务器作为 active 发起端，但在回环接口路由策略下，服务器主动发送握手包往往受挫丢包；同时，环回接口可能存在多个候选网卡地址冲突。
- **已实现的解决办法**：
  1. 在 `WebrtcPusher` 中，若检测到目标 URL 为 localhost 或 127.0.0.1，强制配置 `config.bindAddress` 绑定至 `127.0.0.1` 环回物理接口。
  2. 在 SDP 交换前，对生成的 Offer 字符串做动态正则替换，将 `a=setup:actpass` 强制重写为 `a=setup:active`。这会强迫 MediaMTX 成为 passive 接收端，迫使 WHIP 客户端主动发出 `ClientHello` 握手。该修改将 DTLS 建连协商降到了 5ms 级。

---

## 5. RTP over UDP 空挂断导致程序崩溃问题

- **现象**：在启动 UDP 协议推流时，如果拉流端 (VLC/ffplay) 还没有上线开启监听，或者拉流端中途关闭，推流主程序会由于网络发送返回 `ECONNREFUSED` (连接被拒) 错误而直接崩溃。
- **原因**：`UdpSender` 内部使用了 `connect()` 绑定了目的套接字。在无监听状态下发送数据，系统协议栈会收到 ICMP 端口不可达消息，导致下一个 `send` 调用抛出 `ECONNREFUSED` 致命错误。
- **已实现的解决办法**：在 `UdpSender::send` 发送逻辑中拦截了 `ECONNREFUSED` 报错，记录警告日志但允许其安全返回 `true`。避免了无连接协议因为拉流端离线而拖垮推流主管线，实现了接收端的无缝热插拔。

---

## 6. RTMP 画面写入 FLV Muxer 格式错误

- **现象**：RTMP 推流日志正常，但是拉流端渲染黑屏或提示格式解析失败。
- **原因**：MPP 硬编码输出的是 Annex-B 字节流（包含 3 字节或 4 字节的 `0x000001` / `0x00000001` 起始码）。旧版 FFmpeg FLV muxer 对 H.264/H.265 封装能力有限，直接写入或缺少正确参数集时会导致播放端反初始化解码器失败。
- **已实现的解决办法**：升级项目内置 FFmpeg 到 8.1.1。`RtmpPusher` 只从首帧提取 H.264 SPS/PPS 或 H.265 VPS/SPS/PPS 作为 extradata，后续 Annex-B packet 交给 FFmpeg FLV muxer，由其生成 AVC 或 Enhanced RTMP/HEVC 封装。

---

## 7. Ubuntu 桌面环境下本地预览不可见问题 (DisplayRenderer)

- **现象**：当 ELF-RK3588 上开启了图形桌面环境时，程序向 `/dev/fb0` (Framebuffer) 写入的像素直接被 X11/GNOME 窗口管理器的桌面覆盖，无法显示本地画面。
- **原因**：桌面窗口系统的合成管理器 (Compositor) 独占了屏幕绘制权限，导致直接写显存被瞬间覆盖。
- **已实现的解决办法**：在 `DisplayRenderer` 中实现了自适应的渲染后端。程序优先检测 X11 环境变量。在有桌面环境下，自动动态 dlopen `libX11.so.6` 并在一个独立的异步 X11 窗口中利用 RGA 加速渲染视频流；当在纯命令行环境（无 X11 图形界面）下，则自动退避到原有的 `/dev/fb0` Framebuffer 渲染逻辑中。预览由 `debug.enable_preview` 控制，编码成功后移动同帧 `VideoFrame` 到预览队列；DMA-only 主路径开启预览时通过 RGA fd 输入渲染，不需要把整帧 NV12 拷贝回用户态。X11/fb0 显示提交仍有转换和输出开销，不属于显示端到端零拷贝。

---

## 8. FFmpeg WHIP 升级后推流超时挂起与 DTLS 握手失败问题

- **现象**：在使用内置升级后的 FFmpeg 8.1.1 运行 WebRTC (WHIP) 推流脚本时，程序在连接初期无限期挂起或报错，伴随有 `Handshake failed, r0=-1, r1=5` 和 `DTLS session failed` 的网络错误。
- **原因**：
  1. **HTTP 代理拦截**：开发板配置了 `http_proxy` 环境变量。FFmpeg HTTP 客户端由于不支持 CIDR (如 `192.168.137.0/24`) 的 `no_proxy` 过滤规则，将本地 WHIP 连接转给主机的 Clash 代理端口，导致 HTTP POST 挂起或返回 502。
  2. **不可达 ICE 候选地址**：流媒体服务器主机开启了代理 TUN 网卡（IP为 `198.18.0.1`）。MediaMTX 将其作为首个候选地址发送给客户端。而 FFmpeg whip 封装器极简设计下仅支持单候选地址，盲目挑选第一个 candidate 进行绑定，导致板端向不可达的 `198.18.0.1` 发送 DTLS 握手，产生 `r1=5` (SSL_ERROR_SYSCALL) 错误。
  3. **中断回调悬空指针**：为限制连接挂起时间，代码中在 `push_video` 栈上分配了 `CallbackOpaque` 并传递给 FFmpeg 注册为中断回调。当 `avformat_write_header` returned 后，该变量退栈被销毁，而底层的 `URLContext` 在后续发送媒体帧时仍以值拷贝保留了对该悬空指针的调用，导致异常超时退出。
- **已实现的解决办法**：
  1. **脚本代理清理**：在运行脚本 `run_13855_camera.sh` 和 `run_usb_camera.sh` 执行前一律 unset 代理环境变量，防止本地局域网流量被拦截。
  2. **ICE 候选地址优先级过滤补丁**：修改 MediaMTX 服务端配置以只对外广播可达 IP 候选地址；并在 FFmpeg 编译期自动执行 `patch_whip.py` 脚本，补丁改写了 `whip.c`，使其在解析 SDP 答复时优先选择与 WHIP URL 宿主机 IP 完全相同的候选地址，实现复杂网卡拓扑自适应。
  3. **堆分配生命周期锁定**：将连接超时结构体 `CallbackOpaque` 放置于堆上（`Impl` 的成员变量内），并在建连完成之后设置 `disabled` 标志位，确保即使 FFmpeg 协议栈底层重入回调，也不会发生非法内存访问或误判超时。

---

## 9. 音视频采集 PTS 延迟与同步误差问题

- **现象**：低延迟推流下，音视频数据时间戳存在固定与不确定的偏差，音画同步未达到最佳状态。
- **原因**：
  1. **音频 PTS 滞后**：音频采集线程在 `snd_pcm_readi()` 返回后直接获取当前系统时间打点。因为该接口为阻塞式读取，返回时刻对应当前 Period 数据的结束时间，导致音频 PTS 整体滞后了一个 buffer 周期（默认 20ms）。
  2. **视频 PTS 延迟与抖动**：视频采集线程在 `VIDIOC_DQBUF` 出队成功后打上当前系统时间，这不仅计入了硬件曝光与物理传输延迟，还包含了用户态线程调度的随机抖动，导致视频 PTS 存在 32ms~61ms 的额外延迟与抖动。
- **已实现的解决办法**：
  1. **音频起点时间补偿**：在 `snd_pcm_readi()` 读取成功（`frames > 0`）后，结合实际读取的采样帧数与采样率，精确算得当前 period 缓冲区的物理持续时长，并从当前时间回推该时长作为最终 PTS。这样可使 `AudioFrame::pts_us` 精确对应这段 PCM 的第一个采样点（起点）时间。
  2. **视频优先使用 Monotonic 驱动时间戳**：在 `VIDIOC_DQBUF` 返回后，检查缓冲区 `buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK` 是否符合 `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`。符合且数值合法时，直接采用驱动自带的硬件级单调时间戳（消除 32ms~61ms 用户态延迟及调度抖动）；否则，安全退避至用户态出队时刻打点。

