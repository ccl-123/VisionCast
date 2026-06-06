#!/usr/bin/env bash
set -euo pipefail

dmesg | grep -Ei "13855|ov13855|camera|mipi|csi|rkcif|rkisp" || true

if command -v media-ctl >/dev/null 2>&1; then
    for dev in /dev/media0 /dev/media1; do
        if [[ -e "${dev}" ]]; then
            echo
            echo "===== ${dev} ====="
            media-ctl -p -d "${dev}" || true
        fi
    done
else
    echo "media-ctl not found"
fi
