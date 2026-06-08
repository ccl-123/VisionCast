# Opus

This directory contains the Opus 1.3.1 development headers and aarch64
libraries used by VisionCast RTP/WebRTC audio.

- Headers: Ubuntu 22.04 `libopus-dev:arm64` package
- Shared library: Ubuntu 22.04 `libopus0:arm64` package
- ABI/SONAME: `libopus.so.0`
- Upstream: <https://opus-codec.org/>
- License: BSD, see `COPYING`

VisionCast links this library for S16_LE PCM to Opus encoding. The encoded
frames use RTP payload type 111 and a 48 kHz RTP clock.
