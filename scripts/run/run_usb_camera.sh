#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BIN="${INSTALL_DIR}/bin/visioncast"
CONFIG="${INSTALL_DIR}/config/video_usb_c270.json"

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

export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH:-}"
cd "${INSTALL_DIR}"
exec "${BIN}" "${ARGS[@]}" "$@"
