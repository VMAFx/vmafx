#!/bin/bash
# Arm B campaign — probe-removed variant, 12 rounds per device, out-of-band memory logs.
# The saved no-probe binary (SUMMARY.md line 47) makes the arm identity explicit:
# the shared test_cuda_oom_pictures_released.exe would silently measure arm A
# unless armB_edit.py was applied and the test rebuilt first.
export PATH="/c/msys64/mingw64/bin:/c/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.4/bin:$PATH"
cd /c/PLAN/VMAF/worktrees/_oom-master || exit 2
[ -x ./build/test/noprobe.exe ] || {
  echo "arm B binary missing: ./build/test/noprobe.exe (see SUMMARY.md)" >&2
  exit 2
}
BASE=/c/PLAN/VMAF/verify/tmp/oom-r20
mkdir -p "$BASE/armB/dev0" "$BASE/armB/dev1"
: > "$BASE/armB/timeline.log"
for DEV in 0 1; do
  for i in $(seq -w 1 12); do
    {
      echo "=== armB dev$DEV run$i start $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      nvidia-smi -i "$DEV" --query-gpu=memory.used,utilization.gpu --format=csv,noheader
    } >> "$BASE/armB/timeline.log"
    CUDA_VISIBLE_DEVICES=$DEV ./build/test/noprobe.exe \
      > "$BASE/armB/dev$DEV/run-$i.out" 2> "$BASE/armB/dev$DEV/run-$i.err"
    echo "rc=$?" >> "$BASE/armB/dev$DEV/run-$i.err"
    {
      echo "=== armB dev$DEV run$i end   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      nvidia-smi -i "$DEV" --query-gpu=memory.used,utilization.gpu --format=csv,noheader
    } >> "$BASE/armB/timeline.log"
  done
done
echo "ARM-B-DONE $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$BASE/armB/timeline.log"
