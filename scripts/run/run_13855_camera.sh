#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BIN="${INSTALL_DIR}/bin/visioncast"
CONFIG="${INSTALL_DIR}/config/visioncast_config.json"

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

# Set sensor subdevice frame rate to 30 FPS if media-ctl and v4l2-ctl are available
if command -v media-ctl >/dev/null 2>&1 && command -v v4l2-ctl >/dev/null 2>&1; then
    SUBDEV=$(media-ctl -d /dev/media0 -e 'm00_b_ov13855 3-0036' 2>/dev/null || echo "")
    if [[ -n "${SUBDEV}" && -c "${SUBDEV}" ]]; then
        echo "Configuring camera sensor ${SUBDEV} to 30 FPS..."
        v4l2-ctl -d "${SUBDEV}" --set-subdev-fps pad=0,fps=30 || true
    fi
fi

export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH:-}"
cd "${INSTALL_DIR}"
exec "${BIN}" "${ARGS[@]}" "$@"
