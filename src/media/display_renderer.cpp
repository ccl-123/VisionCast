/**
 * @file display_renderer.cpp
 * @brief VisionCast 显示渲染实现文件
 * @details 支持多种显示后端，优先级顺序为：
 *          1. X11 Window (动态加载 libX11 并创建渲染窗口)；
 *          2. Linux Framebuffer (/dev/fb0)。
 *          集成了瑞芯微 RGA 硬件加速引擎，用于将 NV12 格式的高速缩放与色彩空间转换（至 RGB/BGR 格式）；
 *          若 RGA 不可用，则自动退化为 CPU 软件颜色转换和差值缩放。
 */

#include "media/display_renderer.h"

#include <fcntl.h>
#include <dlfcn.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>
#include <utility>

#include "common/log.h"

#if defined(VISIONCAST_ENABLE_RGA)
#include "im2d.h"
#endif

namespace visioncast {
namespace {

// 辅助数值裁剪函数
std::uint8_t clamp_u8(int value) {
    return static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
}

// 软件转换算法：将 NV12 (YUV420sp) 的指定像素位置的 Y/U/V 样本还原为标准 RGB888。
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

// 软件像素写入逻辑：根据当前 framebuffer 的 bits_per_pixel（16/24/32位），将 RGB 像素以正确的通道偏移和字节序写入目标显存。
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

/**
 * @class X11Window
 * @brief 动态加载 libX11.so 并管理 X11 渲染窗口的辅助类
 */
class X11Window {
public:
    ~X11Window() {
        close();
    }

    // 打开 X11 窗口并根据当前屏幕尺寸和视频分辨率计算最佳等比缩放大小
    bool open_window(const std::string& name, int source_width, int source_height) {
        if (std::getenv("DISPLAY") == nullptr || !load_api()) {
            return false;
        }
        display_ = x_open_display_(nullptr);
        if (display_ == nullptr) {
            VC_LOG_WARN("display", "XOpenDisplay failed for DISPLAY=" +
                                       std::string(std::getenv("DISPLAY")));
            close();
            return false;
        }

        const int screen = DefaultScreen(display_);
        const int max_width = std::max(320, DisplayWidth(display_, screen) - 80);
        const int max_height = std::max(240, DisplayHeight(display_, screen) - 100);
        const double scale =
            std::min(1.0,
                     std::min(static_cast<double>(max_width) / source_width,
                              static_cast<double>(max_height) / source_height));
        width_ = std::max(1, static_cast<int>(source_width * scale));
        height_ = std::max(1, static_cast<int>(source_height * scale));

        window_ = x_create_simple_window_(
            display_,
            RootWindow(display_, screen),
            20,
            20,
            static_cast<unsigned int>(width_),
            static_cast<unsigned int>(height_),
            1,
            BlackPixel(display_, screen),
            WhitePixel(display_, screen));
        if (window_ == 0) {
            VC_LOG_WARN("display", "XCreateSimpleWindow failed");
            close();
            return false;
        }
        x_store_name_(display_, window_, name.c_str());
        x_select_input_(
            display_, window_, ExposureMask | StructureNotifyMask);
        gc_ = x_create_gc_(display_, window_, 0, nullptr);
        if (gc_ == nullptr) {
            VC_LOG_WARN("display", "XCreateGC failed");
            close();
            return false;
        }
        x_map_raised_(display_, window_);
        x_flush_(display_);
        VC_LOG_INFO("display",
                    "X11/XWayland preview enabled " + std::to_string(width_) +
                        "x" + std::to_string(height_) + " DISPLAY=" +
                        std::string(std::getenv("DISPLAY")));
        return true;
    }

    bool valid() const {
        return display_ != nullptr && window_ != 0 && gc_ != nullptr;
    }

    // 渲染 NV12 格式视频帧
    void render_nv12(const VideoFrame& frame) {
        if (!valid() || frame.format != "NV12" || frame.width <= 0 ||
            frame.height <= 0) {
            return;
        }
        process_events();
        if (!valid()) {
            return;
        }

        const int screen = DefaultScreen(display_);
        XImage* image = x_create_image_(
            display_,
            DefaultVisual(display_, screen),
            static_cast<unsigned int>(DefaultDepth(display_, screen)),
            ZPixmap,
            0,
            nullptr,
            static_cast<unsigned int>(width_),
            static_cast<unsigned int>(height_),
            32,
            0);
        if (image == nullptr || image->bits_per_pixel != 32) {
            if (image != nullptr) {
                image->f.destroy_image(image);
            }
            return;
        }

        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(image->bytes_per_line) * height_);
        image->data = reinterpret_cast<char*>(pixels.data());
        const int stride = frame.stride > 0 ? frame.stride : frame.width;
        const int vertical_stride =
            frame.vertical_stride > 0 ? frame.vertical_stride : frame.height;
        const std::size_t y_storage =
            static_cast<std::size_t>(stride) * vertical_stride;
        if (frame.data.size() < y_storage + y_storage / 2U) {
            image->data = nullptr;
            image->f.destroy_image(image);
            return;
        }

        bool converted = false;
#if defined(VISIONCAST_ENABLE_RGA)
        // 尝试调用瑞芯微 RGA 加速执行色彩转换及缩放
        int destination_format = 0;
        if (image->red_mask == 0x00ff0000UL &&
            image->blue_mask == 0x000000ffUL) {
            destination_format = RK_FORMAT_BGRA_8888;
        } else if (image->red_mask == 0x000000ffUL &&
                   image->blue_mask == 0x00ff0000UL) {
            destination_format = RK_FORMAT_RGBA_8888;
        }
        if (destination_format != 0 &&
            image->bytes_per_line == width_ * 4) {
            rga_buffer_t source = wrapbuffer_virtualaddr_t(
                const_cast<std::uint8_t*>(frame.data.data()),
                frame.width,
                frame.height,
                stride,
                vertical_stride,
                RK_FORMAT_YCbCr_420_SP);
            rga_buffer_t destination = wrapbuffer_virtualaddr_t(
                pixels.data(),
                width_,
                height_,
                width_,
                height_,
                destination_format);
            const IM_STATUS status = imresize(source, destination);
            converted =
                status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR;
        }
#endif
        // 若 RGA 硬件加速不可用，则退化至 CPU 软件双线性插值缩放与 NV12->RGB 转换
        if (!converted) {
            const auto* y_plane = frame.data.data();
            const auto* uv_plane = frame.data.data() + y_storage;
            for (int y = 0; y < height_; ++y) {
                const int source_y = y * frame.height / height_;
                auto* row = pixels.data() +
                            static_cast<std::size_t>(y) * image->bytes_per_line;
                for (int x = 0; x < width_; ++x) {
                    const int source_x = x * frame.width / width_;
                    std::uint8_t r = 0;
                    std::uint8_t g = 0;
                    std::uint8_t b = 0;
                    nv12_to_rgb(
                        y_plane, uv_plane, stride, source_x, source_y, r, g, b);
                    auto* pixel = row + static_cast<std::size_t>(x) * 4U;
                    if (image->red_mask == 0x00ff0000UL) {
                        pixel[0] = b;
                        pixel[1] = g;
                        pixel[2] = r;
                    } else {
                        pixel[0] = r;
                        pixel[1] = g;
                        pixel[2] = b;
                    }
                    pixel[3] = 0xFF;
                }
            }
        }

        // 推送 XImage 数据刷新 X 窗口
        x_put_image_(display_,
                     window_,
                     gc_,
                     image,
                     0,
                     0,
                     0,
                     0,
                     static_cast<unsigned int>(width_),
                     static_cast<unsigned int>(height_));
        x_flush_(display_);
        image->data = nullptr;
        image->f.destroy_image(image);
    }

private:
    template <typename Function>
    bool load_symbol(Function& function, const char* name) {
        function = reinterpret_cast<Function>(dlsym(library_, name));
        return function != nullptr;
    }

    // 动态 dlopen 绑定 libX11 接口
    bool load_api() {
        library_ = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (library_ == nullptr) {
            VC_LOG_WARN("display", std::string("dlopen libX11.so.6: ") + dlerror());
            return false;
        }
        if (!load_symbol(x_open_display_, "XOpenDisplay") ||
            !load_symbol(x_close_display_, "XCloseDisplay") ||
            !load_symbol(x_create_simple_window_, "XCreateSimpleWindow") ||
            !load_symbol(x_store_name_, "XStoreName") ||
            !load_symbol(x_select_input_, "XSelectInput") ||
            !load_symbol(x_create_gc_, "XCreateGC") ||
            !load_symbol(x_free_gc_, "XFreeGC") ||
            !load_symbol(x_map_raised_, "XMapRaised") ||
            !load_symbol(x_destroy_window_, "XDestroyWindow") ||
            !load_symbol(x_pending_, "XPending") ||
            !load_symbol(x_next_event_, "XNextEvent") ||
            !load_symbol(x_create_image_, "XCreateImage") ||
            !load_symbol(x_put_image_, "XPutImage") ||
            !load_symbol(x_flush_, "XFlush")) {
            VC_LOG_WARN("display", "libX11 is missing required symbols");
            close();
            return false;
        }
        return true;
    }

    // 轮询处理 X11 事件（如窗口尺寸改变、窗口关闭等）
    void process_events() {
        while (valid() && x_pending_(display_) > 0) {
            XEvent event{};
            x_next_event_(display_, &event);
            if (event.type == ConfigureNotify) {
                width_ = std::max(1, event.xconfigure.width);
                height_ = std::max(1, event.xconfigure.height);
            } else if (event.type == DestroyNotify) {
                window_ = 0;
            }
        }
    }

    void close() {
        if (display_ != nullptr && gc_ != nullptr) {
            x_free_gc_(display_, gc_);
            gc_ = nullptr;
        }
        if (display_ != nullptr && window_ != 0) {
            x_destroy_window_(display_, window_);
            window_ = 0;
        }
        if (display_ != nullptr) {
            x_close_display_(display_);
            display_ = nullptr;
        }
        if (library_ != nullptr) {
            dlclose(library_);
            library_ = nullptr;
        }
    }

    void* library_ = nullptr;
    Display* display_ = nullptr;
    Window window_ = 0;
    GC gc_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    decltype(&XOpenDisplay) x_open_display_ = nullptr;
    decltype(&XCloseDisplay) x_close_display_ = nullptr;
    decltype(&XCreateSimpleWindow) x_create_simple_window_ = nullptr;
    decltype(&XStoreName) x_store_name_ = nullptr;
    decltype(&XSelectInput) x_select_input_ = nullptr;
    decltype(&XCreateGC) x_create_gc_ = nullptr;
    decltype(&XFreeGC) x_free_gc_ = nullptr;
    decltype(&XMapRaised) x_map_raised_ = nullptr;
    decltype(&XDestroyWindow) x_destroy_window_ = nullptr;
    decltype(&XPending) x_pending_ = nullptr;
    decltype(&XNextEvent) x_next_event_ = nullptr;
    decltype(&XCreateImage) x_create_image_ = nullptr;
    decltype(&XPutImage) x_put_image_ = nullptr;
    decltype(&XFlush) x_flush_ = nullptr;
};

/**
 * @class Framebuffer
 * @brief 基于 Linux Framebuffer (/dev/fb0) 的显存直接渲染辅助类
 */
class Framebuffer {
public:
    ~Framebuffer() {
        close();
    }

    // 打开 framebuffer 设备，获取参数并执行 mmap 映射
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

    // 渲染 NV12 格式视频帧至显存空间
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

        const int bytes_per_pixel = static_cast<int>(var_.bits_per_pixel / 8U);

#if defined(VISIONCAST_ENABLE_RGA)
        // 尝试调用瑞芯微 RGA 硬件加速引擎进行图像缩放和颜色空间转换，直接写入显存
        int dest_format = 0;
        if (var_.bits_per_pixel == 32) {
            if (var_.red.offset == 0) {
                dest_format = RK_FORMAT_RGBA_8888;
            } else if (var_.blue.offset == 0) {
                dest_format = RK_FORMAT_BGRA_8888;
            }
        } else if (var_.bits_per_pixel == 24) {
            if (var_.red.offset == 0) {
                dest_format = RK_FORMAT_RGB_888;
            } else if (var_.blue.offset == 0) {
                dest_format = RK_FORMAT_BGR_888;
            }
        } else if (var_.bits_per_pixel == 16) {
            dest_format = RK_FORMAT_RGB_565;
        }

        if (dest_format != 0 && bytes_per_pixel > 0) {
            rga_buffer_t src = wrapbuffer_virtualaddr_t(
                const_cast<std::uint8_t*>(frame.data.data()),
                frame.width,
                frame.height,
                stride,
                vertical_stride,
                RK_FORMAT_YCbCr_420_SP);

            void* dst_addr = memory_ + static_cast<std::size_t>(var_.yoffset) * fix_.line_length +
                             static_cast<std::size_t>(var_.xoffset) * bytes_per_pixel;

            rga_buffer_t dst = wrapbuffer_virtualaddr_t(
                dst_addr,
                static_cast<int>(var_.xres),
                static_cast<int>(var_.yres),
                static_cast<int>(fix_.line_length / bytes_per_pixel),
                static_cast<int>(var_.yres),
                dest_format);

            IM_STATUS status = imresize(src, dst);
            if (status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR) {
                return;
            }
        }
#endif

        // 若硬件加速不可用，则执行 CPU 软件色彩空间转换并按行写入映射的显存
        const auto* y_plane = frame.data.data();
        const auto* uv_plane = frame.data.data() + y_storage;
        const int out_w = static_cast<int>(var_.xres);
        const int out_h = static_cast<int>(var_.yres);
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

// 渲染线程的主事件循环
void DisplayRenderer::render_loop() {
    X11Window window;
    Framebuffer framebuffer;
    bool backend_selected = false;
    bool use_window = false;
    bool use_framebuffer = false;
    while (running_) {
        VideoFrame frame;
        if (!queue_.pop(frame)) {
            break;
        }
        // 首帧到来时探测最佳显示后端（优先 X11 窗口，其次 Framebuffer）
        if (!backend_selected) {
            use_window =
                window.open_window(window_name_, frame.width, frame.height);
            if (!use_window) {
                use_framebuffer = framebuffer.open_device();
            }
            backend_selected = true;
        }
        // 渲染到对应后端
        if (use_window && window.valid()) {
            window.render_nv12(frame);
        } else if (use_framebuffer) {
            framebuffer.render_nv12(frame);
        }
    }
}

}  // namespace visioncast
