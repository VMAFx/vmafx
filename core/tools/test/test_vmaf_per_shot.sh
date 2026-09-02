#!/bin/sh
# Smoke test for the vmaf-perShot per-shot CRF predictor sidecar
# (T6-3b / ADR-0222). Invoked from `meson test`; the binary lives
# under <build>/tools/vmaf-perShot.
set -eu

BIN=./tools/vmaf-perShot
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_per_shot.scratch"
mkdir -p "${WORK}"

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

echo "test_vmaf_per_shot: PASS (${ROWS} shot rows)"
