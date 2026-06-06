#include "media/display_renderer.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "common/log.h"

namespace visioncast {
namespace {

std::uint8_t clamp_u8(int value) {
    return static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
}

void nv12_to_rgb(const std::uint8_t* y_plane,
                 const std::uint8_t* uv_plane,
                 int stride,
                 int x,
                 int y,
                 std::uint8_t& r,
                 std::uint8_t& g,
                 std::uint8_t& b) {
    const int y_value = static_cast<int>(y_plane[static_cast<std::size_t>(y) * stride + x]);
    const int uv_index = static_cast<int>((static_cast<std::size_t>(y / 2) * stride + (x & ~1)));
    const int u = static_cast<int>(uv_plane[uv_index]) - 128;
    const int v = static_cast<int>(uv_plane[uv_index + 1]) - 128;
    const int c = y_value - 16;
    r = clamp_u8((298 * c + 409 * v + 128) >> 8);
    g = clamp_u8((298 * c - 100 * u - 208 * v + 128) >> 8);
    b = clamp_u8((298 * c + 516 * u + 128) >> 8);
}

void write_pixel(std::uint8_t* dst,
                 const fb_var_screeninfo& var,
                 std::uint8_t r,
                 std::uint8_t g,
                 std::uint8_t b) {
    if (var.bits_per_pixel == 32) {
        dst[var.red.offset / 8] = r;
        dst[var.green.offset / 8] = g;
        dst[var.blue.offset / 8] = b;
        if (var.transp.length > 0) {
            dst[var.transp.offset / 8] = 0xFF;
        }
        return;
    }
    if (var.bits_per_pixel == 24) {
        dst[var.red.offset / 8] = r;
        dst[var.green.offset / 8] = g;
        dst[var.blue.offset / 8] = b;
        return;
    }
    if (var.bits_per_pixel == 16) {
        const std::uint16_t pixel =
            static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        dst[0] = static_cast<std::uint8_t>(pixel & 0xFF);
        dst[1] = static_cast<std::uint8_t>(pixel >> 8);
    }
}

class Framebuffer {
public:
    ~Framebuffer() {
        close();
    }

    bool open_device() {
        fd_ = ::open("/dev/fb0", O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            VC_LOG_WARN("display", std::string("open /dev/fb0: ") + std::strerror(errno));
            return false;
        }
        if (ioctl(fd_, FBIOGET_FSCREENINFO, &fix_) != 0 ||
            ioctl(fd_, FBIOGET_VSCREENINFO, &var_) != 0) {
            VC_LOG_WARN("display", std::string("FBIOGET_*SCREENINFO: ") + std::strerror(errno));
            close();
            return false;
        }
        size_ = static_cast<std::size_t>(fix_.line_length) * var_.yres_virtual;
        memory_ = static_cast<std::uint8_t*>(
            mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (memory_ == MAP_FAILED) {
            VC_LOG_WARN("display", std::string("mmap /dev/fb0: ") + std::strerror(errno));
            memory_ = nullptr;
            close();
            return false;
        }
        VC_LOG_INFO("display",
                    "framebuffer enabled " + std::to_string(var_.xres) + "x" +
                        std::to_string(var_.yres) + " bpp=" + std::to_string(var_.bits_per_pixel));
        return true;
    }

    void close() {
        if (memory_ != nullptr) {
            munmap(memory_, size_);
            memory_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool valid() const {
        return memory_ != nullptr;
    }

    void render_nv12(const VideoFrame& frame) {
        if (!valid() || frame.format != "NV12" || frame.width <= 0 || frame.height <= 0) {
            return;
        }
        const int stride = frame.stride > 0 ? frame.stride : frame.width;
        const int vertical_stride =
            frame.vertical_stride > 0 ? frame.vertical_stride : frame.height;
        const std::size_t y_storage =
            static_cast<std::size_t>(stride) * vertical_stride;
        if (frame.data.size() < y_storage + y_storage / 2U) {
            return;
        }

        const auto* y_plane = frame.data.data();
        const auto* uv_plane = frame.data.data() + y_storage;
        const int out_w = static_cast<int>(var_.xres);
        const int out_h = static_cast<int>(var_.yres);
        const int bytes_per_pixel = static_cast<int>(var_.bits_per_pixel / 8U);
        if (bytes_per_pixel < 2) {
            return;
        }

        for (int y = 0; y < out_h; ++y) {
            const int src_y = y * frame.height / out_h;
            auto* row = memory_ + static_cast<std::size_t>(y + var_.yoffset) * fix_.line_length +
                        static_cast<std::size_t>(var_.xoffset) * bytes_per_pixel;
            for (int x = 0; x < out_w; ++x) {
                const int src_x = x * frame.width / out_w;
                std::uint8_t r = 0;
                std::uint8_t g = 0;
                std::uint8_t b = 0;
                nv12_to_rgb(y_plane, uv_plane, stride, src_x, src_y, r, g, b);
                write_pixel(row + static_cast<std::size_t>(x) * bytes_per_pixel, var_, r, g, b);
            }
        }
    }

private:
    int fd_ = -1;
    fb_fix_screeninfo fix_{};
    fb_var_screeninfo var_{};
    std::uint8_t* memory_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace

DisplayRenderer::DisplayRenderer(std::string window_name)
    : window_name_(std::move(window_name)) {}

DisplayRenderer::~DisplayRenderer() {
    stop();
}

bool DisplayRenderer::start() {
    if (running_) {
        return true;
    }
    running_ = true;
    thread_ = std::thread(&DisplayRenderer::render_loop, this);
    return true;
}

void DisplayRenderer::stop() {
    running_ = false;
    queue_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DisplayRenderer::submit(VideoFrame frame) {
    if (running_) {
        queue_.push_drop_oldest(std::move(frame));
    }
}

void DisplayRenderer::render_loop() {
    (void)window_name_;
    Framebuffer framebuffer;
    const bool enabled = framebuffer.open_device();
    while (running_) {
        VideoFrame frame;
        if (!queue_.pop(frame)) {
            break;
        }
        if (enabled) {
            framebuffer.render_nv12(frame);
        }
    }
}

}  // namespace visioncast
