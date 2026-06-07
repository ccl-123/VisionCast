/**
 * @file udp_sender.cpp
 * @brief VisionCast UDP 网络发送器实现文件
 * 
 * 本文件利用 Linux BSD Sockets 接口，实现了基于已连接套接字 (Connected UDP Socket) 方式的
 * 数据发送。主要特点为：
 * 1. 采用 connect 关联目标 IP 和端口，以便能直接调用 send，并减少内核层面的寻址校验开销。
 * 2. 使用 MSG_NOSIGNAL 标志避免发送异常时进程收到 SIGPIPE 信号导致崩溃。
 * 3. 忽略 ECONNREFUSED 错误，以确保对端尚未监听时发送不会意外断开。
 */

#include "transport/udp_sender.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <utility>

namespace visioncast {

UdpSender::UdpSender(std::string ip, int port)
    : ip_(std::move(ip)), port_(port) {}

UdpSender::~UdpSender() {
    close();
}

bool UdpSender::open(std::string& error) {
    if (fd_ >= 0) {
        return true;
    }
    if (ip_.empty() || port_ <= 0 || port_ > 65535) {
        error = "invalid UDP target " + ip_ + ":" + std::to_string(port_);
        return false;
    }

    // 创建 UDP 套接字，并通过 SOCK_CLOEXEC 确保在执行 exec 系统调用时自动关闭，避免 FD 泄露
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        error = std::string("socket(AF_INET, SOCK_DGRAM): ") + std::strerror(errno);
        return false;
    }

    // 初始化 IPv4 地址结构体
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port_));
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
        error = "invalid IPv4 address: " + ip_;
        close();
        return false;
    }

    // 调用 connect 绑定目标地址。这在 UDP 中并非建立 TCP 连接，
    // 而是指定了默认发送目的地，进而可使用 send() 发送数据，并且丢弃来自其他 IP 的包。
    if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::string("connect UDP ") + ip_ + ":" + std::to_string(port_) +
                ": " + std::strerror(errno);
        close();
        return false;
    }
    return true;
}

void UdpSender::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UdpSender::send(const std::uint8_t* data, std::size_t size, std::string& error) {
    // 惰性初始化：如果套接字尚未打开，自动调用 open 建立套接字
    if (fd_ < 0 && !open(error)) {
        return false;
    }
    
    // 发送数据报文。使用 MSG_NOSIGNAL 防止产生 SIGPIPE 信号
    const ssize_t sent = ::send(fd_, data, size, MSG_NOSIGNAL);
    if (sent < 0) {
        // 如果接收方尚未启动或端口未监听，某些操作系统可能会通过 ICMP 返回端口不可达
        // 表现为 ECONNREFUSED 错误。对于 UDP，此处通常选择忽略该错误，继续后续报文的发送。
        if (errno == ECONNREFUSED) {
            return true;
        }
        error = std::string("UDP send: ") + std::strerror(errno);
        return false;
    }
    
    // UDP 为整包发送，如果发送成功的字节数不等于预期值，通常代表网络发送缓冲区满或其他异常情况
    if (static_cast<std::size_t>(sent) != size) {
        std::ostringstream out;
        out << "short UDP send: " << sent << "/" << size;
        error = out.str();
        return false;
    }
    return true;
}

}  // namespace visioncast

