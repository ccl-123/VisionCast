# ELF-RK3588 VisionCast 调试命令

> 项目：VisionCast 智能眼镜音视频推流系统
> 平台：ELF-RK3588
> 文档作用：集中记录摄像头、音频、MPP、RGA、网络、推流相关调试命令。

---

## 1. 系统信息

```bash
uname -a
uname -m
cat /etc/os-release
free -h
df -h
uptime
```

查看 CPU：

```bash
lscpu
```

查看内存：

```bash
free -h
cat /proc/meminfo | head
```

---

## 2. 视频设备枚举

```bash
v4l2-ctl --list-devices
```

查看某个节点信息：

```bash
v4l2-ctl -d /dev/videoX -D
v4l2-ctl -d /dev/videoX --all
v4l2-ctl -d /dev/videoX --list-formats-ext
```

---

## 3. 13855 / MIPI / ISP 日志

```bash
dmesg | grep -Ei "13855|ov13855|camera|mipi|csi|rkcif|rkisp"
```

查看 I2C 设备名：

```bash
for f in /sys/bus/i2c/devices/*/name; do
    echo "$f: $(cat $f)"
done | grep -Ei "ov|13855|camera|imx|gc"
```

---

## 4. Media Graph

RKCIF：

```bash
media-ctl -p -d /dev/media0
```

RKISP：

```bash
media-ctl -p -d /dev/media1
```

USB C270：

```bash
media-ctl -p -d /dev/media2
```

---

## 5. RKISP mainpath 节点能力

```bash
for i in {11..17}; do
    echo "===== /dev/video$i ====="
    v4l2-ctl -d /dev/video$i -D || true
    v4l2-ctl -d /dev/video$i --list-formats-ext || true
done
```

---

## 6. USB C270 节点能力

```bash
v4l2-ctl -d /dev/video21 -D
v4l2-ctl -d /dev/video21 --list-formats-ext

v4l2-ctl -d /dev/video22 -D
v4l2-ctl -d /dev/video22 --list-formats-ext
```

---

## 7. V4L2 采集测试

### 7.1 C270 MJPEG 测试

```bash
v4l2-ctl -d /dev/video21 \
  --set-fmt-video=width=1280,height=720,pixelformat=MJPG \
  --stream-mmap=4 \
  --stream-count=100 \
  --stream-to=/tmp/c270_test.mjpg
```

### 7.2 C270 YUYV 测试

```bash
v4l2-ctl -d /dev/video21 \
  --set-fmt-video=width=640,height=480,pixelformat=YUYV \
  --stream-mmap=4 \
  --stream-count=100 \
  --stream-to=/tmp/c270_test.yuv
```

### 7.3 13855 候选节点测试

```bash
v4l2-ctl -d /dev/videoX \
  --stream-mmap=4 \
  --stream-count=100 \
  --stream-to=/tmp/13855_test.yuv
```

指定格式测试：

```bash
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=1280,height=720,pixelformat=NV12 \
  --stream-mmap=4 \
  --stream-count=100 \
  --stream-to=/tmp/13855_720p_nv12.yuv
```

---

## 8. 音频设备查询

```bash
arecord -l
aplay -l
cat /proc/asound/cards
cat /proc/asound/pcm
```

查询 ALSA 参数：

```bash
arecord -D hw:1,0 --dump-hw-params
```

---

## 9. NAU8822 Mixer

```bash
amixer -c 1 scontrols
amixer -c 1 contents
```

图形化控制：

```bash
alsamixer -c 1
```

常用查询：

```bash
amixer -c 1 sget 'Main Mic'
amixer -c 1 sget 'Headset Mic'
amixer -c 1 sget 'ADC'
amixer -c 1 sget 'PGA'
amixer -c 1 sget 'PGA Boost'
```

---

## 10. 音频录放测试

录音：

```bash
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/audio_test.wav
```

播放：

```bash
aplay -D hw:1,0 /tmp/audio_test.wav
```

单声道录音：

```bash
arecord -D hw:1,0 -f S16_LE -r 48000 -c 1 -d 5 /tmp/audio_mono.wav
```

---

## 11. USB Audio 检查

```bash
lsmod | grep snd_usb_audio
sudo modprobe snd_usb_audio
dmesg | grep -Ei "usb audio|snd_usb|C270|webcam|audio"
```

当前已知结论：

```text
当前内核缺少 snd_usb_audio 模块，USB C270 麦克风暂不可用。
```

---

## 12. MPP 检查

```bash
which mpi_enc_test
which mpi_dec_test
which mpp_info_test
```

查询库：

```bash
ldconfig -p | grep -i mpp
find /usr/include -iname "rk_mpi.h" 2>/dev/null
find /usr/include -iname "mpp_frame.h" 2>/dev/null
find /usr/lib -iname "librockchip_mpp.so*" 2>/dev/null
find /lib -iname "librockchip_mpp.so*" 2>/dev/null
```

编码测试程序：

```bash
mpi_enc_test
```

查看帮助：

```bash
mpi_enc_test --help
```

---

## 13. RGA 检查

```bash
ldconfig -p | grep -i rga
find /usr/include -iname "*rga*" 2>/dev/null
find /usr/lib -iname "*rga*" 2>/dev/null
find /lib -iname "*rga*" 2>/dev/null
```

---

## 14. RKAIQ 检查

```bash
ps aux | grep -i rkaiq
find /etc -iname "*iq*" 2>/dev/null
find /usr -iname "*rkaiq*" 2>/dev/null | head -50
```

查找 IQ 文件：

```bash
find / -iname "*13855*" 2>/dev/null
find / -iname "*ov13855*" 2>/dev/null
```

---

## 15. 网络检查

```bash
ip addr
ip route
ping -c 4 192.168.137.1
ping -c 4 baidu.com
```

查看端口占用：

```bash
ss -tulnp
```

---

## 16. RTSP 播放测试

板端推流后，在 PC 或板端测试：

```bash
ffplay rtsp://BOARD_IP:8554/live
```

VLC 播放地址：

```text
rtsp://BOARD_IP:8554/live
```

---

## 17. UDP/RTP 调试

监听 UDP：

```bash
nc -ul 5004
```

抓包：

```bash
sudo tcpdump -i eth0 udp port 5004
```

---

## 18. 性能监控

CPU：

```bash
top
htop
pidstat -p <pid> 1
```

内存：

```bash
free -h
cat /proc/<pid>/status | grep -E "VmRSS|VmSize"
```

温度：

```bash
find /sys/class/thermal -name temp -exec sh -c 'echo "$1: $(cat $1)"' _ {} \;
```

磁盘：

```bash
df -h
du -sh *
```

---

## 19. 项目构建相关

本地构建：

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

清理：

```bash
rm -rf build/*
```

---

## 20. 当前调试优先级

当前最优先执行：

```bash
media-ctl -p -d /dev/media0
media-ctl -p -d /dev/media1
```

然后执行：

```bash
for i in {11..17}; do
    echo "===== /dev/video$i ====="
    v4l2-ctl -d /dev/video$i -D
    v4l2-ctl -d /dev/video$i --list-formats-ext
done
```

目标：

```text
确认 13855 最终从哪个 /dev/videoX 输出。
```

---

## 21. 当前结论

1. VisionCast 当前优先确认 13855 输出节点；
2. 音频默认使用 NAU8822 `hw:1,0`；
3. USB C270 可作为视频备用输入；
4. USB C270 麦克风当前不可用；
5. MPP/RGA/RKAIQ 路径需要进一步确认；
6. 首版目标是 RTSP/RTP 推流闭环。
