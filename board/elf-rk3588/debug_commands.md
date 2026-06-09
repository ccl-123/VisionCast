# ELF-RK3588 VisionCast 调试与运维命令手册

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 状态：系统设备节点已全部锁定，提供日常运行监控、故障自查与推拉流测试的快捷命令手册。

---

## 1. 核心运行与推流命令

### 1.1 使用脚本启动（推荐）
系统在安装包的运行目录下集成了自动化运行脚本，支持传入协议参数直接运行：
- **WebRTC 协议推流**：
  ```bash
  ./install/visioncast/scripts/run/run_usb_camera.sh webrtc
  ```
- **RTMP 协议推流**：
  ```bash
  ./install/visioncast/scripts/run/run_usb_camera.sh rtmp
  ```
- **RTP over UDP 推流**：
  ```bash
  ./install/visioncast/scripts/run/run_usb_camera.sh rtp
  ```

### 1.2 手动命令启动
直接启动主程序，可以通过动态指定动态链接库加载路径（LD_LIBRARY_PATH）：
```bash
env LD_LIBRARY_PATH=./install/visioncast/lib ./install/visioncast/bin/visioncast --protocol webrtc
```

---

## 2. 视频采集设备常用调试

### 2.1 列出所有 V4L2 视频物理设备
```bash
v4l2-ctl --list-devices
```

### 2.2 查看 13855 MIPI 摄像头 (主节点 `/dev/video11`) 属性
```bash
v4l2-ctl -d /dev/video11 -D
v4l2-ctl -d /dev/video11 --list-formats-ext
```

### 2.3 查看 USB C270 (备用节点 `/dev/video21`) 属性
```bash
v4l2-ctl -d /dev/video21 -D
v4l2-ctl -d /dev/video21 --list-formats-ext
```

### 2.4 手动进行 V4L2 视频采集裸流测试
- **主 MIPI 摄像头采集测试 (NV12 720p 100帧)**：
  ```bash
  v4l2-ctl -d /dev/video11 \
    --set-fmt-video=width=1280,height=720,pixelformat=NV12 \
    --stream-mmap=4 \
    --stream-count=100 \
    --stream-to=/tmp/13855_test.yuv
  ```
- **备用 USB 摄像头采集测试 (MJPEG 720p 100帧)**：
  ```bash
  v4l2-ctl -d /dev/video21 \
    --set-fmt-video=width=1280,height=720,pixelformat=MJPG \
    --stream-mmap=4 \
    --stream-count=100 \
    --stream-to=/tmp/c270_test.mjpg
  ```

---

## 3. 音频设备常用调试

### 3.1 列出 ALSA 录音与播放硬件设备
```bash
arecord -l
aplay -l
```

### 3.2 录音与播放闭环验证测试
- **手动录音测试 (双声道 S16_LE 48000Hz 5秒)**：
  ```bash
  arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/audio_test.wav
  ```
- **手动播放录音测试**：
  ```bash
  aplay -D hw:1,0 /tmp/audio_test.wav
  ```

### 3.3 Mixer 模拟音量与增益调节
- **查询主麦克风 (Main Mic) 通道增益属性**：
  ```bash
  amixer -c 1 sget 'Main Mic'
  amixer -c 1 sget 'PGA'
  amixer -c 1 sget 'PGA Boost'
  ```
- **命令行图形化调节 Mixer 控制面板**：
  ```bash
  alsamixer -c 1
  ```
  *(注：按 `F4` 切换至 Capture 采集页面，使用方向键调节 PGA 增益及开关)*

---

## 4. 物理拓扑关系 (Media Graph) 查询

- **查询 CIF (MIPI 接收前端) 拓扑**：
  ```bash
  media-ctl -p -d /dev/media0
  ```
- **查询 ISP (图像处理器与输出节点) 拓扑**：
  ```bash
  media-ctl -p -d /dev/media1
  ```
- **查询 UVC (USB 摄像头) 拓扑**：
  ```bash
  media-ctl -p -d /dev/media2
  ```

---

## 5. 板端运行状况与性能监控

### 5.1 查看 CPU 与进程资源占用率
```bash
htop
# 或者使用 pidstat 对推流进程做每秒统计
pidstat -p $(pgrep visioncast) 1
```

### 5.2 监控 SoC 硬件核心实时温度
```bash
find /sys/class/thermal -name temp -exec sh -c 'echo "$1: $(cat $1)"' _ {} \;
```

### 5.3 监控网络实时流量/码率
```bash
ifstat 1
# 或者使用 sar 统计 eth0 网口流量
sar -n DEV 1
```

### 5.4 查看流媒体端口占用情况
```bash
ss -tulnp | grep -E "1936|8891"
```

---

## 6. 结论与运维指引

1. **零手工配置**：系统启动与运行已通过环境变量与配置文件实现自动化。如遇到音视频断续或无声，可使用本文档第 3 节的 amixer 命令与 arecord 验证硬件健康度。
2. **多协议拉流端接入**：
   - WebRTC WHIP 拉流：浏览器直接访问 `http://BOARD_IP:8891/live/stream`。
   - RTMP 拉流：`ffplay rtmp://BOARD_IP:1936/live/stream`。
   - RTP over UDP 拉流：将板端生成的 `test.sdp` 拷贝至 PC 并运行 `ffplay -protocol_whitelist file,rtp,udp test.sdp`。
