#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "transport/network_sender.h"

namespace visioncast {

class UdpSender : public NetworkSender {
public:
    UdpSender(std::string ip, int port);
    ~UdpSender() override;

    bool open(std::string& error) override;
    void close() override;
    bool send(const std::uint8_t* data, std::size_t size, std::string& error) override;

private:
    std::string ip_;
    int port_ = 0;
    int fd_ = -1;
};

}  // namespace visioncast
