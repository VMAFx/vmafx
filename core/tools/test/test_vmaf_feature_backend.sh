#!/bin/sh -x
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# ADR-1359: `--feature <cpu-name>` with an explicit device `--backend` runs the
# backend's twin, falls back to the CPU extractor with one warning when there is
# none or it cannot honour the request, and the JSON receipt says what ran.
#
#   test_vmaf_feature_backend.sh cpu        # every build: exact-name routing
#   test_vmaf_feature_backend.sh <backend>  # cuda|sycl|hip|metal: twin routing
#
# The device run exits 77 (meson SKIP) when the backend is not usable here.
set -e

BACKEND="${1:-cpu}"
# MESON_SOURCE_ROOT is core/; the Netflix fixtures sit one level above it.
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

# run <tag> <expected exit> <vmaf args...>: JSON to <tag>.json, stderr to <tag>.err
run() {
  tag="$1"
  want="$2"
  shift 2
  set +e
  ./tools/vmaf --reference "$REF" --distorted "$DIS" \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 --frame_cnt 3 \
    --json --output "$tag.json" "$@" 2>"$tag.err"
  got=$?
  set -e
  if [ "$got" -ne "$want" ]; then
    cat "$tag.err"
    echo "FAIL: $tag exited $got, expected $want"
    exit 1
  fi
}

# receipt <tag> <backend_used> <extractor=backend>...: exact receipt check
receipt() {
  python3 - "$@" <<'PY'
import json, sys
tag, used, *want = sys.argv[1:]
doc = json.load(open(tag + ".json"))
got = ["%s=%s" % (e["extractor"], e["backend"]) for e in doc["feature_backends"]]
if doc["backend_used"] != used or got != want:
    print("FAIL: %s receipt backend_used=%s %s, expected %s %s"
          % (tag, doc["backend_used"], got, used, want))
    sys.exit(1)
PY
}

no_warning() {
  if grep -q "warning:" "$1.err"; then
    cat "$1.err"
    echo "FAIL: $1 printed a warning"
    exit 1
  fi
}

has_warning() {
  if [ "$(grep -c "warning: --feature $2" "$1.err")" -ne 1 ] || ! grep -q "$3" "$1.err"; then
    cat "$1.err"
    echo "FAIL: $1 lacks one warning for $2 saying '$3'"
    exit 1
  fi
}

if [ "$BACKEND" = cpu ]; then
  # --backend cpu, --backend auto and no --backend keep the exact name.
  run cpu 0 --no_prediction --feature ciede --backend cpu
  no_warning cpu
  receipt cpu cpu ciede=cpu
  run auto 0 --no_prediction --feature ciede --backend auto
  no_warning auto
  receipt auto cpu ciede=cpu
  run unset 0 --no_prediction --feature ciede
  no_warning unset
  receipt unset cpu ciede=cpu
  # A twin name still needs its backend (ADR-0543): exit 100, as before.
  run pinned 100 --no_prediction --feature ciede_sycl --backend cpu
  echo "ok: cpu, auto and unset --backend keep exact-name --feature routing"
  exit 0
fi

# Probe the device; a machine without it skips instead of failing.
set +e
./tools/vmaf --reference "$REF" --distorted "$DIS" \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 --frame_cnt 1 \
  --no_prediction --feature ciede --backend "$BACKEND" >/dev/null 2>&1
probe=$?
set -e
if [ "$probe" -ne 0 ]; then
  echo "[skip: backend $BACKEND not usable on this machine]"
  exit 77
fi

# 1. A CPU name runs the twin, silently, and the receipt names the twin.
run mapped 0 --no_prediction --feature ciede --backend "$BACKEND"
no_warning mapped
TWIN=$(python3 -c 'import json; print(json.load(open("mapped.json"))["feature_backends"][0]["extractor"])')
[ "$TWIN" != ciede ] || {
  echo "FAIL: ciede was not mapped to a $BACKEND twin"
  exit 1
}
receipt mapped "$BACKEND" "$TWIN=$BACKEND"

# 2. Naming the twin directly is unchanged, and computes the same scores.
run explicit 0 --no_prediction --feature "$TWIN" --backend "$BACKEND"
no_warning explicit
receipt explicit "$BACKEND" "$TWIN=$BACKEND"
python3 - <<'PY'
import json, sys
a = json.load(open("mapped.json"))["frames"]
b = json.load(open("explicit.json"))["frames"]
if [f["metrics"] for f in a] != [f["metrics"] for f in b]:
    print("FAIL: mapped and explicit twin scores differ")
    sys.exit(1)
PY

# 3. No twin: CPU extractor, one warning, and a CPU receipt.
run no_twin 0 --no_prediction --feature brisque --backend "$BACKEND"
has_warning no_twin brisque "has no twin"
receipt no_twin cpu brisque=cpu

# 4. An option the twin does not implement keeps the CPU extractor. No
#    float_motion twin implements motion_filter_size (the SYCL and Metal
#    float_ssim twins now mirror enable_lcs, ADR-1365), so every backend runs it.
run option 0 --no_prediction --feature float_motion=motion_filter_size=3 --backend "$BACKEND"
has_warning option float_motion "cannot honour option 'motion_filter_size'"
receipt option cpu float_motion=cpu

# 5. The twin implements scale=1 only (ADR-1324): scale=2 keeps the CPU.
run geometry 0 --no_prediction --feature float_ssim=scale=2 --backend "$BACKEND"
has_warning geometry float_ssim "cannot run 576x324 8-bit pictures"
receipt geometry cpu float_ssim=cpu

# 6. A mixed run reports the device and lists the CPU extractor next to it.
run mixed 0 --no_prediction --feature ciede --feature brisque --backend "$BACKEND"
has_warning mixed brisque "has no twin"
receipt mixed "$BACKEND" "$TWIN=$BACKEND" brisque=cpu

echo "ok: --feature routes to the $BACKEND twin ($TWIN) and falls back with a warning"
