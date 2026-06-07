cmake_minimum_required(VERSION 3.16)

project(VisionCast
    VERSION 0.1.0
    DESCRIPTION "VisionCast RK3588 audio/video streaming"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(THIRDPARTY_DIR "${PROJECT_SOURCE_DIR}/3rdparty")
set(RGA_INC_DIR "${THIRDPARTY_DIR}/rga/include")
set(RGA_LIB "${THIRDPARTY_DIR}/rga/lib/Linux/aarch64/librga.so")
set(MPP_INC_DIR "${THIRDPARTY_DIR}/mpp/include/rockchip")
set(MPP_LIB_DIR "${THIRDPARTY_DIR}/mpp/Linux/aarch64")
set(MPP_LIB "${MPP_LIB_DIR}/librockchip_mpp.so")
set(FFMPEG_INC_DIR "${THIRDPARTY_DIR}/ffmpeg/include")
set(FFMPEG_LIB_DIR "${THIRDPARTY_DIR}/ffmpeg/lib/aarch64")
set(AVFORMAT_LIB "${FFMPEG_LIB_DIR}/libavformat.so")
set(AVCODEC_LIB "${FFMPEG_LIB_DIR}/libavcodec.so")
set(AVUTIL_LIB "${FFMPEG_LIB_DIR}/libavutil.so")
set(SWRESAMPLE_LIB "${FFMPEG_LIB_DIR}/libswresample.so")
set(JPEG_INC_DIR "${THIRDPARTY_DIR}/jpeg/include")
set(JPEG_ARCH_INC_DIR "${JPEG_INC_DIR}/aarch64-linux-gnu")
set(JPEG_LIB_DIR "${THIRDPARTY_DIR}/jpeg/lib/aarch64")
set(JPEG_LIB "${JPEG_LIB_DIR}/libjpeg.so")
set(ALSA_LIB_DIR "${THIRDPARTY_DIR}/alsa/lib/aarch64")
set(WEBRTC_INC_DIR "${THIRDPARTY_DIR}/webrtc/include")
set(WEBRTC_LIB_DIR "${THIRDPARTY_DIR}/webrtc/lib/aarch64")
set(DATACHANNEL_LIB "${WEBRTC_LIB_DIR}/libdatachannel.so")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" VISIONCAST_SYSTEM_PROCESSOR)
if (VISIONCAST_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(VISIONCAST_TARGET_RK3588 ON)
else()
    set(VISIONCAST_TARGET_RK3588 OFF)
endif()

add_executable(visioncast
    src/main.cpp
    src/common/config.cpp
    src/common/log.cpp
    src/common/time_utils.cpp
    src/media/audio_capture.cpp
    src/media/audio_encoder.cpp
    src/media/display_renderer.cpp
    src/media/media_clock.cpp
    src/media/mjpeg_decoder.cpp
    src/media/mpp_encoder.cpp
    src/media/rga_processor.cpp
    src/media/video_capture.cpp
    src/pipeline/audio_pipeline.cpp
    src/pipeline/av_pipeline.cpp
    src/pipeline/pipeline_manager.cpp
    src/pipeline/video_pipeline.cpp
    src/transport/rtp_packetizer.cpp
    src/transport/av_transport.cpp
    src/transport/udp_sender.cpp
    src/transport/rtmp_pusher.cpp
    src/transport/webrtc_pusher.cpp
)

target_include_directories(visioncast
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(visioncast
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_link_libraries(visioncast PRIVATE Threads::Threads)
target_link_libraries(visioncast PRIVATE ${CMAKE_DL_LIBS})
if (VISIONCAST_TARGET_RK3588 AND EXISTS "${JPEG_INC_DIR}/jpeglib.h" AND EXISTS "${JPEG_LIB}")
    target_include_directories(visioncast SYSTEM PRIVATE ${JPEG_INC_DIR} ${JPEG_ARCH_INC_DIR})
    target_link_libraries(visioncast PRIVATE ${JPEG_LIB})
else()
    find_package(JPEG REQUIRED)
    target_link_libraries(visioncast PRIVATE JPEG::JPEG)
endif()

if (VISIONCAST_TARGET_RK3588 AND EXISTS "${MPP_INC_DIR}/rk_mpi.h" AND EXISTS "${MPP_LIB}")
    target_compile_definitions(visioncast PRIVATE VISIONCAST_ENABLE_MPP=1)
    target_include_directories(visioncast SYSTEM PRIVATE ${MPP_INC_DIR})
    target_link_libraries(visioncast PRIVATE ${MPP_LIB})
else()
    message(WARNING "Rockchip MPP headers/library were not found; video encoding will be disabled")
endif()



if (VISIONCAST_TARGET_RK3588
    AND EXISTS "${FFMPEG_INC_DIR}/libavformat/avformat.h"
    AND EXISTS "${AVFORMAT_LIB}"
    AND EXISTS "${AVCODEC_LIB}"
    AND EXISTS "${AVUTIL_LIB}"
    AND EXISTS "${SWRESAMPLE_LIB}")
    target_compile_definitions(visioncast PRIVATE VISIONCAST_ENABLE_FFMPEG=1)
    target_include_directories(visioncast SYSTEM PRIVATE ${FFMPEG_INC_DIR})
else()
    message(WARNING "AArch64 FFmpeg development files were not found; RTMP mode will be disabled")
endif()

if (VISIONCAST_TARGET_RK3588 AND EXISTS "${RGA_INC_DIR}/im2d.h" AND EXISTS "${RGA_LIB}")
    target_compile_definitions(visioncast PRIVATE VISIONCAST_ENABLE_RGA=1)
    target_include_directories(visioncast SYSTEM PRIVATE ${RGA_INC_DIR})
    target_link_libraries(visioncast PRIVATE ${RGA_LIB})
else()
    message(WARNING "Rockchip RGA headers/library were not found; software NV12 conversion will be used")
endif()

if (VISIONCAST_TARGET_RK3588
    AND EXISTS "${WEBRTC_INC_DIR}/rtc/rtc.hpp"
    AND EXISTS "${DATACHANNEL_LIB}")
    target_compile_definitions(visioncast PRIVATE VISIONCAST_ENABLE_WEBRTC=1)
    target_include_directories(visioncast SYSTEM PRIVATE ${WEBRTC_INC_DIR})
    target_link_libraries(visioncast PRIVATE ${DATACHANNEL_LIB})
else()
    message(WARNING "libdatachannel WebRTC files were not found; WHIP mode will be disabled")
endif()

install(TARGETS visioncast RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION config)
install(DIRECTORY scripts/run scripts/board DESTINATION scripts
    USE_SOURCE_PERMISSIONS
)
install(PROGRAMS scripts/fix_freq_rk3588.sh DESTINATION scripts)

install(DIRECTORY "${THIRDPARTY_DIR}/rga/lib/Linux/aarch64/"
    DESTINATION lib
    FILES_MATCHING PATTERN "*.so*"
)
install(DIRECTORY "${MPP_LIB_DIR}/"
    DESTINATION lib
    FILES_MATCHING PATTERN "*.so*"
)
install(DIRECTORY "${FFMPEG_LIB_DIR}/"
    DESTINATION lib
    FILES_MATCHING PATTERN "*.so*"
)
install(DIRECTORY "${JPEG_LIB_DIR}/"
    DESTINATION lib
    FILES_MATCHING PATTERN "*.so*"
)
install(DIRECTORY "${ALSA_LIB_DIR}/"
    DESTINATION lib
    FILES_MATCHING PATTERN "*.so*"
)
if (EXISTS "${WEBRTC_LIB_DIR}")
    install(DIRECTORY "${WEBRTC_LIB_DIR}/"
        DESTINATION lib
        FILES_MATCHING PATTERN "*.so*"
    )
endif()
