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

    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        error = std::string("socket(AF_INET, SOCK_DGRAM): ") + std::strerror(errno);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port_));
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
        error = "invalid IPv4 address: " + ip_;
        close();
        return false;
    }

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
    if (fd_ < 0 && !open(error)) {
        return false;
    }
    const ssize_t sent = ::send(fd_, data, size, MSG_NOSIGNAL);
    if (sent < 0) {
        error = std::string("UDP send: ") + std::strerror(errno);
        return false;
    }
    if (static_cast<std::size_t>(sent) != size) {
        std::ostringstream out;
        out << "short UDP send: " << sent << "/" << size;
        error = out.str();
        return false;
    }
    return true;
}

}  // namespace visioncast
