#!/usr/bin/env bash
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
#
# test/e2e/fixtures/gen-tiny-yuv.sh
#
# Generate a pair of small synthetic YUV420p clips for e2e tests.
# Output: test/e2e/fixtures/{ref,dist}.{yuv,y4m}
#
# Spec:
#   Resolution: 216x160
#   Frames:     8
#   Format:     YUV420p 8-bit
#   ref.yuv:    solid grey (Y=128, U=128, V=128)
#   dist.yuv:   solid grey with a 5% brightness reduction (Y=121)
#
# 216x160 is the smallest 4:2:0 geometry the default model accepts. The
# server scores with vmaf_v1.0.16_3d0h (ADR-1169), whose cambi feature needs
# one axis >= 216 and whose speed_chroma feature needs both chroma planes
# >= 80, i.e. luma >= 160 on both axes. Below that the CLI refuses the input
# and /v1/score answers 500. scripts/ci/test_e2e_runtime_contract.py checks
# this geometry against the thresholds in core/src/feature/.
#
# The raw clips are committed; solid frames compress to almost nothing in
# git. The Y4M wrappers are regenerated for each run so the vmafx-server can
# infer the frame geometry from their headers without adding dimensions to
# its REST contract.
#
# ADR-0783.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REF="${SCRIPT_DIR}/ref.yuv"
DIST="${SCRIPT_DIR}/dist.yuv"
REF_Y4M="${SCRIPT_DIR}/ref.y4m"
DIST_Y4M="${SCRIPT_DIR}/dist.y4m"

WIDTH=216
HEIGHT=160
FRAMES=8

echo "Preparing YUV420p fixtures (${WIDTH}x${HEIGHT}, ${FRAMES} frames)..."

# Require python3 for deterministic byte generation and Y4M framing. The raw
# files remain committed evidence; an unexpected size fails closed instead of
# silently producing a malformed scoring request.
python3 - "${WIDTH}" "${HEIGHT}" "${FRAMES}" \
  "${REF}" "${DIST}" "${REF_Y4M}" "${DIST_Y4M}" <<'PYEOF'
import hashlib
import os
import sys

width, height, frames = (int(value) for value in sys.argv[1:4])
y_size = width * height
uv_size = width * height // 4
frame_size = y_size + 2 * uv_size
expected_size = frame_size * frames

def ensure_raw(path, expected_digest):
    if not os.path.isfile(path):
        raise SystemExit(f"committed fixture is missing: {path}")
    actual_size = os.path.getsize(path)
    if actual_size != expected_size:
        raise SystemExit(
            f"fixture {path} has {actual_size} bytes; expected {expected_size}"
        )
    with open(path, "rb") as fixture:
        actual_digest = hashlib.sha256(fixture.read()).hexdigest()
    if actual_digest != expected_digest:
        raise SystemExit(
            f"fixture {path} has SHA-256 {actual_digest}; expected {expected_digest}"
        )

def write_y4m(raw_path, y4m_path):
    header = f"YUV4MPEG2 W{width} H{height} F24:1 Ip A0:0 C420jpeg\n".encode()
    with open(raw_path, "rb") as raw, open(y4m_path, "wb") as y4m:
        y4m.write(header)
        for frame_index in range(frames):
            frame = raw.read(frame_size)
            if len(frame) != frame_size:
                raise SystemExit(f"fixture {raw_path} ended at frame {frame_index}")
            y4m.write(b"FRAME\n")
            y4m.write(frame)
        if raw.read(1):
            raise SystemExit(f"fixture {raw_path} contains trailing data")

ref, dist, ref_y4m, dist_y4m = sys.argv[4:]
ensure_raw(ref, "086f54a3241754a0d89ffe899dcc57ea16ae6030f860795a5b18e6181c57a5a2")
ensure_raw(dist, "3b5e57077dc0d41f95dc8f3d80f7d5895b58398b83cdb8ac6e1bfc27957e031a")
write_y4m(ref, ref_y4m)
write_y4m(dist, dist_y4m)

for path in (ref, dist, ref_y4m, dist_y4m):
    print(f"Prepared {os.path.getsize(path)} bytes at {path}")
PYEOF

echo "Fixture generation complete."
