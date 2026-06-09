#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file patch_whip.py
@brief FFmpeg libavformat/whip.c 自动补丁工具

本脚本在板端或主机编译 FFmpeg 期间，由 build_ffmpeg.sh 构建脚本解压源码后自动调用。
其核心作用是改写 FFmpeg whip muxer 默认的单候选地址 (ICE candidate) 选择逻辑。

【背景与问题】：
原生 FFmpeg 8.x whip Muxer (whip.c) 仅支持单候选地址，且在解析 SDP Answer 时盲目挑选遇到的
第一个 UDP 'host' candidate。当流媒体服务器所在主机存在 VPN 虚拟网卡（例如 Clash TUN 虚拟网卡
分配的 198.18.0.1）、Docker 虚拟网卡等多个接口时，排在首位的往往是板端完全不可达的私有地址，
导致后续 DTLS 握手包发送失败，连接挂起并最终超时。

【解决办法】：
本脚本利用字符串替换，将 whip.c 中的 candidate 解析循环进行重构。在循环解析 candidate 时，
动态调用 av_url_split 提取当前推流 WHIP 接口 URL 中的宿主机 IP 地址，并与 candidate 声明的 IP 进行比对。
若 IP 一致则优先保存，若不一致则保留第一个备用。从而在不改变 FFmpeg 整体架构的前提下，优雅解决了
多网卡/代理环境下的 WebRTC 建连故障。
"""

import sys
import os

def main():
    # 1. 检查命令行参数，确保传入了 FFmpeg 源码根目录
    if len(sys.argv) < 2:
        print("Usage: patch_whip.py <FFmpeg-source-directory>")
        sys.exit(1)

    # 2. 定位 whip.c 文件的路径
    whip_c_path = os.path.join(sys.argv[1], "libavformat", "whip.c")
    if not os.path.exists(whip_c_path):
        print(f"Error: {whip_c_path} does not exist")
        sys.exit(1)

    # 3. 读取源码内容
    with open(whip_c_path, "r", encoding="utf-8") as f:
        content = f.read()

    # 4. 原生 FFmpeg 8.1.1 匹配的候选地址解析代码块 (遇到第一个就跳出)
    target = """        } else if (av_strstart(line, "a=candidate:", &ptr) && !whip->ice_protocol) {
            if (ptr && av_stristr(ptr, "host")) {
                /* Refer to RFC 5245 15.1 */
                char foundation[33], protocol[17], host[129];
                int component_id, priority, port;
                ret = sscanf(ptr, "%32s %d %16s %d %128s %d typ host", foundation, &component_id, protocol, &priority, host, &port);
                if (ret != 6) {
                    av_log(whip, AV_LOG_ERROR, "Failed %d to parse line %d %s from %s\\n",
                        ret, i, line, whip->sdp_answer);
                    ret = AVERROR(EIO);
                    goto end;
                }

                if (av_strcasecmp(protocol, "udp")) {
                    av_log(whip, AV_LOG_ERROR, "Protocol %s is not supported by RTC, choose udp, line %d %s of %s\\n",
                        protocol, i, line, whip->sdp_answer);
                    ret = AVERROR(EIO);
                    goto end;
                }

                whip->ice_protocol = av_strdup(protocol);
                whip->ice_host = av_strdup(host);
                whip->ice_port = port;
                if (!whip->ice_protocol || !whip->ice_host) {
                    ret = AVERROR(ENOMEM);
                    goto end;
                }
            }
        }"""

    # 5. 替换后的高兼容性候选地址解析代码块 (优先挑选与 WHIP URL 宿主机 IP 相同的 candidate)
    replacement = """        } else if (av_strstart(line, "a=candidate:", &ptr)) {
            if (ptr && av_stristr(ptr, "host")) {
                /* Refer to RFC 5245 15.1 */
                char foundation[33], protocol[17], host[129];
                int component_id, priority, port;
                ret = sscanf(ptr, "%32s %d %16s %d %128s %d typ host", foundation, &component_id, protocol, &priority, host, &port);
                if (ret == 6 && !av_strcasecmp(protocol, "udp")) {
                    char whip_host[256] = {0};
                    // 动态从当前推流 WHIP 接口 URL (s->url) 中解析目标主机 IP
                    av_url_split(NULL, 0, NULL, 0, whip_host, sizeof(whip_host), NULL, NULL, 0, s->url);
                    // 若尚未设置默认候选地址，或者当前候选地址 IP 与 WHIP URL 宿主机 IP 完全匹配，则进行保存/覆盖
                    if (!whip->ice_protocol || !strcmp(host, whip_host)) {
                        av_freep(&whip->ice_protocol);
                        av_freep(&whip->ice_host);
                        whip->ice_protocol = av_strdup(protocol);
                        whip->ice_host = av_strdup(host);
                        whip->ice_port = port;
                        if (!whip->ice_protocol || !whip->ice_host) {
                            ret = AVERROR(ENOMEM);
                            goto end;
                        }
                    }
                }
            }
        }"""

    # 6. 执行安全替换
    if target in content:
        content = content.replace(target, replacement)
        with open(whip_c_path, "w", encoding="utf-8") as f:
            f.write(content)
        print("Successfully patched whip.c to prefer matching WHIP URL candidate")
    else:
        print("Error: Target candidate parsing block not found in whip.c")
        sys.exit(1)

if __name__ == "__main__":
    main()
