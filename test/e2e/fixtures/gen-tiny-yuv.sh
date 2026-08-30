#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# test/e2e/fixtures/gen-tiny-yuv.sh
#
# Generate a pair of tiny synthetic YUV420p clips for e2e tests.
# Output: test/e2e/fixtures/ref.yuv and test/e2e/fixtures/dist.yuv
#
# Spec:
#   Resolution: 64x64
#   Frames:     8
#   Format:     YUV420p 8-bit
#   ref.yuv:    solid grey (Y=128, U=128, V=128)
#   dist.yuv:   solid grey with a 5% brightness reduction (Y=121)
#
# The clips are small enough (~24 KiB each) to commit to git.
# If the files already exist, the script exits without regenerating them.
#
# ADR-0783.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REF="${SCRIPT_DIR}/ref.yuv"
DIST="${SCRIPT_DIR}/dist.yuv"

WIDTH=64
HEIGHT=64
FRAMES=8

if [ -f "${REF}" ] && [ -f "${DIST}" ]; then
  echo "Fixtures already present — skipping generation."
  exit 0
fi

echo "Generating tiny YUV420p fixtures (${WIDTH}x${HEIGHT}, ${FRAMES} frames)..."

# Require python3 for deterministic byte generation.
python3 - <<PYEOF
import struct, os

width, height, frames = ${WIDTH}, ${HEIGHT}, ${FRAMES}
y_size = width * height
uv_size = width * height // 4
frame_size = y_size + 2 * uv_size

def write_yuv(path, y_val, u_val=128, v_val=128):
    with open(path, 'wb') as f:
        for _ in range(frames):
            f.write(bytes([y_val]) * y_size)
            f.write(bytes([u_val]) * uv_size)
            f.write(bytes([v_val]) * uv_size)

write_yuv('${REF}', y_val=128)
write_yuv('${DIST}', y_val=121)  # ~5% dimmer
print(f"Wrote {os.path.getsize('${REF}')} bytes to ${REF}")
print(f"Wrote {os.path.getsize('${DIST}')} bytes to ${DIST}")
PYEOF

echo "Fixture generation complete."
