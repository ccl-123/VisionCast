#!/usr/bin/env bash
set -euo pipefail

if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl --list-devices
    for i in {11..17} 21 22; do
        if [[ -e "/dev/video${i}" ]]; then
            echo
            echo "===== /dev/video${i} ====="
            v4l2-ctl -d "/dev/video${i}" -D || true
            v4l2-ctl -d "/dev/video${i}" --list-formats-ext || true
        fi
    done
else
    echo "v4l2-ctl not found"
    exit 1
fi
