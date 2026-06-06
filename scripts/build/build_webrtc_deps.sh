#!/usr/bin/env bash
# ==============================================================================
# Build libdatachannel for VisionCast WebRTC WHIP mode.
#
# Output:
#   3rdparty/webrtc/include/rtc/*.hpp
#   3rdparty/webrtc/lib/aarch64/libdatachannel.so*
#   3rdparty/webrtc/lib/aarch64/libssl.so.3
#   3rdparty/webrtc/lib/aarch64/libcrypto.so.3
#
# The script downloads libdatachannel v0.22 and the exact submodule commits used
# by that release, then cross-compiles it with aarch64-linux-gnu-g++.
# ==============================================================================

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="${ROOT_DIR}/build/webrtc_deps"
SRC_DIR="${WORK_DIR}/libdatachannel-src"
BUILD_DIR="${WORK_DIR}/build_aarch64"
INSTALL_DIR="${ROOT_DIR}/3rdparty/webrtc"
OPENSSL_DEV_DIR="${WORK_DIR}/openssl_arm64_dev"

LIBDATACHANNEL_REF="${LIBDATACHANNEL_REF:-b74fa447fc7cec41689ca73515ef9a46d2a29fae}"
CC="${CC:-aarch64-linux-gnu-gcc}"
CXX="${CXX:-aarch64-linux-gnu-g++}"
AARCH64_LIB_DIR="${AARCH64_LIB_DIR:-/usr/lib/aarch64-linux-gnu}"

download_extract() {
    local url="$1"
    local dest="$2"
    local name="$3"
    local archive="${WORK_DIR}/${name}.tar.gz"

    rm -rf "${dest}"
    mkdir -p "${dest}"
    echo "  -> ${name}"
    curl -L --connect-timeout 20 --max-time 180 -o "${archive}" "${url}"
    tar -xzf "${archive}" -C "${dest}" --strip-components=1
}

echo "========================================"
echo "VisionCast WebRTC dependency build"
echo "========================================"
echo "ROOT:    ${ROOT_DIR}"
echo "WORK:    ${WORK_DIR}"
echo "INSTALL: ${INSTALL_DIR}"
echo "CXX:     $("${CXX}" --version | head -n1)"

mkdir -p "${WORK_DIR}"

echo ""
echo "Downloading libdatachannel and submodules..."
download_extract \
    "https://github.com/paullouisageneau/libdatachannel/archive/${LIBDATACHANNEL_REF}.tar.gz" \
    "${SRC_DIR}" \
    "libdatachannel"

download_extract \
    "https://github.com/SergiusTheBest/plog/archive/e21baecd4753f14da64ede979c5a19302618b752.tar.gz" \
    "${SRC_DIR}/deps/plog" \
    "plog"

download_extract \
    "https://github.com/sctplab/usrsctp/archive/ebb18adac6501bad4501b1f6dccb67a1c85cc299.tar.gz" \
    "${SRC_DIR}/deps/usrsctp" \
    "usrsctp"

download_extract \
    "https://github.com/paullouisageneau/libjuice/archive/8d1a99a0683a811876c03a73ff764a92774027ad.tar.gz" \
    "${SRC_DIR}/deps/libjuice" \
    "libjuice"

download_extract \
    "https://github.com/nlohmann/json/archive/9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03.tar.gz" \
    "${SRC_DIR}/deps/json" \
    "json"

download_extract \
    "https://github.com/cisco/libsrtp/archive/a566a9cfcd619e8327784aa7cff4a1276dc1e895.tar.gz" \
    "${SRC_DIR}/deps/libsrtp" \
    "libsrtp"

OPENSSL_TARGET_SHIM='
if(OPENSSL_FOUND)
  if(NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto UNKNOWN IMPORTED)
  endif()
  set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    IMPORTED_LOCATION_RELEASE "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
  if(NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL UNKNOWN IMPORTED)
  endif()
  set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    IMPORTED_LOCATION_RELEASE "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
endif()
'

OPENSSL_TARGET_SHIM_ESCAPED="${OPENSSL_TARGET_SHIM//\$/\\\$}"
perl -0pi -e 's/find_package\(OpenSSL REQUIRED\)/find_package(OpenSSL REQUIRED)\n'"${OPENSSL_TARGET_SHIM_ESCAPED//$'\n'/\\n}"'/' \
    "${SRC_DIR}/CMakeLists.txt"
perl -0pi -e 's/find_package\(OpenSSL 1\.1\.0 REQUIRED\)/find_package(OpenSSL 1.1.0 REQUIRED)\n'"${OPENSSL_TARGET_SHIM_ESCAPED//$'\n'/\\n}"'/' \
    "${SRC_DIR}/deps/libsrtp/CMakeLists.txt"

if [[ ! -f "${OPENSSL_DEV_DIR}/root/usr/include/aarch64-linux-gnu/openssl/opensslconf.h" \
      || ! -f "${OPENSSL_DEV_DIR}/root/usr/lib/aarch64-linux-gnu/libssl.so.3" \
      || ! -f "${OPENSSL_DEV_DIR}/root/usr/lib/aarch64-linux-gnu/libcrypto.so.3" ]]; then
    echo ""
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

rm -rf "${BUILD_DIR}" "${INSTALL_DIR}"
mkdir -p "${BUILD_DIR}" "${INSTALL_DIR}/lib/aarch64"

echo ""
echo "Configuring libdatachannel..."
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_FIND_ROOT_PATH="${AARCH64_LIB_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
    -DCMAKE_INSTALL_LIBDIR=lib/aarch64 \
    -DCMAKE_C_FLAGS="-I${OPENSSL_DEV_ROOT}/usr/include/aarch64-linux-gnu" \
    -DCMAKE_CXX_FLAGS="-I${OPENSSL_DEV_ROOT}/usr/include/aarch64-linux-gnu" \
    -DCMAKE_EXE_LINKER_FLAGS="-L${AARCH64_LIB_DIR} -Wl,-rpath-link,${AARCH64_LIB_DIR}" \
    -DOPENSSL_INCLUDE_DIR="${OPENSSL_DEV_ROOT}/usr/include" \
    -DOPENSSL_SSL_LIBRARY="${OPENSSL_LIB_DIR}/libssl.so.3" \
    -DOPENSSL_CRYPTO_LIBRARY="${OPENSSL_LIB_DIR}/libcrypto.so.3" \
    -DNO_EXAMPLES=ON \
    -DNO_TESTS=ON \
    -DNO_WEBSOCKET=ON \
    -DNO_MEDIA=OFF \
    -DUSE_NICE=OFF \
    -DUSE_GNUTLS=OFF \
    -DUSE_MBEDTLS=OFF \
    -DBUILD_SHARED_LIBS=ON

echo ""
echo "Building libdatachannel..."
cmake --build "${BUILD_DIR}" -j"$(nproc)"
cmake --install "${BUILD_DIR}"

cp -f "${OPENSSL_LIB_DIR}/libssl.so.3" "${INSTALL_DIR}/lib/aarch64/"
cp -f "${OPENSSL_LIB_DIR}/libcrypto.so.3" "${INSTALL_DIR}/lib/aarch64/"

echo ""
echo "========================================"
echo "WebRTC dependencies installed"
echo "========================================"
echo "Include: ${INSTALL_DIR}/include"
echo "Lib:     ${INSTALL_DIR}/lib/aarch64"
