#include "transport/rtsp_server.h"

#include <limits>
#include <utility>

#if defined(VISIONCAST_ENABLE_RTSP)
#include "rtsp_demo.h"
#endif

namespace visioncast {

struct RtspServer::Impl {
#if defined(VISIONCAST_ENABLE_RTSP)
    rtsp_demo_handle demo = nullptr;
    rtsp_session_handle session = nullptr;
#endif
    bool opened = false;
};

RtspServer::RtspServer(int port, std::string path)
    : impl_(std::make_unique<Impl>()), port_(port), path_(std::move(path)) {}

RtspServer::~RtspServer() {
    close();
}

bool RtspServer::open(std::string& error) {
#if defined(VISIONCAST_ENABLE_RTSP)
    if (impl_->opened) {
        return true;
    }
    if (port_ <= 0 || port_ > 65535 || path_.empty() || path_.front() != '/') {
        error = "invalid RTSP listen endpoint";
        return false;
    }
    impl_->demo = create_rtsp_demo(port_);
    if (impl_->demo == nullptr) {
        error = "create_rtsp_demo failed";
        return false;
    }
    impl_->session = rtsp_new_session(impl_->demo, path_.c_str());
    if (impl_->session == nullptr) {
        error = "rtsp_new_session failed";
        close();
        return false;
    }
    if (rtsp_set_video(impl_->session, RTSP_CODEC_ID_VIDEO_H264, nullptr, 0) != 0 ||
        rtsp_set_audio(impl_->session, RTSP_CODEC_ID_AUDIO_G711A, nullptr, 0) != 0 ||
        rtsp_set_audio_sample_rate(impl_->session, 8000) != 0 ||
        rtsp_set_audio_channels(impl_->session, 1) != 0 ||
        rtsp_sync_video_ts(
            impl_->session, rtsp_get_reltime(), rtsp_get_ntptime()) != 0 ||
        rtsp_sync_audio_ts(
            impl_->session, rtsp_get_reltime(), rtsp_get_ntptime()) != 0) {
        error = "failed to configure RTSP H264/G711A session";
        close();
        return false;
    }
    impl_->opened = true;
    return true;
#else
    error = "RTSP server support is not enabled in this build";
    return false;
#endif
}

void RtspServer::close() {
#if defined(VISIONCAST_ENABLE_RTSP)
    if (impl_->session != nullptr) {
        rtsp_del_session(impl_->session);
        impl_->session = nullptr;
    }
    if (impl_->demo != nullptr) {
        rtsp_del_demo(impl_->demo);
        impl_->demo = nullptr;
    }
#endif
    impl_->opened = false;
}

bool RtspServer::push_video(const std::uint8_t* data,
                            std::size_t size,
                            std::uint64_t pts_us,
                            std::string& error) {
#if defined(VISIONCAST_ENABLE_RTSP)
    if (!impl_->opened || data == nullptr || size == 0 ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "invalid RTSP video frame";
        return false;
    }
    if (rtsp_tx_video(
            impl_->session, data, static_cast<int>(size), pts_us) != 0 ||
        rtsp_do_event(impl_->demo) != 0) {
        error = "RTSP video send failed";
        return false;
    }
    return true;
#else
    (void)data;
    (void)size;
    (void)pts_us;
    error = "RTSP server support is not enabled in this build";
    return false;
#endif
}

bool RtspServer::push_audio(const std::uint8_t* data,
                            std::size_t size,
                            std::uint64_t pts_us,
                            std::string& error) {
#if defined(VISIONCAST_ENABLE_RTSP)
    if (!impl_->opened || data == nullptr || size == 0 ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "invalid RTSP audio frame";
        return false;
    }
    if (rtsp_tx_audio(
            impl_->session, data, static_cast<int>(size), pts_us) != 0 ||
        rtsp_do_event(impl_->demo) != 0) {
        error = "RTSP audio send failed";
        return false;
    }
    return true;
#else
    (void)data;
    (void)size;
    (void)pts_us;
    error = "RTSP server support is not enabled in this build";
    return false;
#endif
}

}  // namespace visioncast
