/**
 * @file webrtc_pusher.cpp
 * @brief VisionCast WebRTC WHIP 推流客户端模块实现文件
 * 
 * 本文件利用 libdatachannel C++ API (rtc/rtc.hpp)，实现了 WebRTC WHIP 推流客户端。
 * 流程包含：
 * 1. 本地网络 IP 绑定以进行 ICE 通信。
 * 2. 构造本地 SDP Offer，添加 H.264 视频轨道和 PCMA 音频轨道。
 * 3. 使用原生 Sockets 实现 HTTP POST 交换 SDP Offer/Answer，与 WHIP 服务端协商。
 * 4. 建立 DTLS 握手与 SRTP 加密信道，实现超低延迟的音视频传输。
 */

#include "transport/webrtc_pusher.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/log.h"

#if defined(VISIONCAST_ENABLE_WEBRTC)
#include "rtc/rtc.hpp"
#include "rtc/rtc.h"
#endif

namespace visioncast {
namespace {

constexpr std::uint32_t kVideoSsrc = 0x56435631U;
constexpr std::uint32_t kAudioSsrc = 0x56434131U;
constexpr int kVideoPayloadType = 96;
constexpr int kAudioPayloadType = 8;

struct ParsedHttpUrl {
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

#if defined(VISIONCAST_ENABLE_WEBRTC)
// 解析目标 Host 的本地网卡绑定 IP 地址，以确定 WebRTC 通信时绑定的本地网络接口
bool get_resolved_local_ip(const std::string& host, std::string& local_ip) {
    if (host == "localhost") {
        local_ip = "127.0.0.1";
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0) {
        return false;
    }
    
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        freeaddrinfo(results);
        return false;
    }
    
    bool found = false;
    for (addrinfo* r = results; r != nullptr && !found; r = r->ai_next) {
        for (ifaddrs* ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
            if (r->ai_addr->sa_family != ifa->ifa_addr->sa_family) continue;
            
            if (r->ai_addr->sa_family == AF_INET) {
                auto* sin_r = reinterpret_cast<sockaddr_in*>(r->ai_addr);
                auto* sin_ifa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
                if (sin_r->sin_addr.s_addr == sin_ifa->sin_addr.s_addr) {
                    char buf[INET_ADDRSTRLEN];
                    if (inet_ntop(AF_INET, &sin_r->sin_addr, buf, sizeof(buf))) {
                        local_ip = buf;
                        found = true;
                    }
                    break;
                }
            } else if (r->ai_addr->sa_family == AF_INET6) {
                auto* sin6_r = reinterpret_cast<sockaddr_in6*>(r->ai_addr);
                auto* sin6_ifa = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
                if (std::memcmp(&sin6_r->sin6_addr, &sin6_ifa->sin6_addr, sizeof(in6_addr)) == 0) {
                    char buf[INET6_ADDRSTRLEN];
                    if (inet_ntop(AF_INET6, &sin6_r->sin6_addr, buf, sizeof(buf))) {
                        local_ip = buf;
                        found = true;
                    }
                    break;
                }
            }
        }
    }
    
    freeifaddrs(interfaces);
    freeaddrinfo(results);
    return found;
}

bool parse_http_url(const std::string& url, ParsedHttpUrl& parsed, std::string& error) {
    constexpr const char* scheme = "http://";
    if (url.rfind(scheme, 0) != 0) {
        error = "WebRTC WHIP currently supports plain http:// URLs only: " + url;
        return false;
    }

    const std::size_t authority_start = std::strlen(scheme);
    const std::size_t path_start = url.find('/', authority_start);
    const std::string authority =
        url.substr(authority_start, path_start == std::string::npos
                                         ? std::string::npos
                                         : path_start - authority_start);
    if (authority.empty()) {
        error = "invalid WHIP URL host: " + url;
        return false;
    }
    parsed.path = path_start == std::string::npos ? "/" : url.substr(path_start);

    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos && colon + 1 < authority.size()) {
        parsed.host = authority.substr(0, colon);
        parsed.port = authority.substr(colon + 1);
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty() || parsed.port.empty()) {
        error = "invalid WHIP URL endpoint: " + url;
        return false;
    }
    return true;
}

std::string trim_cr(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

bool parse_http_response(const std::string& response, int& status, std::string& body, std::string& error) {
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        error = "WHIP response did not contain HTTP headers";
        return false;
    }

    std::istringstream headers(response.substr(0, header_end));
    std::string status_line;
    if (!std::getline(headers, status_line)) {
        error = "WHIP response status line is empty";
        return false;
    }
    status_line = trim_cr(status_line);
    std::istringstream status_stream(status_line);
    std::string version;
    status_stream >> version >> status;
    if (version.rfind("HTTP/", 0) != 0 || status <= 0) {
        error = "invalid WHIP HTTP status line: " + status_line;
        return false;
    }
    body = response.substr(header_end + 4);
    return true;
}

// 通过 HTTP POST 请求，将本地生成的 SDP Offer 发送到 WHIP 接收端，并获取 SDP Answer 响应
bool http_post_sdp(const std::string& url,
                   const std::string& offer_sdp,
                   std::string& answer_sdp,
                   std::string& error) {
    ParsedHttpUrl endpoint;
    if (!parse_http_url(url, endpoint, error)) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int gai = getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &results);
    if (gai != 0) {
        error = std::string("resolve WHIP host: ") + gai_strerror(gai);
        return false;
    }

    int fd = -1;
    for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);
    if (fd < 0) {
        error = "connect WHIP endpoint failed: " + endpoint.host + ":" + endpoint.port;
        return false;
    }

    std::ostringstream request;
    request << "POST " << endpoint.path << " HTTP/1.1\r\n"
            << "Host: " << endpoint.host << ":" << endpoint.port << "\r\n"
            << "Content-Type: application/sdp\r\n"
            << "Accept: application/sdp\r\n"
            << "Connection: close\r\n"
            << "Content-Length: " << offer_sdp.size() << "\r\n\r\n"
            << offer_sdp;
    const std::string bytes = request.str();

    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t ret = send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (ret <= 0) {
            error = "send WHIP offer failed";
            close(fd);
            return false;
        }
        sent += static_cast<std::size_t>(ret);
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        const ssize_t ret = recv(fd, buffer, sizeof(buffer), 0);
        if (ret < 0) {
            error = "receive WHIP answer failed";
            close(fd);
            return false;
        }
        if (ret == 0) {
            break;
        }
        response.append(buffer, buffer + ret);
    }
    close(fd);

    int status = 0;
    if (!parse_http_response(response, status, answer_sdp, error)) {
        return false;
    }
    if (status < 200 || status >= 300 || answer_sdp.empty()) {
        error = "WHIP server returned HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

std::string pc_state_text(rtc::PeerConnection::State state) {
    std::ostringstream out;
    out << state;
    return out.str();
}
#endif

}  // namespace

struct WebRtcPusher::Impl {
#if defined(VISIONCAST_ENABLE_WEBRTC)
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;
    std::mutex mutex;
    std::condition_variable cv;
    bool gathering_done = false;
    bool connected = false;
    bool failed = false;
    std::string local_sdp;
    std::string state_error;
#endif
    bool opened = false;
};

WebRtcPusher::WebRtcPusher(std::string whip_url, VideoConfig video, AudioConfig audio)
    : impl_(std::make_unique<Impl>()),
      whip_url_(std::move(whip_url)),
      video_(std::move(video)),
      audio_(std::move(audio)) {}

WebRtcPusher::~WebRtcPusher() {
    disconnect();
}

bool WebRtcPusher::connect(std::string& error) {
#if defined(VISIONCAST_ENABLE_WEBRTC)
    if (impl_->opened) {
        return true;
    }

    static bool logger_initialized = false;
    if (!logger_initialized) {
        rtcInitLogger(RTC_LOG_DEBUG, nullptr);
        logger_initialized = true;
    }

    try {
        rtc::Configuration config;
        config.disableAutoNegotiation = true;
        config.forceMediaTransport = true;

        ParsedHttpUrl endpoint;
        std::string parse_err;
        if (parse_http_url(whip_url_, endpoint, parse_err)) {
            std::string local_ip;
            if (get_resolved_local_ip(endpoint.host, local_ip)) {
                config.bindAddress = local_ip;
                VC_LOG_INFO("webrtc", "Local endpoint detected, binding WebRTC client to " + local_ip);
            }
        }

        // 在纯局域网环境下，不需要外网 STUN 服务器，避免因 DNS 解析超时导致启动挂起
        // config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        impl_->pc = std::make_shared<rtc::PeerConnection>(config);
        // 设置 PeerConnection 状态变化的监听器。
        // 当状态转为 Connected 时代表握手成功；如果转为 Failed 或 Closed 则视为失败。
        impl_->pc->onStateChange([this](rtc::PeerConnection::State state) {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (state == rtc::PeerConnection::State::Connected) {
                impl_->connected = true;
            } else if (state == rtc::PeerConnection::State::Failed ||
                       state == rtc::PeerConnection::State::Closed) {
                impl_->failed = true;
                impl_->state_error = "WebRTC PeerConnection state=" + pc_state_text(state);
            }
            impl_->cv.notify_all();
        });
        
        // 监听 ICE Candidate 收集状态变化，收集完成后读取并保存本地 SDP Offer
        impl_->pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete) {
                return;
            }
            std::lock_guard<std::mutex> lock(impl_->mutex);
            auto description = impl_->pc->localDescription();
            if (description) {
                impl_->local_sdp = std::string(description.value());
            }
            impl_->gathering_done = true;
            impl_->cv.notify_all();
        });

        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(kVideoPayloadType);
        video.addSSRC(kVideoSsrc, "video", "visioncast", "video");
        impl_->video_track = impl_->pc->addTrack(video);

        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addPCMACodec(kAudioPayloadType);
        audio.addSSRC(kAudioSsrc, "audio", "visioncast", "audio");
        impl_->audio_track = impl_->pc->addTrack(audio);

        impl_->pc->setLocalDescription(rtc::Description::Type::Offer);
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            const bool ready = impl_->cv.wait_for(lock, std::chrono::seconds(10), [this] {
                return impl_->gathering_done || impl_->failed;
            });
            if (!ready || impl_->local_sdp.empty()) {
                error = "WebRTC ICE gathering timed out before WHIP POST";
                disconnect();
                return false;
            }
            if (impl_->failed) {
                error = impl_->state_error;
                disconnect();
                return false;
            }
        }

        // 强制将客户端设置为 DTLS active 角色（主动发送 Client Hello），以防与某些 SFU/网关握手冲突
        std::string modified_offer = impl_->local_sdp;
        size_t pos = 0;
        while ((pos = modified_offer.find("a=setup:actpass", pos)) != std::string::npos) {
            modified_offer.replace(pos, 15, "a=setup:active");
            pos += 14;
        }

        std::string answer_sdp;
        if (!http_post_sdp(whip_url_, modified_offer, answer_sdp, error)) {
            disconnect();
            return false;
        }
        VC_LOG_INFO("webrtc", "Local SDP (Offer) [Modified to setup:active]:\n" + modified_offer);
        VC_LOG_INFO("webrtc", "Remote SDP (Answer):\n" + answer_sdp);
        impl_->pc->setRemoteDescription(rtc::Description(answer_sdp, "answer"));

        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            const bool finished = impl_->cv.wait_for(lock, std::chrono::seconds(10), [this] {
                return impl_->connected || impl_->failed;
            });
            if (impl_->failed) {
                error = impl_->state_error;
                disconnect();
                return false;
            }
            if (!finished || !impl_->connected) {
                error = "WebRTC connection timed out (ICE gathering/handshake failed)";
                disconnect();
                return false;
            }
        }

        impl_->opened = true;
        VC_LOG_INFO("webrtc", "WHIP session established: " + whip_url_);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("WebRTC connect failed: ") + ex.what();
        disconnect();
        return false;
    }
#else
    error = "WebRTC WHIP support is not enabled; build 3rdparty/webrtc first";
    return false;
#endif
}

void WebRtcPusher::disconnect() {
#if defined(VISIONCAST_ENABLE_WEBRTC)
    if (impl_->pc) {
        impl_->pc->close();
    }
    impl_->video_track.reset();
    impl_->audio_track.reset();
    impl_->pc.reset();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->gathering_done = false;
        impl_->connected = false;
        impl_->failed = false;
        impl_->local_sdp.clear();
        impl_->state_error.clear();
    }
#endif
    impl_->opened = false;
}

bool WebRtcPusher::push_video_rtp(const std::vector<RtpPacket>& packets, std::string& error) {
#if defined(VISIONCAST_ENABLE_WEBRTC)
    if (!impl_->opened || !impl_->video_track) {
        error = "WebRTC video track is not open";
        return false;
    }
    if (!impl_->video_track->isOpen()) {
        return true;
    }
    for (const auto& packet : packets) {
        if (!packet.bytes.empty() &&
            !impl_->video_track->send(reinterpret_cast<const rtc::byte*>(packet.bytes.data()),
                                      packet.bytes.size())) {
            // 打印警告日志但不中断，防止突发大包或网络瞬时拥堵拖垮整个推流管线
            static int warn_count = 0;
            if (warn_count++ % 100 == 0) {
                VC_LOG_WARN("webrtc", "WebRTC video RTP send buffer full or congested (packets dropped)");
            }
        }
    }
    return true;
#else
    (void)packets;
    error = "WebRTC WHIP support is not enabled";
    return false;
#endif
}

bool WebRtcPusher::push_audio_rtp(const std::vector<RtpPacket>& packets, std::string& error) {
#if defined(VISIONCAST_ENABLE_WEBRTC)
    if (!impl_->opened || !impl_->audio_track) {
        error = "WebRTC audio track is not open";
        return false;
    }
    if (!impl_->audio_track->isOpen()) {
        return true;
    }
    for (const auto& packet : packets) {
        if (!packet.bytes.empty() &&
            !impl_->audio_track->send(reinterpret_cast<const rtc::byte*>(packet.bytes.data()),
                                      packet.bytes.size())) {
            static int warn_count = 0;
            if (warn_count++ % 100 == 0) {
                VC_LOG_WARN("webrtc", "WebRTC audio RTP send buffer full or congested (packets dropped)");
            }
        }
    }
    return true;
#else
    (void)packets;
    error = "WebRTC WHIP support is not enabled";
    return false;
#endif
}

}  // namespace visioncast
