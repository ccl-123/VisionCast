#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
if [[ ! -x "${INSTALL_DIR}/bin/visioncast" &&
      -x "${INSTALL_DIR}/install/visioncast/bin/visioncast" ]]; then
    INSTALL_DIR="${INSTALL_DIR}/install/visioncast"
fi
BIN="${INSTALL_DIR}/bin/visioncast"
CONFIG="${INSTALL_DIR}/config/visioncast_config.json"
export VISIONCAST_CAMERA_PROFILE="mipi_13855"

if [[ ! -x "${BIN}" ]]; then
    echo "visioncast executable not found: ${BIN}"
    exit 1
fi

PROTOCOL="${VISIONCAST_PROTOCOL:-${1:-}}"
if [[ $# -gt 0 && "${1}" != --* ]]; then
    shift
fi

ARGS=(--config "${CONFIG}")
if [[ -n "${PROTOCOL}" ]]; then
    ARGS+=(--protocol "${PROTOCOL}")
fi
if [[ -n "${RTMP_URL:-}" ]]; then
    ARGS+=(--rtmp-url "${RTMP_URL}")
fi
if [[ -n "${WEBRTC_URL:-}" ]]; then
    ARGS+=(--webrtc-url "${WEBRTC_URL}")
fi
if [[ -n "${SERVER_IP:-}" ]]; then
    ARGS+=(--server-ip "${SERVER_IP}")
fi
if [[ -n "${VIDEO_PORT:-}" ]]; then
    ARGS+=(--video-port "${VIDEO_PORT}")
fi
if [[ -n "${AUDIO_PORT:-}" ]]; then
    ARGS+=(--audio-port "${AUDIO_PORT}")
fi
if [[ -n "${SDP_PATH:-}" ]]; then
    ARGS+=(--sdp-path "${SDP_PATH}")
fi

# SSH and non-interactive shells don't inherit the active GNOME session.
if [[ -z "${DISPLAY:-}" && -S /tmp/.X11-unix/X0 ]]; then
    export DISPLAY=:0
fi
if [[ -z "${XDG_RUNTIME_DIR:-}" && -d "/run/user/$(id -u)" ]]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi
if [[ -z "${XAUTHORITY:-}" && -n "${XDG_RUNTIME_DIR:-}" ]]; then
    for auth_file in "${XDG_RUNTIME_DIR}"/.mutter-Xwaylandauth.*; do
        if [[ -r "${auth_file}" ]]; then
            export XAUTHORITY="${auth_file}"
            break
        fi
    done
fi

# 清理代理环境变量，防止本地推流流量被代理软件拦截导致 502/连接挂起
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY

export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH:-}"
cd "${INSTALL_DIR}"
exec "${BIN}" "${ARGS[@]}" "$@"
