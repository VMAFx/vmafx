#!/bin/sh
# Smoke test for vmaf-roi high-bit-depth YUV frame seeking / luma load.
set -eu

BIN=./tools/vmaf_roi
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_roi_high_bitdepth.scratch"
mkdir -p "${WORK}"

SRC="${WORK}/two_frames_4x4_420p10le.yuv"
OUT="${WORK}/roi_frame1.qp"

python3 - "${SRC}" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])

def put16(values):
    out = bytearray()
    for value in values:
        out += int(value).to_bytes(2, "little")
    return out

# Two 4x4 yuv420p10le frames. Chroma content is irrelevant for vmaf-roi
# saliency, but frame-byte accounting must skip it correctly to reach
# frame 1.
frame0_y = [64] * 16
frame0_uv = [128] * 8
frame1_y = [896] * 16
frame1_uv = [512] * 8
path.write_bytes(
    put16(frame0_y) + put16(frame0_uv) +
    put16(frame1_y) + put16(frame1_uv)
)
PY

"${BIN}" \
  --reference "${SRC}" \
  --width 4 --height 4 \
  --frame 1 \
  --pixel_format 420 --bitdepth 10 \
  --ctu-size 8 \
  --encoder x265 \
  --output "${OUT}"

if ! grep -q "# frame=1 ctu=8 cols=1 rows=1" "${OUT}"; then
  echo "test_vmaf_roi_high_bitdepth: missing expected x265 header" >&2
  cat "${OUT}" >&2
  exit 1
fi

ROWS=$(grep -cv '^#' "${OUT}")
if [ "${ROWS}" -ne 1 ]; then
  echo "test_vmaf_roi_high_bitdepth: expected one CTU row, got ${ROWS}" >&2
  cat "${OUT}" >&2
  exit 1
fi

if "${BIN}" \
  --reference "${SRC}" \
  --width 4 --height 4 \
  --frame 2 \
  --pixel_format 420 --bitdepth 10 \
  --ctu-size 8 \
  --encoder x265 \
  --output "${WORK}/short.qp" 2>/dev/null; then
  echo "test_vmaf_roi_high_bitdepth: expected frame 2 to fail short read" >&2
  exit 1
fi

if "${BIN}" \
  --reference "${SRC}" \
  --width 4 --height 4 \
  --frame 1 \
  --pixel_format 420 --bitdepth 9 \
  --output "${WORK}/bad.qp" 2>/dev/null; then
  echo "test_vmaf_roi_high_bitdepth: expected --bitdepth 9 to fail" >&2
  exit 1
fi

echo "test_vmaf_roi_high_bitdepth: PASS"
