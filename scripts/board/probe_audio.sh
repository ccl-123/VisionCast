#!/usr/bin/env bash
set -euo pipefail

if command -v arecord >/dev/null 2>&1; then
    arecord -l || true
    if command -v timeout >/dev/null 2>&1; then
        timeout 2s arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 --dump-hw-params /dev/null 2>&1 \
            | sed '/Aborted by signal/d' || true
    else
        echo "timeout not found; skip arecord --dump-hw-params to avoid blocking capture."
    fi
else
    echo "arecord not found"
fi

echo
cat /proc/asound/cards 2>/dev/null || true
echo
cat /proc/asound/pcm 2>/dev/null || true
