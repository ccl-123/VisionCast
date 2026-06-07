# VisionCast 智能眼镜音视频推流系统 (已完工最终版)

[![Platform](https://img.shields.io/badge/Platform-ELF--RK3588-blue.svg)](https://www.elfboard.com/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Status](https://img.shields.io/badge/Status-Verified%20%26%20Running-brightgreen.svg)]()

VisionCast 是一个基于 **ELF-RK3588** 开发板的智能眼镜音视频推流系统。本项目模拟智能眼镜第一视角采集场景，已实现高性能、低延迟的摄像头视频与麦克风音频实时采集、硬件编码（H.264 Baseline）、本地非阻塞预览渲染、多协议推流分发。

---

## 🛠 硬件与平台规范

- **开发平台**：ELF-RK3588 (Rockchip RK3588)
- **视频输入**：
  - 主通道：13855 MIPI 摄像头 (MIPI CSI-2 接口，NV12 采集，支持 30 FPS 锁定)
  - 备通道：USB C270 摄像头 (支持降级退避，MJPEG 采集硬解)
- **音频输入**：板载 rockchip-nau8822 Codec (ALSA 设备 `hw:1,0`，支持双声道采集降采样为单声道)
- **核心实现**：基于 V4L2/ALSA/RGA/MPP 底层接口，打通多协议超低延迟推流管线，支持局域网 200ms ~ 300ms 端到端时延。

---

## 📁 目录结构与文档指南

整个项目的开发文档与数据流设计分为**项目全局文档**和**板级验证记录**两部分。您可以直接点击以下链接查看详细设计：

### 1. 全局设计与流程文档 (docs/)
- 📄 **[预研与开发文档.md](file:///home/elf/open_project/VisionCast/预研与开发文档.md)**：项目开发技术总结与各层级实现路线说明。
- 📄 **[docs/device_probe.md](file:///home/elf/open_project/VisionCast/docs/device_probe.md)**：ELF-RK3588 音视频设备枚举与探测结果，包含 V4L2 节点、ALSA 卡号及物理麦克风映射。
- 📄 **[docs/camera_13855_flow.md](file:///home/elf/open_project/VisionCast/docs/camera_13855_flow.md)**：13855 摄像头从 Sensor 经过 MIPI, CIF, ISP 到 MPP 编码器的完整视频数据流说明。
- 📄 **[docs/audio_nau8822_flow.md](file:///home/elf/open_project/VisionCast/docs/audio_nau8822_flow.md)**：NAU8822 音频链路，包含 mixer 开关控件、ALSA 采集参数及统一时间戳设计。
- 📄 **[docs/pipeline_design.md](file:///home/elf/open_project/VisionCast/docs/pipeline_design.md)**：系统音视频管线设计，描述多线程缓冲队列模型、各协议（WebRTC/RTMP/RTSP/UDP）的最终设计。
- 📄 **[docs/performance_test.md](file:///home/elf/open_project/VisionCast/docs/performance_test.md)**：性能测试记录，追踪帧率、码率、端到端延迟及 CPU 占用。
- 📄 **[docs/multi_protocol_test_guide.md](file:///home/elf/open_project/VisionCast/docs/multi_protocol_test_guide.md)**：多协议推拉流本地及跨设备实战测试教程。

### 2. 板端验证与调试细节 (board/elf-rk3588/)
- 📄 **[board/elf-rk3588/device_nodes.md](file:///home/elf/open_project/VisionCast/board/elf-rk3588/device_nodes.md)**：板端视频与音频设备节点的详细查询与输出记录。
- 📄 **[board/elf-rk3588/media_graph.md](file:///home/elf/open_project/VisionCast/board/elf-rk3588/media_graph.md)**：RKISP 与 RKCIF media-ctl 拓扑连接图关系与查询步骤。
- 📄 **[board/elf-rk3588/audio_mixer.md](file:///home/elf/open_project/VisionCast/board/elf-rk3588/audio_mixer.md)**：板载 NAU8822 的 amixer 各通道控制开关与增益配置。
- 📄 **[board/elf-rk3588/camera_13855.md](file:///home/elf/open_project/VisionCast/board/elf-rk3588/camera_13855.md)**：13855 摄像头驱动加载、I2C 适配与点亮记录。
- 📄 **[board/elf-rk3588/debug_commands.md](file:///home/elf/open_project/VisionCast/board/elf-rk3588/debug_commands.md)**：常用的音视频通路调试与录制/测试命令汇总。

---

## 📈 有待优化的性能方向 (Future Optimizations)

系统当前版本已经实现了高稳定性与良好的低延迟运行，但在未来的升级中仍有以下点有待进一步优化：
1. **DMA-BUF 零拷贝链路打通**：
   从 V4L2 导出 DMA-BUF FD 直接传递给 RGA 导入，再将转换后的 FD 传给 MPP 编码。实现纯硬件显存级别零拷贝流转，将端到端延迟降低至 150ms 级别。
2. **动态码率与拥塞控制 (Congestion Control)**：
   目前推流仅使用固定的 CBR 码率。未来可引入 WebRTC/RTCP 反馈机制，读取客户端返回的丢包率和 RTT 延迟，实时动态调整 MPP 编码器的 `Bitrate` 目标。
3. **Opus 高质量音频编码支持**：
   当前 WebRTC 中音频使用 G.711A (PCMA)。未来可交叉编译静态 `libopus` 并集成到项目中，提供高质量、低延迟的双向音频对讲。
4. **H.265 硬件视频编码支持**：
   在配置文件和推流层中新增 H.265 支线，利用 MPP 提供的 H.265 硬编码器提高弱网下的图像带宽比。
