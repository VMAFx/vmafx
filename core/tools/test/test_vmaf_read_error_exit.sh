#!/bin/sh
# ADR-1262 — a failed input read must not exit 0.
#
# Before ADR-1262 `run_frame_loop()` reported only a frame count, so every way
# a read could fail looked to main() exactly like a clean end of stream: the
# binary exited 0 and wrote a full report over whatever prefix had arrived.
# Two corrupt inputs were additionally classified as "both streams ended",
# because the `ret1 && ret2` test ran before the `ret1 < 0 || ret2 < 0` one and
# both -1 values satisfy it, so not even a diagnostic was printed.
#
# The four cases below pin the distinction the fix draws: a stream that FAILS
# to read is an error (exit 102), a stream that legitimately ENDS earlier than
# its partner stays a warning (exit 0).
set -eu

BIN=./tools/vmaf
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_read_error_exit.scratch"
rm -rf "${WORK}"
mkdir -p "${WORK}"

python3 - "${WORK}" <<'PY'
from pathlib import Path
import sys

work = Path(sys.argv[1])

# 24x24 4:2:0 8-bit: 24*24 luma + 2 * 12*12 chroma = 864 bytes per frame.
W = H = 24
FRAME = W * H + 2 * (W // 2) * (H // 2)
HEADER = b"YUV4MPEG2 W%d H%d F25:1 Ip A1:1 C420jpeg\n" % (W, H)

# Two visibly different frames so psnr has something to chew on; the exact
# content is irrelevant to the exit status under test.
frame0 = bytes((i * 7 + 16) % 256 for i in range(FRAME))
frame1 = bytes((i * 11 + 96) % 256 for i in range(FRAME))


def y4m(*frames):
    return HEADER + b"".join(b"FRAME\n" + f for f in frames)


for tag in ("ref", "dis"):
    # Two whole frames -- the control.
    (work / f"clean_{tag}.y4m").write_bytes(y4m(frame0, frame1))
    # Frame 2's payload cut short: the reader hits EOF mid-frame and fails.
    (work / f"trunc_{tag}.y4m").write_bytes(
        y4m(frame0) + b"FRAME\n" + frame1[: FRAME // 2]
    )

# One whole frame, cleanly terminated: a legitimately shorter stream.
(work / "short_ref.y4m").write_bytes(y4m(frame0))
PY

# Run $BIN and report its exit status without tripping `set -e`.
run_status() {
  rm -f "${WORK}/out.json"
  status=0
  "${BIN}" -r "$1" -d "$2" \
    --feature psnr --no_prediction \
    --json -o "${WORK}/out.json" >/dev/null 2>"${WORK}/err.txt" || status=$?
  echo "${status}"
}

fail() {
  echo "FAIL: $1" >&2
  echo "--- stderr ---" >&2
  cat "${WORK}/err.txt" >&2 || true
  exit 1
}

# 1. Both inputs clean: success, and a report is written.
got=$(run_status "${WORK}/clean_ref.y4m" "${WORK}/clean_dis.y4m")
[ "${got}" = "0" ] || fail "two clean streams exited ${got}, expected 0"
[ -s "${WORK}/out.json" ] || fail "two clean streams wrote no report"

# 2. Both inputs truncated. This is the case the old ordering mis-classified
#    as a clean end of stream, so it is the one that regressed silently.
got=$(run_status "${WORK}/trunc_ref.y4m" "${WORK}/trunc_dis.y4m")
[ "${got}" = "102" ] || fail "two truncated streams exited ${got}, expected 102"
grep -q "problem while reading pictures" "${WORK}/err.txt" ||
  fail "two truncated streams printed no read diagnostic"
[ ! -e "${WORK}/out.json" ] ||
  fail "a report was written for a run that failed to read its input"

# 3. Only the reference truncated: same verdict.
got=$(run_status "${WORK}/trunc_ref.y4m" "${WORK}/clean_dis.y4m")
[ "${got}" = "102" ] || fail "one truncated stream exited ${got}, expected 102"

# 4. A legitimately shorter reference is NOT an error: scoring the common
#    prefix stays supported, and stays exit 0.
got=$(run_status "${WORK}/short_ref.y4m" "${WORK}/clean_dis.y4m")
[ "${got}" = "0" ] || fail "a legitimately shorter stream exited ${got}, expected 0"
grep -q "ended before" "${WORK}/err.txt" ||
  fail "a legitimately shorter stream printed no 'ended before' warning"
[ -s "${WORK}/out.json" ] || fail "a legitimately shorter stream wrote no report"

echo "test_vmaf_read_error_exit: all 4 cases pass"
