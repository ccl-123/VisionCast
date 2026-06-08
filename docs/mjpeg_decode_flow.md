# USB MJPEG 解码链路说明

> 平台：ELF-RK3588
> 设备：USB C270，默认节点 `/dev/video21`
> 目标：说明 MJPEG 从 V4L2 压缩帧到 MPP 编码输入之间的实际路径、拷贝边界和性能日志字段。

## 1. 链路总览

USB C270 输出的是压缩 MJPEG，不是可直接送 H.264 编码器的 NV12。当前实际路径为：

```text
V4L2 DQBUF bytesused JPEG
  -> JPEG 边界校验和尾部裁剪
  -> MPP MJPEG 解码输入 buffer
  -> MPP 解码输出 NV12 DRM buffer / DMA-BUF fd
  -> RGA 直通或 fd-to-fd resize
  -> MPP H.264 编码 EXT_DMA 输入
  -> RTP / RTMP / WebRTC 封包发送
```

## 2. 输入处理

采集端只使用 `VIDIOC_DQBUF` 返回的 `bytesused` 作为 JPEG 实际长度，不使用 mmap buffer 总长度或 `sizeimage`。进入解码前会检查 SOI/EOI：

```text
SOI: ff d8
EOI: ff d9
```

如果尾部存在驱动填充字节，会裁剪到完整 JPEG 结束位置。无效 MJPEG 帧不会继续送 MPP，错误计入视频性能日志的 `处理失败`。

## 3. MPP 解码策略

USB 摄像头每次 DQBUF 已经是一张完整 JPEG，因此 MPP MJPEG 解码保持外部分帧模式：

```text
split_parse = 0
普通帧不设置 EOS
```

压缩 MJPEG 输入当前不走 `MPP_BUFFER_TYPE_EXT_DMA`。实测 C270/BSP 组合下，压缩输入 DMA-BUF 会间歇产生 `errinfo=256` 的无效 MPP 输出帧，所以该路径已删除。当前保留的是更稳定的 CPU 可见压缩包输入：JPEG payload 写入 MPP 输入 buffer；解码后的 NV12 仍保持 DMA-BUF 输出。

## 4. 解码输出与零拷贝边界

MPP 解码输出使用 DRM buffer，并通过 DMA-BUF fd 挂到 `VideoFrame::dma`。若解码尺寸已等于编码目标尺寸，则 RGA 直通：

```text
MPP decoded NV12 fd -> MPP encoder EXT_DMA
```

若需要缩放，则走 RGA fd-to-fd：

```text
MPP decoded NV12 fd -> RGA output DRM fd -> MPP encoder EXT_DMA
```

主路径不会把解码后的整帧 NV12 拷贝到 `std::vector`。仍然存在的拷贝包括：压缩 JPEG payload 写入 MPP 输入 buffer、编码后 H.264 拷贝到 `EncodedPacket::data`、协议封包拷贝。

## 5. 性能日志字段

视频日志中的 MJPEG 相关字段：

- `MJPEG(hw/sw/dma输出)`：窗口内 MJPEG 硬解帧数、软解帧数、解码输出为 DMA-BUF 的帧数。
- `耗时`：以 `MJPEG=...ms` 和 `图像处理=...ms` 分别显示单独解码耗时、解码之后的 RGA/CPU fallback/直通判断耗时。
- `前处理路径(fd/直通/拷贝)`：MJPEG 解码后如果尺寸匹配，通常显示为 `直通`；需要缩放时才显示 `fd`。
- `MPP输入(DMA/COPY)`：编码器是否使用 DMA-BUF 输入。USB MJPEG 主路径应为 `DMA`。

健康状态示例：

```text
MJPEG(hw/sw/dma输出)=30/0/30
前处理路径(fd/直通/拷贝)=0/30/0
MPP输入(DMA/COPY)=30/0
处理失败=0
```
