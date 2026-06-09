#!/usr/bin/env bash
# ==============================================================================
# VisionCast RK3588 板端本机构建脚本
#
# 使用场景：
#   1. 已经把完整 VisionCast 工程放到 RK3588 开发板上；
#   2. 希望直接在板端 Ubuntu 22.04 aarch64 环境编译；
#   3. PC 侧交叉编译和自动部署请使用 scripts/build/cross_build.sh。
#
# 输出：
#   build/build_linux_aarch64/          CMake 构建目录
#   install/visioncast/bin/visioncast   安装后的可执行文件
#   install/visioncast/config/          安装后的配置文件
#   install/visioncast/scripts/         安装后的板端脚本
#
# 可覆盖环境变量：
#   GCC_COMPILER=aarch64-linux-gnu      编译器前缀
# ==============================================================================

set -euo pipefail

GCC_COMPILER="${GCC_COMPILER:-aarch64-linux-gnu}"

export CC="${GCC_COMPILER}-gcc"
export CXX="${GCC_COMPILER}-g++"

ROOT_PWD="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_PWD}/build/build_linux_aarch64"
INSTALL_DIR="${ROOT_PWD}/install/visioncast"

echo "========================================"
echo "VisionCast RK3588 板端构建"
echo "========================================"
echo "ROOT:    ${ROOT_PWD}"
echo "BUILD:   ${BUILD_DIR}"
echo "INSTALL: ${INSTALL_DIR}"
echo "CXX:     $("${CXX}" --version | head -n1)"

echo ""
echo "检查 FFmpeg 8.1.1 WHIP/RTSP 依赖..."
"${ROOT_PWD}/scripts/build/deps/build_ffmpeg.sh"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# 仅在首次构建或顶层 CMakeLists.txt 更新后重新配置，保留增量编译速度。
if [[ ! -f "CMakeCache.txt" || "${ROOT_PWD}/CMakeLists.txt" -nt "CMakeCache.txt" ]]; then
    rm -f CMakeCache.txt
    rm -rf CMakeFiles
    cmake ../.. \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"
fi

make -j"$(nproc)"
make install

echo ""
echo "========================================"
echo "VisionCast 板端构建完成"
echo "========================================"
echo "在 RK3588 上运行："
echo "  cd ${INSTALL_DIR}"
echo "  ./scripts/run/run_13855_camera.sh"
echo "  ./scripts/run/run_usb_camera.sh"
