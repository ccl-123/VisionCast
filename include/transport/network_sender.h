/**
 * @file network_sender.h
 * @brief VisionCast 网络发送器抽象基类头文件
 * 
 * 本文件定义了 NetworkSender 接口类。
 * 该接口定义了通用网络传输发送器的基本行为（打开连接、关闭连接、发送数据），
 * 用于解耦底层网络协议（如 UDP、TCP 等）的具体实现。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace visioncast {

/**
 * @class NetworkSender
 * @brief 网络发送器抽象基类，定义了统一的数据发送接口
 */
class NetworkSender {
public:
    /**
     * @brief 虚析构函数，确保派生类对象能被正确销毁
     */
    virtual ~NetworkSender() = default;

    /**
     * @brief 打开网络传输通道/连接
     * @param error 传入传出参数，用于返回出错时的详细错误信息
     * @return 成功返回 true，失败返回 false
     */
    virtual bool open(std::string& error) = 0;

    /**
     * @brief 关闭网络传输通道/连接，释放相关网络资源
     */
    virtual void close() = 0;

    /**
     * @brief 发送指定长度的二进制数据
     * @param data 指向待发送数据缓冲区的指针
     * @param size 待发送数据的字节数
     * @param error 传入传出参数，用于返回出错时的详细错误信息
     * @return 发送成功返回 true，失败返回 false
     */
    virtual bool send(const std::uint8_t* data, std::size_t size, std::string& error) = 0;
};

}  // namespace visioncast

