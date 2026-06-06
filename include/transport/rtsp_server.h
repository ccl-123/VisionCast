#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace visioncast {

class RtspServer {
public:
    RtspServer(int port, std::string path);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    bool open(std::string& error);
    void close();
    bool push_video(const std::uint8_t* data,
                    std::size_t size,
                    std::uint64_t pts_us,
                    std::string& error);
    bool push_audio(const std::uint8_t* data,
                    std::size_t size,
                    std::uint64_t pts_us,
                    std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int port_;
    std::string path_;
};

}  // namespace visioncast
