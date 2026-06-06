#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace visioncast {

class NetworkSender {
public:
    virtual ~NetworkSender() = default;
    virtual bool open(std::string& error) = 0;
    virtual void close() = 0;
    virtual bool send(const std::uint8_t* data, std::size_t size, std::string& error) = 0;
};

}  // namespace visioncast
