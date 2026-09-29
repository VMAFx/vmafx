#!/bin/sh
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# ADR-1366 — frame read-ahead must not change which frames are scored, in
# which order, or how a run ends.
#
# `vmaf` reads the reference and the distorted stream on two reader threads, a
# bounded number of frames ahead of the scoring loop. Every frame below has a
# constant luma plane and each reference/distorted pair differs by a known
# amount, so every per-frame psnr_y is known in advance: a frame delivered out
# of order, paired with the wrong partner, dropped or duplicated changes the
# value at its index. The cases cover the threaded path (two distinct files),
# the inline path (the same file on both sides), a worker pool larger than the
# clip, --frame_cnt, --frame_skip_*, a stream that ends early and a stream
# that fails to read.
set -eu

BIN=./tools/vmaf
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_frame_readahead.scratch"
rm -rf "${WORK}"
mkdir -p "${WORK}"

python3 - "${WORK}" <<'PY'
from pathlib import Path
import sys

work = Path(sys.argv[1])
W, H, N = 64, 48, 40
HEADER = b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (W, H)
CHROMA = bytes([128]) * (2 * (W // 2) * (H // 2))


def frame(luma):
    return b"FRAME\n" + bytes([luma]) * (W * H) + CHROMA


def ref_luma(i):
    return 50 + 3 * i


def dis_luma(i):
    return ref_luma(i) + 1 + (7 * i) % 11


ref = HEADER + b"".join(frame(ref_luma(i)) for i in range(N))
dis = HEADER + b"".join(frame(dis_luma(i)) for i in range(N))
(work / "ref.y4m").write_bytes(ref)
(work / "dis.y4m").write_bytes(dis)
frame_len = len(frame(0))
# A reference that ENDS after 25 frames, and a distorted stream whose frame 30
# is cut short (a read error).
(work / "ref_short.y4m").write_bytes(ref[: len(HEADER) + 25 * frame_len])
(work / "dis_trunc.y4m").write_bytes(dis[: len(HEADER) + 30 * frame_len + frame_len // 2])
PY

fail() {
  echo "FAIL: $1" >&2
  echo "--- stderr ---" >&2
  cat "${WORK}/err.txt" >&2 || true
  exit 1
}

# run NAME REF DIS [extra args...] -> sets $status; report in ${WORK}/NAME.json
run() {
  name="$1"
  ref="$2"
  dis="$3"
  shift 3
  status=0
  "${BIN}" -r "${ref}" -d "${dis}" --feature psnr --no_prediction --precision max \
    --json -o "${WORK}/${name}.json" "$@" >/dev/null 2>"${WORK}/err.txt" || status=$?
}

# check NAME FRAMES SKIP_REF SKIP_DIS: the report holds exactly FRAMES frames
# and frame n carries the psnr_y of reference frame n+SKIP_REF against
# distorted frame n+SKIP_DIS.
check() {
  python3 - "${WORK}/$1.json" "$2" "$3" "$4" <<'PY' || fail "$1: per-frame psnr_y mismatch"
import json
import math
import sys

path, frames, skip_ref, skip_dis = sys.argv[1], *map(int, sys.argv[2:])
got = json.load(open(path))["frames"]
if len(got) != frames:
    sys.exit(f"{path}: {len(got)} frames, expected {frames}")
for n, entry in enumerate(got):
    if entry["frameNum"] != n:
        sys.exit(f"{path}: frame {n} reports frameNum {entry['frameNum']}")
    i, j = n + skip_ref, n + skip_dis
    diff = abs((50 + 3 * i) - (50 + 3 * j + 1 + (7 * j) % 11))
    psnr = entry["metrics"]["psnr_y"]
    if diff == 0:
        sys.exit(f"{path}: test fixture produced an identical pair at frame {n}")
    want = 10.0 * math.log10(255.0 * 255.0 / (diff * diff))
    if abs(psnr - want) > 1e-6:
        sys.exit(f"{path}: frame {n} psnr_y {psnr!r}, expected {want!r}")
PY
}

# 1. Two distinct files: both streams are read on reader threads.
run threaded "${WORK}/ref.y4m" "${WORK}/dis.y4m"
[ "${status}" = "0" ] || fail "threaded run exited ${status}"
check threaded 40 0 0

# 2. A worker pool larger than the clip: the picture pool is at its largest and
#    the readers race the workers for pictures.
run threaded_pool "${WORK}/ref.y4m" "${WORK}/dis.y4m" --threads 8
[ "${status}" = "0" ] || fail "--threads 8 run exited ${status}"
check threaded_pool 40 0 0

# 3. --frame_cnt stops both readers after exactly that many frames.
run frame_cnt "${WORK}/ref.y4m" "${WORK}/dis.y4m" --frame_cnt 7
[ "${status}" = "0" ] || fail "--frame_cnt run exited ${status}"
check frame_cnt 7 0 0

# 4. Skipped frames are consumed before the readers start.
run skip "${WORK}/ref.y4m" "${WORK}/dis.y4m" --frame_skip_dist 1 --threads 2
[ "${status}" = "0" ] || fail "--frame_skip_dist run exited ${status}"
check skip 39 0 1

# 5. The same file on both sides is read inline (one object opened twice), and
#    still scores all 40 frames. Identical pairs report psnr_y's ceiling.
status=0
"${BIN}" -r "${WORK}/dis.y4m" -d "${WORK}/dis.y4m" --feature psnr --no_prediction \
  --json -o "${WORK}/inline.json" >/dev/null 2>"${WORK}/err.txt" || status=$?
[ "${status}" = "0" ] || fail "same-file run exited ${status}"
python3 - "${WORK}/inline.json" <<'PY' || fail "same-file run reported the wrong frames"
import json
import sys

frames = json.load(open(sys.argv[1]))["frames"]
if len(frames) != 40 or len({f["metrics"]["psnr_y"] for f in frames}) != 1:
    sys.exit(f"{len(frames)} frames, psnr_y values {sorted({f['metrics']['psnr_y'] for f in frames})}")
PY

# 6. A reference that ends early: the common prefix is scored, exit 0.
run short "${WORK}/ref_short.y4m" "${WORK}/dis.y4m" --threads 2
[ "${status}" = "0" ] || fail "short-reference run exited ${status}, expected 0"
grep -q "ended before" "${WORK}/err.txt" || fail "short-reference run printed no 'ended before'"
check short 25 0 0

# 7. A distorted stream that fails to read: exit 102 and no report, even though
#    the reference reader may already have read frames past the failure.
run trunc "${WORK}/ref.y4m" "${WORK}/dis_trunc.y4m"
[ "${status}" = "102" ] || fail "truncated-distorted run exited ${status}, expected 102"
grep -q "problem while reading pictures" "${WORK}/err.txt" ||
  fail "truncated-distorted run printed no read diagnostic"
[ ! -e "${WORK}/trunc.json" ] || fail "a report was written for a run that failed to read"

# 8. --frame_cnt bounds the readers, not just the scorer: with the damaged frame
#    30 just past the limit, no reader may reach it, so the run succeeds and the
#    Y4M reader never reports the truncated frame.
run trunc_limit "${WORK}/ref.y4m" "${WORK}/dis_trunc.y4m" --frame_cnt 30
[ "${status}" = "0" ] || fail "--frame_cnt 30 before the damaged frame exited ${status}"
if grep -q "Error reading" "${WORK}/err.txt"; then
  fail "a reader read past --frame_cnt"
fi
check trunc_limit 30 0 0

echo "test_vmaf_frame_readahead: all 8 cases pass"
