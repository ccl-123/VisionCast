# ELF-RK3588 设备节点记录 (已完工版)

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 状态：音视频硬件采集节点已全部确认并锁定，系统支持物理摄像头的平滑退避降级与音频混音。

---

## 1. 设备节点总览

当前 ELF-RK3588 硬件开发板上，音视频采集与播放对应的物理节点已全部锁定：

1. **视频主通道**：13855 MIPI 摄像头，对应 `rkisp_mainpath` 节点 `/dev/video11`。
2. **视频备通道**：USB C270 摄像头，对应 UVC 节点 `/dev/video21`。
3. **音频采集通道**：NAU8822 Codec，对应 ALSA 采集节点 `hw:1,0`。
4. **音频播放通道**：耳机与板载输出，对应 ALSA 播放节点 `hw:1,0`。

---

## 2. 视频设备枚举与分类

板端执行 `v4l2-ctl --list-devices` 输出的节点类型划分如下：

| 物理节点 | 驱动类型名称 | 节点实际用途 | 运行状态 |
| --- | --- | --- | --- |
| `/dev/media0` | rkcif-mipi-lvds | RKCIF 前端控制器拓扑 | 仅用作拓扑连接查询 |
| `/dev/video0~10` | rkcif | MIPI 物理接收前端 | 系统底层流转，不直接读取 |
| `/dev/media1` | rkisp_mainpath | RKISP 图像处理器拓扑 | 仅用作拓扑连接查询 |
| `/dev/video11` | rkisp_mainpath | **13855 MIPI CSI 采集主节点** | **正常工作**，输出 NV12 |
| `/dev/video12~17` | rkisp_mainpath | ISP 备用图像通道 | 未使用 |
| `/dev/video18~19` | rkisp-statistics | 3A 自动曝光与白平衡统计节点 | **正常工作**，rkaiq 进程占用 |
| `/dev/video20` | rk_hdmirx | HDMI 采集输入 | 未使用 |
| `/dev/video21` | C270 HD WEBCAM | **USB 摄像头备用节点** | **正常工作**，MJPEG 采集降级 |
| `/dev/video22` | C270 HD WEBCAM | USB 摄像头 metadata 节点 | 未使用 |
| `/dev/media2` | C270 HD WEBCAM | USB 摄像头物理拓扑 | 未使用 |

---

## 3. 13855 MIPI 摄像头节点确认与特性
- **最终物理节点**：`/dev/video11` (rkisp_mainpath)。
- **视频帧能力**：输出 1280x720 30FPS，格式为多平面 NV12。
- **稳定性控制**：由 `VideoCapture` 底层通过 V4L2 selection 固定 16:9 crop，并调用 `/dev/v4l-subdev2` 锁定 Vertical Blanking、Exposure 与 Analogue Gain，消除暗光对物理帧率的干扰，减少灯光频闪横纹，并限制最大增益带来的条纹噪声。

---

## 4. USB C270 摄像头备用节点与退避策略
- **视频节点**：`/dev/video21`。
- **采集规格**：采用 1280x720 30FPS 的 MJPEG 数据源。
- **降级机制**：在主程序无法正常打开 `/dev/video11` 时，软件架构会安全捕获异常，将配置文件中的视频源退避降级至 `/dev/video21`，并通过硬解码转换为 NV12，以保证系统的推流鲁棒性。

---

## 5. 音频采集与播放设备

### 5.1 音频采集
- **物理声卡**：板载 NAU8822 硬件 Codec。
- **配置节点**：`hw:1,0`
- **混音模式**：采集 48000Hz 原始 PCM，在 ALSA 硬件层拒绝单声道时自动以双声道采集并进行均值混音，实现稳定的单声道输出。

### 5.2 播放设备
- **`hw:1,0`**：板载耳机接口。
- **`hw:2,0`**：板载 HDMI 音频设备。
- **`hw:0,0`**：DP 音频通道。

### 5.3 USB 摄像头麦克风不可用状态
由于开发板 Linux 内核裁剪了 `snd_usb_audio` 模块，因此 USB 摄像头的自带麦克风在系统中无法加载，程序统一仅使用 NAU8822 (`hw:1,0`) 的物理 Mic 作为输入源。

---

## 6. 最终结论与运行保障

1. 音视频硬件管线已经全部锁定物理节点，即：
   - 视频主节点：`/dev/video11`
   - 视频备节点：`/dev/video21`
   - 音频节点：`hw:1,0`
2. 系统已在配置文件 `config/visioncast_config.json` 中配置完毕，启动脚本与可执行二进制文件会自动拉取对应节点，不需要人工再次执行探测。
