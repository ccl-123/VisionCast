# VisionCast 智能眼镜音视频推流系统

[![Platform](https://img.shields.io/badge/Platform-ELF--RK3588-blue.svg)](https://www.elfboard.com/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Status](https://img.shields.io/badge/Status-Verified%20%26%20Running-brightgreen.svg)]()

VisionCast 是一个基于 **ELF-RK3588** 开发板的智能眼镜音视频推流系统预研项目。本项目模拟智能眼镜第一视角采集场景，旨在实现高品质、低延迟的摄像头视频与麦克风音频实时采集、硬件编码（H.264/H.265）、封装并进行网络推流。

---

## 🛠 硬件与平台规范

- **开发平台**：ELF-RK3588 (Rockchip RK3588)
- **视频输入**：
  - 正式：13855 MIPI 摄像头 (MIPI CSI-2 接口，支持 RAW Bayer 采集)

- **音频输入**：板载 rockchip-nau8822 Codec (ALSA 设备 `hw:1,0`)
- **核心优化目标**：基于 V4L2/ALSA/RGA/MPP 底层接口，通过 DMA-BUF 零拷贝技术实现极致低延迟推流。

---

## 📁 目录结构与文档指南

整个项目的开发文档与数据流设计分为**项目全局文档**和**板级验证记录**两部分。您可以直接点击以下链接查看详细设计：

### 1. 全局设计与流程文档 (docs/)
- 📄 **[预研与开发文档.md](file:///home/cl/EC-A3588Q/VisionCast/预研与开发文档.md)**：项目预研与开发路线图，阐述技术选型与多阶段演进目标。
- 📄 **[docs/device_probe.md](file:///home/cl/EC-A3588Q/VisionCast/docs/device_probe.md)**：ELF-RK3588 音视频设备枚举记录，包括 V4L2 节点、ALSA 卡号及物理麦克风映射。
- 📄 **[docs/camera_13855_flow.md](file:///home/cl/EC-A3588Q/VisionCast/docs/camera_13855_flow.md)**：13855 摄像头从 Sensor 经过 MIPI, CIF, ISP 到 MPP 编码器的完整视频数据流设计。
- 📄 **[docs/audio_nau8822_flow.md](file:///home/cl/EC-A3588Q/VisionCast/docs/audio_nau8822_flow.md)**：NAU8822 音频链路，包含 mixer 开关控件、ALSA 采集参数及统一时间戳设计。
- 📄 **[docs/pipeline_design.md](file:///home/cl/EC-A3588Q/VisionCast/docs/pipeline_design.md)**：系统音视频管线设计，描述多线程缓冲队列模型、各协议（RTSP/SRT/WebRTC）的演进路线。
- 📄 **[docs/performance_test.md](file:///home/cl/EC-A3588Q/VisionCast/docs/performance_test.md)**：性能测试记录，追踪帧率、码率、端到端延迟及 CPU 占用。

### 2. 板端验证与调试细节 (board/elf-rk3588/)
- 📄 **[board/elf-rk3588/device_nodes.md](file:///home/cl/EC-A3588Q/VisionCast/board/elf-rk3588/device_nodes.md)**：板端视频与音频设备节点的详细查询与输出记录。
- 📄 **[board/elf-rk3588/media_graph.md](file:///home/cl/EC-A3588Q/VisionCast/board/elf-rk3588/media_graph.md)**：RKISP 与 RKCIF media-ctl 拓扑连接图关系与查询步骤。
- 📄 **[board/elf-rk3588/audio_mixer.md](file:///home/cl/EC-A3588Q/VisionCast/board/elf-rk3588/audio_mixer.md)**：板载 NAU8822 的 amixer 各通道控制开关与增益配置。
- 📄 **[board/elf-rk3588/camera_13855.md](file:///home/cl/EC-A3588Q/VisionCast/board/elf-rk3588/camera_13855.md)**：13855 摄像头驱动加载、I2C 适配与点亮记录。
- 📄 **[board/elf-rk3588/debug_commands.md](file:///home/cl/EC-A3588Q/VisionCast/board/elf-rk3588/debug_commands.md)**：常用的音视频通路调试与录制/测试命令汇总。

---


##  零拷贝优化路线设计 (DMA-BUF)

为了在智能眼镜端达到极致的低延迟体验，系统在后续优化阶段将彻底弃用 CPU 拷贝：
```text
[ V4L2 Capture ] ---> ( DMA-BUF FD ) ---> [ RGA Processor ] ---> ( DMA-BUF FD ) ---> [ MPP Encoder ]
```
通过导出文件描述符（FD）在显存中直接进行格式转换与压缩编码，确保极佳的系统响应速率和功耗表现。
