#!/bin/sh -x
# Regression: `vmaf --threads N` on a GPU backend must succeed AND agree with
# the serial run.
#
# Before ADR-1197 it did neither. flush_context_threaded() flushed every
# TEMPORAL extractor including the GPU ones, so a temporal GPU extractor had
# its tail-batch drain run BEFORE the pending boundary collect. Two
# consequences, and the test checks for both because fixing only the first
# turns a loud failure into a silently wrong number:
#
#   1. The later collect in flush_context_cuda() was then a duplicate write,
#      rejected with -EINVAL. That value was folded into the same `err` as the
#      cuCtxSynchronize() result, so the run aborted with exit 234 and
#      "context could not be synchronized" -- while all four CUDA calls had
#      returned success. Every N failed, on every input, on CUDA and SYCL.
#   2. The batch-boundary frame was emitted without the min() against the
#      following frame that motion2/motion3 are defined by. On the 48-frame
#      Netflix pair that moved frame 39's integer_motion2 from 3.724278 to
#      4.382255 and the pooled VMAF from 82.814059 to 82.823778.
#
# Exit 77 is meson's SKIP code, used when no GPU is present.
set -e

BACKEND="${1:-cuda}"
# meson is invoked as `meson setup <build> core`, so MESON_SOURCE_ROOT is the
# core/ directory, not the repository root. The Netflix fixtures live at
# <repo>/python/test/resource/yuv, one level above it. Accept either, so the
# script also works when run by hand from a checkout.
if [ -d "${MESON_SOURCE_ROOT}/python/test/resource/yuv" ]; then
  YUV="${MESON_SOURCE_ROOT}/python/test/resource/yuv"
else
  YUV="${MESON_SOURCE_ROOT}/../python/test/resource/yuv"
fi
REF="${YUV}/src01_hrc00_576x324.yuv"
DIS="${YUV}/src01_hrc01_576x324.yuv"

if [ ! -f "$REF" ] || [ ! -f "$DIS" ]; then
  echo "[skip: Netflix golden YUV pair not present; run scripts/test/fetch-test-yuvs.sh]"
  exit 77
fi

# Probe the backend by running it. A GPU-less machine, a driver mismatch or a
# backend that was not compiled in all land here, and all mean "skip", not
# "fail".
if ! ./tools/vmaf --reference "$REF" --distorted "$DIS" \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --backend "$BACKEND" --json --output serial.json >/dev/null 2>&1; then
  echo "[skip: backend $BACKEND not usable on this machine]"
  exit 77
fi

# Any thread count must work. 1 is included deliberately: it used to fail too,
# and testdata/bench_all.sh hard-codes --threads 1, so its GPU rows were masked
# failures for as long as this bug existed.
for n in 1 2 4; do
  ./tools/vmaf --reference "$REF" --distorted "$DIS" \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --backend "$BACKEND" --threads "$n" --json --output "threaded_$n.json"

  python3 - "$n" <<'PY'
import json, sys
n = sys.argv[1]
a = json.load(open("serial.json"))
b = json.load(open("threaded_%s.json" % n))
va = a["pooled_metrics"]["vmaf"]["mean"]
vb = b["pooled_metrics"]["vmaf"]["mean"]
if va != vb:
    print("FAIL: --threads %s pooled VMAF %.9f != serial %.9f" % (n, vb, va))
    sys.exit(1)

# Per-frame too: the pooled mean can coincide while one frame is wrong.
fa = {f["frameNum"]: f["metrics"] for f in a["frames"]}
fb = {f["frameNum"]: f["metrics"] for f in b["frames"]}
if fa.keys() != fb.keys():
    print("FAIL: --threads %s frame set differs from serial" % n)
    sys.exit(1)
for num, ma in fa.items():
    mb = fb[num]
    if ma.keys() != mb.keys():
        print("FAIL: --threads %s frame %d metric set differs" % (n, num))
        sys.exit(1)
    for k, v in ma.items():
        if v != mb[k]:
            print("FAIL: --threads %s frame %d %s = %.9f != serial %.9f"
                  % (n, num, k, mb[k], v))
            sys.exit(1)
print("ok: --threads %s is identical to serial (%d frames)" % (n, len(fa)))
PY
done
