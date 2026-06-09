#!/usr/bin/env bash
# ==============================================================================
# Build FFmpeg 8.1.1 with OpenSSL and the native WHIP muxer.
#
# Output:
#   3rdparty/ffmpeg/include/libavcodec/*.h
#   3rdparty/ffmpeg/include/libavformat/*.h
#   3rdparty/ffmpeg/include/libavutil/*.h
#   3rdparty/ffmpeg/include/libswresample/*.h
#   3rdparty/ffmpeg/lib/aarch64/libavformat.so*
#   3rdparty/ffmpeg/lib/aarch64/libavcodec.so*
#   3rdparty/ffmpeg/lib/aarch64/libavutil.so*
#   3rdparty/ffmpeg/lib/aarch64/libswresample.so*
#   3rdparty/ffmpeg/lib/aarch64/libssl.so.3
#   3rdparty/ffmpeg/lib/aarch64/libcrypto.so.3
# ==============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
WORK_DIR="${ROOT_DIR}/build/ffmpeg_build"
FFMPEG_SRC_DIR="${WORK_DIR}/ffmpeg-8.1.1"
FFMPEG_TAR="${ROOT_DIR}/build/thirdparty_src/ffmpeg-8.1.1.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-8.1.1.tar.xz"
INSTALL_DIR="${ROOT_DIR}/3rdparty/ffmpeg"
OPENSSL_DEV_DIR="${ROOT_DIR}/build/webrtc_deps/openssl_arm64_dev"
BUILD_OUT_DIR="${WORK_DIR}/ffmpeg-8.1.1-install"
INSTALLED_AVFORMAT="${INSTALL_DIR}/lib/aarch64/libavformat.so.62.12.101"
INSTALLED_VERSION_HEADER="${INSTALL_DIR}/include/libavutil/ffversion.h"
INSTALLED_LIBSSL="${INSTALL_DIR}/lib/aarch64/libssl.so.3"
INSTALLED_LIBCRYPTO="${INSTALL_DIR}/lib/aarch64/libcrypto.so.3"

CC="${CC:-aarch64-linux-gnu-gcc}"
CXX="${CXX:-aarch64-linux-gnu-g++}"

ffmpeg_whip_ready() {
    [[ -f "${INSTALLED_AVFORMAT}" ]] &&
        [[ -f "${INSTALLED_VERSION_HEADER}" ]] &&
        [[ -f "${INSTALLED_LIBSSL}" ]] &&
        [[ -f "${INSTALLED_LIBCRYPTO}" ]] &&
        grep -q 'FFMPEG_VERSION "8.1.1"' "${INSTALLED_VERSION_HEADER}" &&
        strings "${INSTALLED_AVFORMAT}" | grep -q "WHIP muxer" &&
        strings "${INSTALLED_AVFORMAT}" | grep -q "dtls_active"
}

mkdir -p "${WORK_DIR}"

# 1. Prepare the aarch64 OpenSSL development files.
if [[ ! -f "${OPENSSL_DEV_DIR}/root/usr/include/aarch64-linux-gnu/openssl/opensslconf.h" \
      || ! -f "${OPENSSL_DEV_DIR}/root/usr/lib/aarch64-linux-gnu/libssl.so.3" \
      || ! -f "${OPENSSL_DEV_DIR}/root/usr/lib/aarch64-linux-gnu/libcrypto.so.3" ]]; then
    echo "Downloading arm64 OpenSSL development/runtime files..."
    rm -rf "${OPENSSL_DEV_DIR}"
    mkdir -p "${OPENSSL_DEV_DIR}"
    (
        cd "${OPENSSL_DEV_DIR}"
        apt-get download libssl-dev:arm64
        apt-get download libssl3:arm64
        for deb in ./*.deb; do
            dpkg-deb -x "${deb}" root
        done
    )
fi
OPENSSL_DEV_ROOT="${OPENSSL_DEV_DIR}/root"
OPENSSL_LIB_DIR="${OPENSSL_DEV_ROOT}/usr/lib/aarch64-linux-gnu"

copy_openssl_runtime() {
    mkdir -p "${INSTALL_DIR}/lib/aarch64"
    cp -Lf "${OPENSSL_LIB_DIR}/libssl.so.3" "${INSTALLED_LIBSSL}"
    cp -Lf "${OPENSSL_LIB_DIR}/libcrypto.so.3" "${INSTALLED_LIBCRYPTO}"
}

copy_openssl_runtime

if [[ "${FORCE_FFMPEG_REBUILD:-0}" != "1" ]] && ffmpeg_whip_ready; then
    echo "FFmpeg 8.1.1 WHIP 已就绪，跳过重新编译。"
    exit 0
fi

# 2. Prepare a pristine FFmpeg 8.1.1 source tree.
if [[ ! -f "${FFMPEG_TAR}" ]]; then
    echo "Downloading FFmpeg 8.1.1 source..."
    mkdir -p "$(dirname "${FFMPEG_TAR}")"
    curl -L --connect-timeout 20 --max-time 300 -o "${FFMPEG_TAR}" "${FFMPEG_URL}"
fi
rm -rf "${FFMPEG_SRC_DIR}" "${BUILD_OUT_DIR}"
tar -xJf "${FFMPEG_TAR}" -C "${WORK_DIR}"

# 3. Apply custom WHIP candidate matching patch
python3 "${ROOT_DIR}/scripts/build/deps/patch_whip.py" "${FFMPEG_SRC_DIR}"

# 4. Configure and compile FFmpeg.
echo "Configuring FFmpeg..."
(
    cd "${FFMPEG_SRC_DIR}"
    ./configure \
        --prefix="${BUILD_OUT_DIR}" \
        --cc="${CC}" \
        --cxx="${CXX}" \
        --cross-prefix="${CROSS_PREFIX:-aarch64-linux-gnu-}" \
        --enable-cross-compile \
        --target-os=linux \
        --arch=aarch64 \
        --enable-shared \
        --disable-static \
        --disable-programs \
        --disable-doc \
        --disable-debug \
        --disable-avdevice \
        --disable-avfilter \
        --disable-swscale \
        --disable-everything \
        --enable-openssl \
        --enable-network \
        --enable-protocol='rtmp,tcp,http,https,tls,udp,rtp,srtp,dtls' \
        --enable-muxer='flv,whip,rtp' \
        --enable-encoder='aac' \
        --enable-parser='aac,h264,hevc' \
        --enable-demuxer='flv' \
        --enable-decoder='aac,h264,hevc' \
        --enable-bsf='aac_adtstoasc,h264_mp4toannexb,hevc_mp4toannexb,extract_extradata' \
        --enable-small \
        --extra-cflags="-I${OPENSSL_DEV_ROOT}/usr/include -I${OPENSSL_DEV_ROOT}/usr/include/aarch64-linux-gnu" \
        --extra-ldflags="-L${OPENSSL_LIB_DIR} -Wl,-rpath-link,${OPENSSL_LIB_DIR}"
        
    echo "Building FFmpeg..."
    make -j"$(nproc)"
    make install
)

# 5. Verify the staged library before replacing the repository artifacts.
STAGED_AVFORMAT="$(find "${BUILD_OUT_DIR}/lib" -maxdepth 1 -type f -name 'libavformat.so.*' | head -n1)"
if [[ -z "${STAGED_AVFORMAT}" ]]; then
    echo "FFmpeg build did not produce libavformat.so" >&2
    exit 1
fi
if ! strings "${STAGED_AVFORMAT}" | grep -q "WHIP muxer"; then
    echo "Built libavformat does not contain the WHIP muxer" >&2
    exit 1
fi
if ! strings "${STAGED_AVFORMAT}" | grep -q "dtls_active"; then
    echo "Built libavformat does not contain the WHIP DTLS options" >&2
    exit 1
fi

# 6. Replace the generated FFmpeg artifacts only after staged verification.
echo "Updating 3rdparty/ffmpeg..."
rm -rf "${INSTALL_DIR}"
mkdir -p "${INSTALL_DIR}/lib/aarch64"
mkdir -p "${INSTALL_DIR}/include"

cp -r "${BUILD_OUT_DIR}/include/"* "${INSTALL_DIR}/include/"
cp -d "${BUILD_OUT_DIR}/lib/"*.so* "${INSTALL_DIR}/lib/aarch64/"
copy_openssl_runtime

echo "========================================"
FFMPEG_VERSION_HEADER="${INSTALL_DIR}/include/libavutil/ffversion.h"
if [[ -f "${FFMPEG_VERSION_HEADER}" ]]; then
    cat "${FFMPEG_VERSION_HEADER}"
fi
echo "FFmpeg 8.1.1 with native WHIP muxer built successfully."
echo "========================================"
