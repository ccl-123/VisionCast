#include "media/video_capture.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "common/log.h"
#include "media/media_clock.h"

namespace visioncast {
namespace {

struct MmapBuffer {
    struct Plane {
        void* start = nullptr;
        std::size_t length = 0;
    };
    std::vector<Plane> planes;
};

std::string char_array_to_string(const unsigned char* data, std::size_t size) {
    return std::string(reinterpret_cast<const char*>(data),
                       strnlen(reinterpret_cast<const char*>(data), size));
}

std::string fourcc_to_string(std::uint32_t format) {
    std::string value;
    value.push_back(static_cast<char>(format & 0xFF));
    value.push_back(static_cast<char>((format >> 8) & 0xFF));
    value.push_back(static_cast<char>((format >> 16) & 0xFF));
    value.push_back(static_cast<char>((format >> 24) & 0xFF));
    return value;
}

std::uint32_t fourcc_from_string(const std::string& format) {
    if (format == "NV12") {
        return V4L2_PIX_FMT_NV12;
    }
    if (format == "YUYV" || format == "YUY2") {
        return V4L2_PIX_FMT_YUYV;
    }
    if (format == "MJPEG" || format == "MJPG") {
        return V4L2_PIX_FMT_MJPEG;
    }
    return 0;
}

std::string errno_text(const std::string& prefix) {
    std::ostringstream out;
    out << prefix << ": " << std::strerror(errno);
    return out.str();
}

bool xioctl(int fd, unsigned long request, void* arg, const std::string& name, std::string& error) {
    for (;;) {
        if (ioctl(fd, request, arg) == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        error = errno_text(name);
        return false;
    }
}

void enumerate_formats(int fd,
                       v4l2_buf_type type,
                       const std::string& type_name,
                       std::vector<VideoFormatInfo>& formats) {
    for (std::uint32_t index = 0;; ++index) {
        v4l2_fmtdesc desc{};
        desc.index = index;
        desc.type = type;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
            if (errno == EINVAL) {
                break;
            }
            break;
        }

        VideoFormatInfo info;
        info.fourcc = fourcc_to_string(desc.pixelformat);
        info.description = char_array_to_string(desc.description, sizeof(desc.description));
        info.buffer_type = type_name;
        formats.push_back(info);
    }
}

void unmap_buffers(std::vector<MmapBuffer>& buffers) {
    for (auto& buffer : buffers) {
        for (auto& plane : buffer.planes) {
            if (plane.start != nullptr && plane.start != MAP_FAILED) {
                munmap(plane.start, plane.length);
            }
        }
    }
    buffers.clear();
}

bool is_mplane_type(v4l2_buf_type type) {
    return type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
}

void configure_sensor_frame_rate(const VideoConfig& config,
                                 const std::string& active_device) {
    if (config.sensor_subdev.empty() || active_device != config.device) {
        return;
    }

    const int sensor_fd =
        open(config.sensor_subdev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (sensor_fd < 0) {
        VC_LOG_WARN("video-capture",
                    errno_text("open sensor subdevice " + config.sensor_subdev));
        return;
    }

    const auto set_control = [sensor_fd, &config](std::uint32_t id,
                                                  int value,
                                                  const char* name) {
        if (value < 0) {
            return true;
        }
        v4l2_control control{};
        control.id = id;
        control.value = value;
        if (ioctl(sensor_fd, VIDIOC_S_CTRL, &control) == 0) {
            return true;
        }
        VC_LOG_WARN("video-capture",
                    errno_text(std::string("set ") + name + " on " +
                               config.sensor_subdev));
        return false;
    };

    const bool exposure_ok =
        config.sensor_exposure <= 0 ||
        set_control(V4L2_CID_EXPOSURE, config.sensor_exposure, "exposure");
    const bool vblank_ok =
        set_control(V4L2_CID_VBLANK, config.sensor_vblank, "vertical_blanking");
    if (exposure_ok && vblank_ok) {
        VC_LOG_INFO("video-capture",
                    "sensor fixed-rate controls applied: exposure=" +
                        std::to_string(config.sensor_exposure) +
                        " vblank=" + std::to_string(config.sensor_vblank));
    }
    close(sensor_fd);
}

std::string canonical_frame_format(std::uint32_t fourcc) {
    if (fourcc == V4L2_PIX_FMT_MJPEG || fourcc == V4L2_PIX_FMT_JPEG) {
        return "MJPEG";
    }
    if (fourcc == V4L2_PIX_FMT_NV12 || fourcc == V4L2_PIX_FMT_NV12M) {
        return "NV12";
    }
    return fourcc_to_string(fourcc);
}

}  // namespace

VideoCapture::VideoCapture(VideoConfig config)
    : config_(std::move(config)) {}

VideoCapture::~VideoCapture() {
    stop();
}

VideoProbeResult VideoCapture::probe() const {
    return probe_device(config_.device);
}

VideoProbeResult VideoCapture::probe_device(const std::string& device) {
    VideoProbeResult result;
    result.device = device;

    if (device.empty()) {
        result.error = "video device path is empty";
        return result;
    }

    int fd = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        result.error = errno_text("open " + device);
        return result;
    }

    v4l2_capability cap{};
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        result.error = errno_text("VIDIOC_QUERYCAP " + device);
        close(fd);
        return result;
    }

    result.available = true;
    result.driver = char_array_to_string(cap.driver, sizeof(cap.driver));
    result.card = char_array_to_string(cap.card, sizeof(cap.card));
    result.bus_info = char_array_to_string(cap.bus_info, sizeof(cap.bus_info));
    result.capabilities = cap.capabilities;
    result.device_caps = cap.device_caps;

    const std::uint32_t caps = cap.device_caps != 0 ? cap.device_caps : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) != 0) {
        enumerate_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "capture", result.formats);
    }
    if ((caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0) {
        enumerate_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "capture_mplane", result.formats);
    }

    close(fd);
    return result;
}

bool VideoCapture::start(FrameCallback callback) {
    if (running_) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        callback_ = std::move(callback);
        error_.clear();
        active_device_.clear();
        initialization_done_ = false;
        initialization_success_ = false;
    }
    running_ = true;
    thread_ = std::thread(&VideoCapture::capture_loop, this);

    std::unique_lock<std::mutex> lock(state_mutex_);
    const bool signaled = state_cv_.wait_for(lock, std::chrono::seconds(8), [this] {
        return initialization_done_;
    });
    const bool success = signaled && initialization_success_;
    if (!signaled) {
        error_ = "video capture initialization timed out";
    }
    lock.unlock();
    if (!success) {
        stop();
    }
    return success;
}

void VideoCapture::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool VideoCapture::is_running() const {
    return running_.load();
}

std::string VideoCapture::active_device() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return active_device_;
}

std::string VideoCapture::error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return error_;
}

void VideoCapture::finish_initialization(bool success, const std::string& error) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (initialization_done_) {
        return;
    }
    initialization_done_ = true;
    initialization_success_ = success;
    if (success) {
        error_.clear();
    } else if (!error.empty()) {
        error_ = error;
    }
    state_cv_.notify_all();
}

void VideoCapture::capture_loop() {
    const std::vector<std::pair<std::string, std::string>> candidates = {
        {config_.device, config_.format},
        {config_.fallback_device, config_.fallback_format},
    };
    std::set<std::pair<std::string, std::string>> attempted;

    for (const auto& candidate : candidates) {
        const auto& device = candidate.first;
        const auto& requested_format = candidate.second;
        if (!running_ || device.empty() || !attempted.insert(candidate).second) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            active_device_ = device;
        }
        int fd = open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            const std::string open_error = errno_text("open " + device);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                error_ = open_error;
            }
            VC_LOG_WARN("video-capture", open_error);
            continue;
        }

        std::string error;
        v4l2_capability cap{};
        if (!xioctl(fd, VIDIOC_QUERYCAP, &cap, "VIDIOC_QUERYCAP " + device, error)) {
            VC_LOG_WARN("video-capture", error);
            close(fd);
            continue;
        }
        const std::uint32_t caps = cap.device_caps != 0 ? cap.device_caps : cap.capabilities;
        v4l2_buf_type type;
        if ((caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0) {
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        } else if ((caps & V4L2_CAP_VIDEO_CAPTURE) != 0) {
            type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        } else {
            VC_LOG_WARN("video-capture", device + " does not support capture streaming");
            close(fd);
            continue;
        }
        if ((caps & V4L2_CAP_STREAMING) == 0) {
            VC_LOG_WARN("video-capture", device + " does not support capture streaming");
            close(fd);
            continue;
        }

        const std::uint32_t requested_fourcc = fourcc_from_string(requested_format);
        if (requested_fourcc == 0) {
            VC_LOG_WARN("video-capture", "unsupported configured format: " + requested_format);
            close(fd);
            continue;
        }

        v4l2_format fmt{};
        fmt.type = type;
        if (is_mplane_type(type)) {
            fmt.fmt.pix_mp.width = static_cast<std::uint32_t>(config_.width);
            fmt.fmt.pix_mp.height = static_cast<std::uint32_t>(config_.height);
            fmt.fmt.pix_mp.pixelformat = requested_fourcc;
            fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        } else {
            fmt.fmt.pix.width = static_cast<std::uint32_t>(config_.width);
            fmt.fmt.pix.height = static_cast<std::uint32_t>(config_.height);
            fmt.fmt.pix.pixelformat = requested_fourcc;
            fmt.fmt.pix.field = V4L2_FIELD_NONE;
        }
        if (!xioctl(fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT " + device, error)) {
            VC_LOG_WARN("video-capture", error);
            close(fd);
            continue;
        }
        const std::uint32_t active_fourcc =
            is_mplane_type(type) ? fmt.fmt.pix_mp.pixelformat : fmt.fmt.pix.pixelformat;
        if (active_fourcc != requested_fourcc) {
            VC_LOG_WARN("video-capture",
                        device + " negotiated " + fourcc_to_string(active_fourcc) +
                            " instead of requested " + fourcc_to_string(requested_fourcc));
            close(fd);
            continue;
        }

        v4l2_streamparm parm{};
        parm.type = type;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<std::uint32_t>(config_.fps);
        if (ioctl(fd, VIDIOC_S_PARM, &parm) != 0) {
            VC_LOG_WARN("video-capture", errno_text("VIDIOC_S_PARM " + device));
        }

        v4l2_requestbuffers req{};
        req.count = 4;
        req.type = type;
        req.memory = V4L2_MEMORY_MMAP;
        if (!xioctl(fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS " + device, error) || req.count < 2) {
            VC_LOG_WARN("video-capture", error.empty() ? "insufficient V4L2 buffers" : error);
            close(fd);
            continue;
        }

        std::vector<MmapBuffer> buffers(req.count);
        bool map_ok = true;
        for (std::uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            v4l2_plane planes[VIDEO_MAX_PLANES]{};
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (is_mplane_type(type)) {
                buf.m.planes = planes;
                buf.length = VIDEO_MAX_PLANES;
            }
            if (!xioctl(fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF " + device, error)) {
                VC_LOG_WARN("video-capture", error);
                map_ok = false;
                break;
            }
            const std::uint32_t plane_count = is_mplane_type(type) ? buf.length : 1U;
            buffers[i].planes.resize(plane_count);
            for (std::uint32_t p = 0; p < plane_count; ++p) {
                const std::size_t length = is_mplane_type(type) ? planes[p].length : buf.length;
                const off_t offset = static_cast<off_t>(
                    is_mplane_type(type) ? planes[p].m.mem_offset : buf.m.offset);
                buffers[i].planes[p].length = length;
                buffers[i].planes[p].start =
                    mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
                if (buffers[i].planes[p].start == MAP_FAILED) {
                    VC_LOG_WARN("video-capture", errno_text("mmap video buffer"));
                    map_ok = false;
                    break;
                }
            }
            if (!map_ok) {
                break;
            }
        }
        if (!map_ok) {
            unmap_buffers(buffers);
            close(fd);
            continue;
        }

        for (std::uint32_t i = 0; i < buffers.size(); ++i) {
            v4l2_buffer buf{};
            v4l2_plane planes[VIDEO_MAX_PLANES]{};
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (is_mplane_type(type)) {
                buf.m.planes = planes;
                buf.length = static_cast<std::uint32_t>(buffers[i].planes.size());
            }
            if (!xioctl(fd, VIDIOC_QBUF, &buf, "VIDIOC_QBUF " + device, error)) {
                VC_LOG_WARN("video-capture", error);
                map_ok = false;
                break;
            }
        }
        if (!map_ok) {
            unmap_buffers(buffers);
            close(fd);
            continue;
        }

        if (!xioctl(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON " + device, error)) {
            VC_LOG_WARN("video-capture", error);
            unmap_buffers(buffers);
            close(fd);
            continue;
        }
        configure_sensor_frame_rate(config_, device);

        const int active_width = static_cast<int>(
            is_mplane_type(type) ? fmt.fmt.pix_mp.width : fmt.fmt.pix.width);
        const int active_height = static_cast<int>(
            is_mplane_type(type) ? fmt.fmt.pix_mp.height : fmt.fmt.pix.height);
        const int active_stride = static_cast<int>(
            is_mplane_type(type) ? fmt.fmt.pix_mp.plane_fmt[0].bytesperline
                                 : fmt.fmt.pix.bytesperline);
        VC_LOG_INFO("video-capture", "capturing from " + device + " type=" +
                                         (is_mplane_type(type) ? "mplane" : "single") +
                                         " fmt=" + fourcc_to_string(active_fourcc));
        finish_initialization(true);
        std::uint64_t sequence = 0;
        const auto set_runtime_error = [this](const std::string& message) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            error_ = message;
        };
        while (running_) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            const int poll_ret = poll(&pfd, 1, 500);
            if (poll_ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const std::string runtime_error = errno_text("poll video");
                set_runtime_error(runtime_error);
                VC_LOG_ERROR("video-capture", runtime_error);
                break;
            }
            if (poll_ret == 0) {
                continue;
            }

            v4l2_buffer buf{};
            v4l2_plane planes[VIDEO_MAX_PLANES]{};
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            if (is_mplane_type(type)) {
                buf.m.planes = planes;
                buf.length = VIDEO_MAX_PLANES;
            }
            if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                const std::string runtime_error = errno_text("VIDIOC_DQBUF");
                set_runtime_error(runtime_error);
                VC_LOG_ERROR("video-capture", runtime_error);
                break;
            }

            VideoFrame frame;
            frame.pts_us = MediaClock::now_us();
            frame.width = active_width;
            frame.height = active_height;
            frame.stride = active_stride;
            frame.vertical_stride = active_height;
            frame.format = canonical_frame_format(active_fourcc);
            frame.sequence = sequence++;
            if (buf.index >= buffers.size()) {
                const std::string runtime_error =
                    "driver returned invalid video buffer index";
                set_runtime_error(runtime_error);
                VC_LOG_ERROR("video-capture", runtime_error);
                break;
            }
            const std::uint32_t plane_count =
                is_mplane_type(type) ? std::min<std::uint32_t>(
                                          buf.length,
                                          static_cast<std::uint32_t>(buffers[buf.index].planes.size()))
                                     : 1U;
            for (std::uint32_t p = 0; p < plane_count; ++p) {
                const auto& mapped = buffers[buf.index].planes[p];
                const std::size_t data_offset = is_mplane_type(type) ? planes[p].data_offset : 0U;
                const std::size_t bytesused = is_mplane_type(type) ? planes[p].bytesused
                                                                   : buf.bytesused;
                if (data_offset > mapped.length) {
                    continue;
                }
                const std::size_t used =
                    std::min(bytesused, mapped.length) > data_offset
                        ? std::min(bytesused, mapped.length) - data_offset
                        : 0U;
                const auto* ptr = static_cast<const std::uint8_t*>(mapped.start) + data_offset;
                frame.data.insert(frame.data.end(), ptr, ptr + used);
            }
            if (callback_) {
                callback_(std::move(frame));
            }

            if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
                const std::string runtime_error = errno_text("VIDIOC_QBUF");
                set_runtime_error(runtime_error);
                VC_LOG_ERROR("video-capture", runtime_error);
                break;
            }
        }

        if (ioctl(fd, VIDIOC_STREAMOFF, &type) != 0) {
            VC_LOG_WARN("video-capture", errno_text("VIDIOC_STREAMOFF"));
        }
        unmap_buffers(buffers);
        close(fd);
        if (!running_) {
            break;
        }
    }

    const std::string final_error = error();
    finish_initialization(
        false,
        final_error.empty() ? "all configured video devices failed" : final_error);
    running_ = false;
}

}  // namespace visioncast
