#!/bin/sh
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Smoke test for the vmaf-perShot per-shot CRF predictor sidecar
# (T6-3b / ADR-0222). Invoked from `meson test`; the binary lives
# under <build>/tools/vmaf-perShot.
set -eu

BIN=./tools/vmaf-perShot
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_per_shot.scratch"
mkdir -p "${WORK}"

# Keep bounded-input regressions portable across POSIX hosts. GNU `timeout`
# is not available on stock macOS, while Python is already required below.
run_with_timeout() {
  python3 - "$@" <<'PY'
import subprocess
import sys

try:
    result = subprocess.run(sys.argv[1:], check=False, timeout=5)
except subprocess.TimeoutExpired:
    sys.exit(124)
sys.exit(result.returncode)
PY
}

# Locate the small test fixture shipped under <repo>/testdata/. The
# `vmaf` repo nests `libvmaf/` one directory below the testdata root.
ROOT="${MESON_SOURCE_ROOT:-${PWD}/..}"
SRC="${ROOT}/testdata/ref_576x324_48f.yuv"
if [ ! -f "${SRC}" ]; then
  SRC="${ROOT}/../testdata/ref_576x324_48f.yuv"
fi
if [ ! -f "${SRC}" ]; then
  echo "test_vmaf_per_shot: missing fixture (looked in ${ROOT}/testdata and ${ROOT}/../testdata)" >&2
  exit 77 # meson "skip"
fi

# 1. --help returns 0.
"${BIN}" --help >/dev/null

# 2. Basic invocation produces a CSV plan with at least one shot row.
PLAN_CSV="${WORK}/plan.csv"
"${BIN}" \
  --reference "${SRC}" \
  --width 576 --height 324 \
  --pixel_format 420 --bitdepth 8 \
  --output "${PLAN_CSV}" \
  --target-vmaf 90 \
  --crf-min 18 --crf-max 35

if ! head -n 1 "${PLAN_CSV}" | grep -q "shot_id,start_frame"; then
  echo "test_vmaf_per_shot: missing header in CSV plan" >&2
  cat "${PLAN_CSV}" >&2
  exit 1
fi

ROWS=$(($(wc -l <"${PLAN_CSV}") - 1))
if [ "${ROWS}" -lt 1 ]; then
  echo "test_vmaf_per_shot: expected ≥1 shot row, got ${ROWS}" >&2
  exit 1
fi

# Verify every predicted_crf is inside [18, 35].
awk -F, 'NR>1 { if ($7 < 18 || $7 > 35) { print "CRF out of range:", $0; exit 1 } }' \
  "${PLAN_CSV}"

# 3. JSON format also works.
PLAN_JSON="${WORK}/plan.json"
"${BIN}" \
  --reference "${SRC}" \
  --width 576 --height 324 \
  --pixel_format 420 --bitdepth 8 \
  --output "${PLAN_JSON}" \
  --format json

if ! grep -q '"shots"' "${PLAN_JSON}"; then
  echo "test_vmaf_per_shot: JSON plan missing 'shots' key" >&2
  cat "${PLAN_JSON}" >&2
  exit 1
fi

# 4. Generated 4:2:2 and 4:4:4 fixtures exercise chroma skip sizing.
python3 - "${WORK}" <<'PY'
from pathlib import Path
import sys

work = Path(sys.argv[1])
w = 16
h = 16
luma = w * h

def write_fixture(path: Path, chroma_samples: int) -> None:
    frame0 = bytes([32]) * luma + bytes([128]) * chroma_samples
    frame1 = bytes([224]) * luma + bytes([64]) * chroma_samples
    path.write_bytes(frame0 + frame1)

write_fixture(work / "two_frames_422.yuv", luma)
write_fixture(work / "two_frames_444.yuv", luma * 2)
PY

for PF in 422 444; do
  "${BIN}" \
    --reference "${WORK}/two_frames_${PF}.yuv" \
    --width 16 --height 16 \
    --pixel_format "${PF}" --bitdepth 8 \
    --output "${WORK}/plan_${PF}.csv"
  if ! head -n 1 "${WORK}/plan_${PF}.csv" | grep -q "shot_id,start_frame"; then
    echo "test_vmaf_per_shot: ${PF} plan missing CSV header" >&2
    cat "${WORK}/plan_${PF}.csv" >&2
    exit 1
  fi
done

# 5. Invalid args fail with non-zero.
if "${BIN}" --reference /tmp/nope --width 0 --height 0 \
  --pixel_format 420 --bitdepth 8 \
  --output /tmp/out 2>/dev/null; then
  echo "test_vmaf_per_shot: expected failure on invalid width" >&2
  exit 1
fi

if "${BIN}" --reference "${WORK}/two_frames_422.yuv" --width 16 --height 16 \
  --pixel_format 411 --bitdepth 8 \
  --output /tmp/out 2>/dev/null; then
  echo "test_vmaf_per_shot: expected failure on unsupported pixel_format" >&2
  exit 1
fi

if "${BIN}" --reference "${WORK}/two_frames_422.yuv" --width 16 --height 16 \
  --pixel_format 422 --bitdepth 9 \
  --output /tmp/out 2>/dev/null; then
  echo "test_vmaf_per_shot: expected failure on unsupported bitdepth" >&2
  exit 1
fi

# 6. Unknown / unrecognised options must fail, not silently trigger --help.
#    Before the fix, --typo-option returned 1 (success-with-help), masking typos.
if "${BIN}" --typo-option 2>/dev/null; then
  echo "test_vmaf_per_shot: expected failure on unrecognised option --typo-option" >&2
  exit 1
fi

# 7. --frames N bounds finite input.
PLAN_FRAMES_CSV="${WORK}/plan_frames_10.csv"
"${BIN}" \
  --reference "${SRC}" \
  --width 576 --height 324 \
  --pixel_format 420 --bitdepth 8 \
  --output "${PLAN_FRAMES_CSV}" \
  --frames 10

FRAMES_TOTAL=$(awk -F, 'NR>1 { sum += $4 } END { print sum }' "${PLAN_FRAMES_CSV}")
if [ "${FRAMES_TOTAL}" -ne 10 ]; then
  echo "test_vmaf_per_shot: expected 10 frames total with --frames 10, got ${FRAMES_TOTAL}" >&2
  exit 1
fi

# 8. --frames 0 preserves full scan (unbounded compatibility contract).
PLAN_UNBOUNDED_CSV="${WORK}/plan_unbounded.csv"
"${BIN}" \
  --reference "${SRC}" \
  --width 576 --height 324 \
  --pixel_format 420 --bitdepth 8 \
  --output "${PLAN_UNBOUNDED_CSV}" \
  --frames 0

UNBOUNDED_TOTAL=$(awk -F, 'NR>1 { sum += $4 } END { print sum }' "${PLAN_UNBOUNDED_CSV}")
if [ "${UNBOUNDED_TOTAL}" -ne 48 ]; then
  echo "test_vmaf_per_shot: expected 48 frames total with --frames 0, got ${UNBOUNDED_TOTAL}" >&2
  exit 1
fi

# 9. Flag aliases: -F, --frame_cnt, --max-frames behave identically.
for ALIAS_FLAG in "-F" "--frame_cnt" "--max-frames"; do
  ALIAS_CSV="${WORK}/plan_alias.csv"
  "${BIN}" \
    --reference "${SRC}" \
    --width 576 --height 324 \
    --pixel_format 420 --bitdepth 8 \
    --output "${ALIAS_CSV}" \
    ${ALIAS_FLAG} 10
  ALIAS_TOTAL=$(awk -F, 'NR>1 { sum += $4 } END { print sum }' "${ALIAS_CSV}")
  if [ "${ALIAS_TOTAL}" -ne 10 ]; then
    echo "test_vmaf_per_shot: alias ${ALIAS_FLAG} expected 10 frames, got ${ALIAS_TOTAL}" >&2
    exit 1
  fi
done

# 10. Bounded read on /dev/zero must terminate promptly and produce requested frames.
DEV_ZERO_CSV="${WORK}/plan_dev_zero.csv"
run_with_timeout "${BIN}" \
  --reference /dev/zero \
  --width 16 --height 16 \
  --pixel_format 420 --bitdepth 8 \
  --output "${DEV_ZERO_CSV}" \
  --frames 6

DEV_ZERO_FRAMES=$(awk -F, 'NR>1 { sum += $4 } END { print sum }' "${DEV_ZERO_CSV}")
if [ "${DEV_ZERO_FRAMES}" -ne 6 ]; then
  echo "test_vmaf_per_shot: expected 6 frames from /dev/zero, got ${DEV_ZERO_FRAMES}" >&2
  exit 1
fi

# 11. Bounded read on endless FIFO must terminate cleanly and promptly (cannot hang).
FIFO_TEST="${WORK}/endless_fifo.yuv"
rm -f "${FIFO_TEST}"
mkfifo "${FIFO_TEST}"
yes 2>/dev/null | tr -d '\n' >"${FIFO_TEST}" 2>/dev/null &
FIFO_WRITER_PID=$!
FIFO_CSV="${WORK}/plan_fifo.csv"
if ! run_with_timeout "${BIN}" \
  --reference "${FIFO_TEST}" \
  --width 16 --height 16 \
  --pixel_format 420 --bitdepth 8 \
  --output "${FIFO_CSV}" \
  --frames 5; then
  echo "test_vmaf_per_shot: FIFO test timed out or failed" >&2
  kill -9 "${FIFO_WRITER_PID}" 2>/dev/null || true
  rm -f "${FIFO_TEST}"
  exit 1
fi
kill -9 "${FIFO_WRITER_PID}" 2>/dev/null || true
wait "${FIFO_WRITER_PID}" 2>/dev/null || true
rm -f "${FIFO_TEST}"

FIFO_FRAMES=$(awk -F, 'NR>1 { sum += $4 } END { print sum }' "${FIFO_CSV}")
if [ "${FIFO_FRAMES}" -ne 5 ]; then
  echo "test_vmaf_per_shot: expected 5 frames from endless FIFO, got ${FIFO_FRAMES}" >&2
  exit 1
fi

# 12. Invalid --frames arguments fail.
CARRIAGE_RETURN=$(printf '\r')
LINE_FEED=$(printf '\n_')
LINE_FEED=${LINE_FEED%_}
for INVALID_FRAMES in \
  "-1" \
  "-18446744073709551615" \
  "-4294967295" \
  "${CARRIAGE_RETURN}-18446744073709551615" \
  "${LINE_FEED}-18446744073709551615" \
  "${CARRIAGE_RETURN}-4294967295" \
  "${CARRIAGE_RETURN}-1" \
  "   " \
  "4294967296" \
  "not_a_number"; do
  if "${BIN}" --reference "${SRC}" --width 576 --height 324 \
    --pixel_format 420 --bitdepth 8 \
    --output "${WORK}/out.csv" \
    --frames "${INVALID_FRAMES}" 2>/dev/null; then
    echo "test_vmaf_per_shot: expected failure on invalid --frames: ${INVALID_FRAMES}" >&2
    exit 1
  fi
done

echo "test_vmaf_per_shot: PASS (${ROWS} shot rows)"
