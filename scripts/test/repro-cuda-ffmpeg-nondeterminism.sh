#!/usr/bin/env bash
# repro-cuda-ffmpeg-nondeterminism.sh — reproduce T-CUDA-FFMPEG-FILTER-NONDETERMINISM-2026-09-06.
#
# FFmpeg's `libvmaf_cuda` filter intermittently returns a pooled VMAF score that
# is wrong by ~0.5-3 points. The defect is timing-dependent, which is why it was
# originally recorded as an unexplained 10-in-40.
#
# What reproduces it is CONCURRENT CUDA WORK, not host CPU load. A CPU spinner
# is a poor stressor: at load average 22 with pure CPU spin the rate was 1/80,
# while three concurrent `vmaf --backend cuda` processes (GPU ~57-63% busy) put
# it at 56-57/60. Generate GPU contention, or this script will report a clean
# run and tell you nothing:
#
#   # in another shell, for the duration of the measurement
#   for i in 1 2 3; do
#     while :; do vmaf --reference REF --distorted DIS --width 576 --height 324 \
#       --pixel_format 420 --bitdepth 8 --backend cuda --output /dev/null --json \
#       >/dev/null 2>&1; done &
#   done
#
# Then:
#
#   scripts/test/repro-cuda-ffmpeg-nondeterminism.sh 60          # measure
#   scripts/test/repro-cuda-ffmpeg-nondeterminism.sh 60 /path/libvmaf.so.dir
#
# The second argument prepends a directory to LD_LIBRARY_PATH so a patched
# libvmaf can be A/B'd against the installed one. To compare two builds, do NOT
# run one after the other: the corruption rate tracks host load, which moves
# under you, and a sequential comparison will invent an effect that is not
# there. Interleave the two arms run-by-run.
#
# Exit: 0 if every run agreed, 1 if any run deviated, 2 on a setup problem.
set -uo pipefail

N="${1:-40}"
EXTRA_LIB="${2:-}"
YUV="${VMAF_YUV_DIR:-/build/vmaf/python/test/resource/yuv}"
REF="$YUV/src01_hrc00_576x324.yuv"
DIS="$YUV/src01_hrc01_576x324.yuv"

for f in "$REF" "$DIS"; do
  if [[ ! -f "$f" ]]; then
    echo "repro: missing fixture $f (set VMAF_YUV_DIR, or run scripts/test/fetch-test-yuvs.sh)" >&2
    exit 2
  fi
done
command -v ffmpeg >/dev/null || {
  echo "repro: ffmpeg not on PATH" >&2
  exit 2
}
# Capture rather than `grep -q`: with `set -o pipefail`, grep -q closes the pipe
# on its first match, ffmpeg takes SIGPIPE, and the pipeline reports failure even
# though the filter IS present.
have_filter="$(ffmpeg -hide_banner -filters 2>/dev/null | grep -c libvmaf_cuda || true)"
if [[ "$have_filter" -eq 0 ]]; then
  echo "repro: this ffmpeg does not list a libvmaf_cuda filter." >&2
  echo "  Inside the dev container this usually means the runtime library path" >&2
  echo "  is unset -- ffmpeg then lists only its builtin filters. Run under a" >&2
  echo "  login shell (docker exec vmaf-dev-mcp bash -lc '...') or source" >&2
  echo "  /opt/intel/oneapi/setvars.sh first, then retry." >&2
  exit 2
fi

[[ -n "$EXTRA_LIB" ]] && export LD_LIBRARY_PATH="$EXTRA_LIB:${LD_LIBRARY_PATH:-}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "repro: $N runs, load average now $(cut -d' ' -f1-3 /proc/loadavg)"
for ((i = 1; i <= N; i++)); do
  ffmpeg -hide_banner -loglevel error \
    -init_hw_device cuda=cu -filter_hw_device cu \
    -s 576x324 -pix_fmt yuv420p -i "$DIS" \
    -s 576x324 -pix_fmt yuv420p -i "$REF" \
    -lavfi "[0:v]hwupload[d];[1:v]hwupload[r];[d][r]libvmaf_cuda=log_fmt=json:log_path=$work/run_$i.json" \
    -f null - >/dev/null 2>&1 || true
done

python3 - "$work" <<'PY'
import collections, glob, json, os, sys

d = sys.argv[1]
runs = {}
for f in sorted(glob.glob(os.path.join(d, "run_*.json"))):
    try:
        runs[f] = json.load(open(f))
    except Exception:
        pass
if not runs:
    print("repro: no run produced output")
    raise SystemExit(2)

pooled = {f: round(r["pooled_metrics"]["vmaf"]["mean"], 6) for f, r in runs.items()}
counts = collections.Counter(pooled.values())
modal, n_modal = counts.most_common(1)[0]
bad = len(runs) - n_modal
print("repro: %d runs, modal %.6f (%d), deviating %d (%.0f%%)"
      % (len(runs), modal, n_modal, bad, 100.0 * bad / len(runs)))
if not bad:
    print("repro: no deviation. On an idle host this is expected -- add load and retry.")
    raise SystemExit(0)

good = next(f for f, v in pooled.items() if v == modal)
ref = {fr["frameNum"]: fr["metrics"] for fr in runs[good]["frames"]}
metrics = collections.Counter()
for f, v in sorted(pooled.items()):
    if v == modal:
        continue
    cur = {fr["frameNum"]: fr["metrics"] for fr in runs[f]["frames"]}
    frames = set()
    for n, m in ref.items():
        for k, val in m.items():
            if k in cur.get(n, {}) and cur[n][k] != val:
                metrics[k] += 1
                frames.add(n)
    print("  %-14s -> %.6f  corrupt frames %s" % (os.path.basename(f), v, sorted(frames)))
print("  metrics affected:", ", ".join(k for k, _ in metrics.most_common()))
raise SystemExit(1)
PY
