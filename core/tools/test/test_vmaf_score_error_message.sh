#!/bin/sh
# SPDX-License-Identifier: EUPL-1.2
# T-CLI-EXTRACTOR-ERROR-PROPAGATION-2026-10-05 — a frame libvmaf fails to score
# is not an input read failure, and the two messages must not read alike.
#
# `vmaf_read_pictures()` returned an extractor's refusal and the CLI printed
# "problem reading pictures", next to the "problem while reading pictures" it
# prints when a stream FAILS to read. Three cases pin the distinction:
#   positive: an extractor that refuses the frame (float_ms_ssim below its
#             176x176 floor) ends the run with exit 234 and
#             "problem scoring picture 0";
#   negative: the old wording does not appear;
#   boundary: a truncated input still says "problem while reading pictures"
#             and exits 102 (ADR-1262), with no scoring message.
set -eu

BIN=./tools/vmaf
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_score_error_message.scratch"
rm -rf "${WORK}"
mkdir -p "${WORK}"

python3 - "${WORK}" <<'PY'
from pathlib import Path
import sys

work = Path(sys.argv[1])
W = H = 16
FRAME = W * H + 2 * (W // 2) * (H // 2)
HEADER = b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (W, H)
frame0 = bytes((i * 7 + 16) % 256 for i in range(FRAME))
frame1 = bytes((i * 11 + 96) % 256 for i in range(FRAME))


def y4m(*frames):
    return HEADER + b"".join(b"FRAME\n" + f for f in frames)


for tag in ("ref", "dis"):
    (work / f"clean_{tag}.y4m").write_bytes(y4m(frame0, frame1))
    (work / f"trunc_{tag}.y4m").write_bytes(y4m(frame0) + b"FRAME\n" + frame1[: FRAME // 2])
PY

fail() {
  echo "FAIL: $1" >&2
  echo "--- stderr ---" >&2
  cat "${WORK}/err.txt" >&2
  exit 1
}

# run_case <ref> <dis> <feature>: stores stderr, sets $status.
run_case() {
  status=0
  "${BIN}" -r "$1" -d "$2" --feature "$3" --no_prediction \
    --json -o "${WORK}/out.json" >/dev/null 2>"${WORK}/err.txt" || status=$?
}

# Positive: float_ms_ssim refuses 16x16.
run_case "${WORK}/clean_ref.y4m" "${WORK}/clean_dis.y4m" float_ms_ssim
[ "${status}" -eq 234 ] || fail "scoring failure: expected exit 234, got ${status}"
grep -q 'problem scoring picture 0' "${WORK}/err.txt" || fail "no 'problem scoring picture 0'"
# Negative: the old wording is gone; the read-failure wording is not borrowed.
if grep -q 'problem reading pictures' "${WORK}/err.txt"; then
  fail "a scoring failure still says 'problem reading pictures'"
fi
if grep -q 'problem while reading pictures' "${WORK}/err.txt"; then
  fail "a scoring failure says 'problem while reading pictures'"
fi

# Boundary: a truncated stream is still a read failure.
run_case "${WORK}/trunc_ref.y4m" "${WORK}/trunc_dis.y4m" psnr
[ "${status}" -eq 102 ] || fail "read failure: expected exit 102, got ${status}"
grep -q 'problem while reading pictures' "${WORK}/err.txt" || fail "no 'problem while reading pictures'"
if grep -q 'problem scoring picture' "${WORK}/err.txt"; then
  fail "a read failure says 'problem scoring picture'"
fi

rm -rf "${WORK}"
echo "OK"
