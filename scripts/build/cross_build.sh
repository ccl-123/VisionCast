#!/usr/bin/env bash
# ==============================================================================
# VisionCast RK3588 交叉编译与一键部署脚本
#
# 参考：
#   face_attendance/RK3588-NPU/C++/face_recognition_cap/cross_build.sh
#
# 用法：
#   ./scripts/build/cross_build.sh          增量交叉编译 + 部署到 RK3588
#   ./scripts/build/cross_build.sh --build  只编译，不部署
#   ./scripts/build/cross_build.sh --deploy 只部署 install/visioncast 中已有产物
#   ./scripts/build/cross_build.sh --clean  清理后重新编译 + 部署
#
# 默认部署目标：
#   网线优先：192.168.137.202
#   WiFi 备用：192.168.137.111
#   用户/密码：elf/elf
#   目标目录：/home/elf/open_project/VisionCast/install
#
# 可覆盖环境变量：
#   DEVICE_IP_ETH=192.168.137.202
#   DEVICE_IP_WIFI=192.168.137.111
#   DEVICE_USER=elf
#   DEVICE_PASS=elf
#   DEVICE_TARGET_DIR=/home/elf/open_project/VisionCast/install
#   CC=aarch64-linux-gnu-gcc
#   CXX=aarch64-linux-gnu-g++
#   AARCH64_LIB_DIR=/usr/lib/aarch64-linux-gnu（仅用于系统运行库）
# ==============================================================================

set -euo pipefail

DO_BUILD=true
DO_DEPLOY=true
DO_CLEAN=false

for arg in "$@"; do
    case "${arg}" in
        --deploy)
            DO_BUILD=false
            DO_DEPLOY=true
            ;;
        --clean)
            DO_CLEAN=true
            ;;
        --build)
            DO_BUILD=true
            DO_DEPLOY=false
            ;;
        *)
            echo "未知参数：${arg}"
            echo "用法：./scripts/build/cross_build.sh [--deploy|--clean|--build]"
            exit 1
            ;;
    esac
done

ROOT_PWD="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_PWD}/build/build_cross_aarch64"
PACKAGE_DIR="${ROOT_PWD}/install/visioncast"
MPP_LIB_DIR="${ROOT_PWD}/3rdparty/mpp/Linux/aarch64"

export CC="${CC:-aarch64-linux-gnu-gcc}"
export CXX="${CXX:-aarch64-linux-gnu-g++}"

AARCH64_LIB_DIR="${AARCH64_LIB_DIR:-/usr/lib/aarch64-linux-gnu}"

is_system_runtime_lib() {
    case "$1" in
        ld-linux-aarch64.so.1|libc.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libm.so.6|libresolv.so.2|libnsl.so.1|libutil.so.1)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

find_aarch64_library() {
    local lib_name="$1"
    local search_dirs=(
        "${PACKAGE_DIR}/lib"
        "${AARCH64_LIB_DIR}"
        "/lib/aarch64-linux-gnu"
        "/usr/lib/aarch64-linux-gnu"
    )

    local dir
    for dir in "${search_dirs[@]}"; do
        if [[ -e "${dir}/${lib_name}" || -L "${dir}/${lib_name}" ]]; then
            echo "${dir}/${lib_name}"
            return 0
        fi
    done
    return 1
}

copy_runtime_library() {
    local src="$1"
    local name
    local real
    local real_name
    name="$(basename "${src}")"
    real="$(readlink -f "${src}")"
    real_name="$(basename "${real}")"

    if [[ ! -f "${PACKAGE_DIR}/lib/${real_name}" ]]; then
        cp -f "${real}" "${PACKAGE_DIR}/lib/${real_name}"
        echo "  -> 运行库 ${real_name}"
    fi
    if [[ "${name}" != "${real_name}" ]]; then
        ln -sfn "${real_name}" "${PACKAGE_DIR}/lib/${name}"
    fi
}

collect_runtime_dependencies() {
    if ! command -v readelf >/dev/null 2>&1; then
        echo "未找到 readelf，跳过运行库依赖递归收集。"
        return 0
    fi

    mkdir -p "${PACKAGE_DIR}/lib"
    echo ""
    echo "正在递归收集 aarch64 运行库依赖..."

    declare -A scanned=()
    declare -A missing=()
    local queue=()
    local artifact
    while IFS= read -r artifact; do
        queue+=("${artifact}")
    done < <(find "${PACKAGE_DIR}/bin" "${PACKAGE_DIR}/lib" -maxdepth 1 \( -type f -o -type l \) 2>/dev/null | sort)

    while [[ ${#queue[@]} -gt 0 ]]; do
        artifact="${queue[0]}"
        queue=("${queue[@]:1}")

        local real_artifact
        real_artifact="$(readlink -f "${artifact}" 2>/dev/null || true)"
        if [[ -z "${real_artifact}" || ! -f "${real_artifact}" ]]; then
            continue
        fi
        if [[ -n "${scanned[${real_artifact}]:-}" ]]; then
            continue
        fi
        scanned["${real_artifact}"]=1

        local needed
        while IFS= read -r needed; do
            [[ -z "${needed}" ]] && continue
            if is_system_runtime_lib "${needed}"; then
                continue
            fi

            local found
            found="$(find_aarch64_library "${needed}" || true)"
            if [[ -z "${found}" ]]; then
                if [[ -z "${missing[${needed}]:-}" ]]; then
                    echo "  !! 未找到运行库依赖：${needed}"
                    missing["${needed}"]=1
                fi
                continue
            fi

            copy_runtime_library "${found}"
            if [[ -e "${PACKAGE_DIR}/lib/${needed}" || -L "${PACKAGE_DIR}/lib/${needed}" ]]; then
                queue+=("${PACKAGE_DIR}/lib/${needed}")
            else
                queue+=("${found}")
            fi
        done < <(readelf -d "${real_artifact}" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p')
    done
}

echo "========================================"
echo "VisionCast RK3588 交叉编译"
echo "========================================"
echo "模式：build=${DO_BUILD}, deploy=${DO_DEPLOY}, clean=${DO_CLEAN}"
echo "ROOT:    ${ROOT_PWD}"
echo "BUILD:   ${BUILD_DIR}"
echo "INSTALL: ${PACKAGE_DIR}"
echo "CXX:     $("${CXX}" --version | head -n1)"

if [[ "${DO_BUILD}" == true ]]; then
    if [[ "${DO_CLEAN}" == true ]]; then
        echo ""
        echo "正在清理构建和安装目录..."
        rm -rf "${BUILD_DIR}" "${PACKAGE_DIR}"
    fi

    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"

    CMAKE_ARGS=(
        "../.."
        "-DCMAKE_SYSTEM_NAME=Linux"
        "-DCMAKE_SYSTEM_PROCESSOR=aarch64"
        "-DCMAKE_CXX_COMPILER=${CXX}"
        "-DCMAKE_FIND_ROOT_PATH=${AARCH64_LIB_DIR}"
        "-DCMAKE_BUILD_TYPE=Release"
        "-DCMAKE_INSTALL_PREFIX=${PACKAGE_DIR}"
        "-DCMAKE_EXE_LINKER_FLAGS=-L${AARCH64_LIB_DIR} -Wl,-rpath-link,${AARCH64_LIB_DIR}"
    )

    echo ""
    echo "正在配置 CMake..."
    cmake "${CMAKE_ARGS[@]}"

    echo ""
    echo "正在编译..."
    make -j"$(nproc)"
    make install

    if [[ -f "${MPP_LIB_DIR}/librockchip_mpp.so.1" ]]; then
        mkdir -p "${PACKAGE_DIR}/lib"
        cp -f "${MPP_LIB_DIR}/librockchip_mpp.so.1" "${PACKAGE_DIR}/lib/"
    elif [[ -f "${PACKAGE_DIR}/lib/librockchip_mpp.so" ]]; then
        ln -sfn librockchip_mpp.so "${PACKAGE_DIR}/lib/librockchip_mpp.so.1"
    fi
    collect_runtime_dependencies
    echo ""
    echo "========================================"
    echo "编译完成"
    echo "========================================"
    echo "安装目录：${PACKAGE_DIR}"
fi

if [[ "${DO_DEPLOY}" == true ]]; then
    DEVICE_IP_ETH="${DEVICE_IP_ETH:-192.168.137.202}"
    DEVICE_IP_WIFI="${DEVICE_IP_WIFI:-192.168.137.111}"
    DEVICE_USER="${DEVICE_USER:-elf}"
    DEVICE_PASS="${DEVICE_PASS:-elf}"
    DEVICE_TARGET_DIR="${DEVICE_TARGET_DIR:-/home/elf/open_project/VisionCast/install}"

    if [[ ! -x "${PACKAGE_DIR}/bin/visioncast" ]]; then
        echo "缺少可执行文件：${PACKAGE_DIR}/bin/visioncast"
        echo "请先执行 ./scripts/build/cross_build.sh --build"
        exit 1
    fi

    # 与 face_attendance 保持一致：网线 IP 优先，失败后尝试 WiFi IP。
    if ping -c 1 -W 1 "${DEVICE_IP_ETH}" >/dev/null 2>&1; then
        DEVICE_IP="${DEVICE_IP_ETH}"
        echo "网络：有线网 (${DEVICE_IP})"
    elif ping -c 1 -W 1 "${DEVICE_IP_WIFI}" >/dev/null 2>&1; then
        DEVICE_IP="${DEVICE_IP_WIFI}"
        echo "网络：WiFi (${DEVICE_IP})"
    else
        echo "两个 RK3588 IP 都无法连通（${DEVICE_IP_ETH} / ${DEVICE_IP_WIFI}）。"
        exit 1
    fi

    # 有 sshpass 时自动输入 elf 密码；没有时退回交互式 ssh/scp。
    if command -v sshpass >/dev/null 2>&1; then
        SSH_CMD=(sshpass -p "${DEVICE_PASS}" ssh -o StrictHostKeyChecking=no)
        SCP_CMD=(sshpass -p "${DEVICE_PASS}" scp -o StrictHostKeyChecking=no)
    else
        echo "未找到 sshpass，切换为交互式 ssh/scp。"
        SSH_CMD=(ssh -o StrictHostKeyChecking=no)
        SCP_CMD=(scp -o StrictHostKeyChecking=no)
    fi

    echo ""
    echo "========================================"
    echo "正在部署 VisionCast 到 RK3588"
    echo "========================================"
    echo "目标：${DEVICE_USER}@${DEVICE_IP}:${DEVICE_TARGET_DIR}"

    "${SSH_CMD[@]}" "${DEVICE_USER}@${DEVICE_IP}" \
        "mkdir -p '${DEVICE_TARGET_DIR}'"

    echo "  -> 正在传输安装包..."
    SCP_ITEMS=(
        "${PACKAGE_DIR}/bin"
        "${PACKAGE_DIR}/config"
        "${PACKAGE_DIR}/scripts"
    )
    if [[ -d "${PACKAGE_DIR}/lib" ]]; then
        SCP_ITEMS+=("${PACKAGE_DIR}/lib")
    fi
    "${SCP_CMD[@]}" -r \
        "${SCP_ITEMS[@]}" \
        "${DEVICE_USER}@${DEVICE_IP}:${DEVICE_TARGET_DIR}/"

    "${SSH_CMD[@]}" "${DEVICE_USER}@${DEVICE_IP}" \
        "chmod +x '${DEVICE_TARGET_DIR}/bin/visioncast'; find '${DEVICE_TARGET_DIR}/scripts' -type f -name '*.sh' -exec chmod +x {} \\;"

    echo ""
    echo "========================================"
    echo "部署完成"
    echo "========================================"
    echo "在 RK3588 上运行："
    echo "  ssh ${DEVICE_USER}@${DEVICE_IP}"
    echo "  cd ${DEVICE_TARGET_DIR}"
    echo "  export LD_LIBRARY_PATH=./lib:\$LD_LIBRARY_PATH"
    echo "  sudo bash scripts/fix_freq_rk3588.sh"
    echo "  ./scripts/run/run_13855_camera.sh"
    echo "  ./scripts/run/run_usb_camera.sh"
else
    echo ""
    echo "跳过部署（--deploy 可部署已有构建产物）。"
fi
