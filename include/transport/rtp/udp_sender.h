/**
 * @file udp_sender.h
 * @brief VisionCast UDP 网络发送器类头文件
 * 
 * 本文件定义了 UdpSender 类，它继承自 NetworkSender。
 * 通过底层的 BSD Sockets 接口提供 UDP 单播或广播方式的数据报发送功能，
 * 主要用于承载 RTP 封装后的音视频流。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "transport/rtp/network_sender.h"

namespace visioncast {

/**
 * @class UdpSender
 * @brief 基于 UDP 协议的网络数据发送器实现类
 */
class UdpSender : public NetworkSender {
public:
    /**
     * @brief 构造函数，设定目标 IP 地址和端口号
     * @param ip 目标主机的 IP 地址（例如 "192.168.1.100"）
     * @param port 目标端口号（例如 8554）
     */
    UdpSender(std::string ip, int port);

    /**
     * @brief 析构函数，自动关闭套接字并释放系统资源
     */
    ~UdpSender() override;

    /**
     * @brief 创建 UDP 套接字，并配置目标地址结构体
     * @param error 传入传出参数，用于返回套接字创建/配置失败的详细原因
     * @return 成功打开返回 true，失败返回 false
     */
    bool open(std::string& error) override;

    /**
     * @brief 关闭 UDP 套接字并重置文件描述符状态
     */
    void close() override;

    /**
     * @brief 发送 UDP 数据报文
     * @param data 待发送的二进制数据指针
     * @param size 数据字节数
     * @param error 传入传出参数，用于返回发送失败时的错误描述
     * @return 发送成功返回 true，失败返回 false
     */
    bool send(const std::uint8_t* data, std::size_t size, std::string& error) override;

private:
    std::string ip_;      ///< 目标 IP 地址
    int port_ = 0;        ///< 目标端口号
    int fd_ = -1;         ///< UDP 套接字文件描述符 (Socket File Descriptor)
};

}  // namespace visioncast

