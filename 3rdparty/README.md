# VisionCast 第三方依赖说明

`3rdparty/` 保存 VisionCast 交叉编译和目标板运行使用的第三方头文件及 aarch64 库文件。项目的 `CMakeLists.txt` 只从本目录选择 MPP、RGA、RTSP、FFmpeg、JPEG 和 ALSA 依赖，不再引用其他工程或 SDK 中的绝对路径。

## 目录结构

```text
3rdparty/
├── mpp/
│   ├── include/rockchip/
│   └── Linux/aarch64/
├── rga/
│   ├── include/
│   └── lib/Linux/aarch64/
├── rtsp/
│   ├── include/
│   └── lib/aarch64/
├── ffmpeg/
│   ├── include/
│   └── lib/aarch64/
├── jpeg/
    ├── include/
    └── lib/aarch64/
├── webrtc/
│   ├── include/
│   └── lib/aarch64/
└── alsa/
    └── lib/aarch64/
```

## Rockchip MPP

主要文件：

- `mpp/include/rockchip/rk_mpi.h`：MPP 编解码主接口。
- `mpp/include/rockchip/mpp_frame.h`：视频帧及 stride、格式、缓冲区描述。
- `mpp/include/rockchip/mpp_packet.h`：编码包和解码输入包接口。
- `mpp/include/rockchip/mpp_task.h`：Advanced Task API，用于 MJPEG 硬件解码任务。
- `mpp/include/rockchip/mpp_dec_cfg.h`：MJPEG 解码器输出格式和解析模式配置。
- `mpp/include/rockchip/rk_venc_cfg.h`：H.264 编码参数配置接口。
- `mpp/Linux/aarch64/librockchip_mpp.so*`：RK3588 MPP 运行库，SONAME 为 `librockchip_mpp.so.1`。

项目用途：

- `src/media/mjpeg_decoder.cpp` 使用 MPP `MPP_VIDEO_CodingMJPEG` 将 C270 的 MJPEG 压缩帧硬件解码为 NV12。
- `src/media/mpp_encoder.cpp` 使用 MPP 将 NV12 视频帧编码为 H.264 Baseline、CBR、无 B 帧的低延迟码流。

构建开关：

- CMake 找到本目录头文件和库后定义 `VISIONCAST_ENABLE_MPP=1`。
- 目标板运行时必须能加载 `librockchip_mpp.so.1`，交叉安装阶段会复制到 `install/visioncast/lib/`。

## Rockchip RGA

主要文件：

- `rga/include/im2d.h`、`im2d.hpp`：RGA im2d 图像处理接口。
- `rga/include/rga.h`、`RgaUtils.h`：RGA 缓冲和格式辅助定义。
- `rga/lib/Linux/aarch64/librga.so`：RK3588 RGA 运行库。

项目用途：

- `src/media/rga_processor.cpp` 使用 RGA 完成 NV12/YUYV/RGB 图像的颜色转换和分辨率缩放。
- MIPI 摄像头已经输出 NV12 时用于必要的缩放；USB MJPEG 经 MPP 解码为 NV12 后也进入同一处理链路。

构建开关：

- CMake 找到 `im2d.h` 和 `librga.so` 后定义 `VISIONCAST_ENABLE_RGA=1`。
- 未启用 RGA 的主机调试构建使用 CPU 转换路径。

## Rockchip RTSP Helper

主要文件：

- `rtsp/include/rtsp_demo.h`：轻量 RTSP 服务端接口。
- `rtsp/lib/aarch64/librtsp.a`：RTSP/RTP 服务端静态库，支持 H.264 和 G.711A，经 UDP 或 TCP 向客户端发送。

项目用途：

- `src/transport/rtsp_server.cpp` 创建监听端口和会话路径。
- 视频通过 `rtsp_tx_video()` 注入 MPP 产生的 H.264 Annex-B 帧。
- 音频通过 `rtsp_tx_audio()` 注入 G.711 A-law 数据。
- 默认地址为 `rtsp://<板端IP>:8554/live`。

构建开关：

- CMake 找到 `rtsp_demo.h` 和 `librtsp.a` 后定义 `VISIONCAST_ENABLE_RTSP=1`。
- 此库静态链接进 `visioncast`，部署时不需要单独复制 `librtsp.a`。

## FFmpeg

主要头文件目录：

- `ffmpeg/include/libavformat/`：FLV 封装、RTMP 网络输出和容器写入接口。
- `ffmpeg/include/libavcodec/`：AAC 编码器和编码包接口。
- `ffmpeg/include/libavutil/`：帧、时间基、内存、采样格式等基础接口。
- `ffmpeg/include/libswresample/`：S16_LE PCM 到 AAC 编码采样格式的转换。

主要库文件：

- `libavformat.so.58*`：把 H.264/AAC 封装为 FLV，并通过 RTMP URL 主动推流。
- `libavcodec.so.58*`：将 ALSA 捕获的 PCM 音频编码为 AAC。
- `libavutil.so.56*`：FFmpeg 公共数据结构、内存和时间工具。
- `libswresample.so.3*`：音频采样格式转换。

项目用途：

- `src/transport/rtmp_pusher.cpp` 从首个 H.264 关键帧提取 SPS/PPS，生成 AVCDecoderConfigurationRecord。
- 视频以 H.264 写入 FLV，音频由 48 kHz S16_LE PCM 编码为 AAC 后写入同一 RTMP 流。
- 默认推流地址为 `rtmp://192.168.137.1:1935/live/stream`。

构建开关：

- CMake 找到四组头文件和对应库文件后定义 `VISIONCAST_ENABLE_FFMPEG=1`。
- 主程序通过 `dlopen()` 按需加载 FFmpeg；只有选择 RTMP 模式时才加载这些库，因此 UDP/RTSP 不会因 FFmpeg 间接依赖缺失而无法启动。

运行时注意：

- 本目录保存的是 VisionCast RTMP 后端直接调用的 FFmpeg 库；主程序不在 ELF 中直接链接它们，而是在 RTMP 模式启动时按 SONAME 动态加载。
- Ubuntu 提供的 `libavformat.so.58` 和 `libavcodec.so.58` 还依赖 GnuTLS、XML2、压缩库及部分编解码库。目标板若没有这些间接依赖，RTMP 模式仍会加载失败。
- 交叉安装会把四个直接调用的 FFmpeg 库复制到 `install/visioncast/lib/`，并由 `scripts/build/cross_build.sh` 递归收集其余非 glibc 基础运行库依赖。

## WebRTC libdatachannel

主要文件：

- `webrtc/include/rtc/rtc.hpp`：libdatachannel C++ API 主入口。
- `webrtc/lib/aarch64/libdatachannel.so*`：WebRTC PeerConnection、ICE、DTLS、SRTP 运行库。
- `webrtc/lib/aarch64/libssl.so.3`、`libcrypto.so.3`：libdatachannel 使用的 OpenSSL 运行库。

项目用途：

- `src/transport/webrtc_pusher.cpp` 创建 libdatachannel PeerConnection 和音视频 Track。
- WHIP 信令通过 `webrtc_url` 发起 HTTP POST 交换 SDP answer。
- 视频沿用项目 H.264 RTP packetizer，音频沿用 G.711 A-law RTP packetizer，再交给 WebRTC Track 通过 DTLS/SRTP 发送。

构建行为：

- 执行 `scripts/build/build_webrtc_deps.sh` 可下载并交叉构建 libdatachannel v0.22，输出到本目录。
- CMake 找到 `rtc/rtc.hpp` 和 `libdatachannel.so` 后定义 `VISIONCAST_ENABLE_WEBRTC=1`。
- 交叉安装阶段复制 `libdatachannel.so*`、`libssl.so.3`、`libcrypto.so.3` 到 `install/visioncast/lib/`。

运行时注意：

- 当前 WHIP helper 支持明文 `http://` URL，默认值为 `http://192.168.137.1:8889/live/stream`。
- HTTPS WHIP 需要额外实现 TLS HTTP 客户端，不在当前版本范围内。

## libjpeg-turbo

主要文件：

- `jpeg/include/jpeglib.h`、`jerror.h`、`jmorecfg.h`：JPEG 解码 API。
- `jpeg/include/aarch64-linux-gnu/jconfig.h`：aarch64 构建配置。
- `jpeg/lib/aarch64/libjpeg.so.8*`：aarch64 libjpeg-turbo 运行库。

项目用途：

- `src/media/mjpeg_decoder.cpp` 提供 MJPEG 软件解码降级路径。
- RK3588 正常路径优先使用 MPP 硬解；libjpeg 用于 MPP 初始化不可用时的兼容降级和主机侧调试。

构建行为：

- aarch64 构建使用本目录的头文件和库。
- x86_64 主机调试构建使用主机系统的 `find_package(JPEG)`，不能链接本目录的 aarch64 库。

## ALSA libasound

主要文件：

- `alsa/lib/aarch64/libasound.so.2`：ALSA PCM 用户态运行库。
- `alsa/lib/aarch64/libasound.so.2.0.0`：上述 SONAME 对应的实际库文件。

项目用途：

- `src/media/audio_capture.cpp` 使用 `snd_pcm_open()`、`snd_pcm_set_params()`、`snd_pcm_readi()` 等接口从 `hw:1,0` 采集 48 kHz、单声道、S16_LE PCM。
- ALSA 采集结果进入 G.711A/RTP 音频链路，RTMP 模式则进一步编码为 AAC。

构建与运行行为：

- 音频采集模块通过 `dlopen("libasound.so.2")` 和 `dlsym()` 按需加载，不会让主程序在启动时强依赖 ALSA。
- 当前代码只使用少量稳定的 ALSA PCM C ABI，并在源码中声明所需类型和函数签名，因此编译不需要复制完整 `alsa/*.h` 头文件。
- 交叉安装阶段会将该库复制到 `install/visioncast/lib/`，运行脚本通过 `LD_LIBRARY_PATH` 优先加载它。

## 架构与部署约束

- `lib/` 下的二进制均为 Linux aarch64，目标平台是 RK3588。
- x86_64 主机不能直接加载这些 `.so` 文件。
- 运行脚本设置 `LD_LIBRARY_PATH=<安装目录>/lib`，优先加载安装包中的 MPP、RGA、FFmpeg、JPEG、WebRTC、OpenSSL 和 ALSA 库。
- 更新任一预编译库时，必须同时保留真实版本文件和 SONAME 链接，例如 `libavformat.so.58 -> libavformat.so.58.76.100`。

## 来源记录

- MPP、RGA：来自 VisionCast 已通过交叉编译验证的 Rockchip SDK 头文件与 RK3588 aarch64 运行库；头文件和库必须保持版本兼容。
- RTSP helper：来自 Firefly RK3588 SDK `external/common_algorithm/misc`。
- FFmpeg、libjpeg-turbo、ALSA libasound、OpenSSL arm64 运行库：来自当前 Ubuntu 22.04 aarch64 交叉开发包。
- WebRTC libdatachannel：由 `scripts/build/build_webrtc_deps.sh` 下载上游源码和固定子模块提交交叉构建。

这些文件的许可证仍由各上游项目规定。发布产品前应核对对应版本的许可证和再分发要求。
