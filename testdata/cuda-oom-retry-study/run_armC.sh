#!/bin/bash
# Arm C campaign — poll-until-back with timing (bounded 60 s), 12 rounds per device.
export PATH="/c/msys64/mingw64/bin:/c/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.4/bin:$PATH"
cd /c/PLAN/VMAF/worktrees/_oom-master || exit 2
BASE=/c/PLAN/VMAF/verify/tmp/oom-r20
mkdir -p "$BASE/armC/dev0" "$BASE/armC/dev1"
: > "$BASE/armC/timeline.log"
for DEV in 0 1; do
  for i in $(seq -w 1 12); do
    {
      echo "=== armC dev$DEV run$i start $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      nvidia-smi -i "$DEV" --query-gpu=memory.used,utilization.gpu --format=csv,noheader
    } >> "$BASE/armC/timeline.log"
    CUDA_VISIBLE_DEVICES=$DEV ./build/test/test_cuda_oom_pictures_released.exe \
      > "$BASE/armC/dev$DEV/run-$i.out" 2> "$BASE/armC/dev$DEV/run-$i.err"
    echo "rc=$?" >> "$BASE/armC/dev$DEV/run-$i.err"
    {
      echo "=== armC dev$DEV run$i end   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      nvidia-smi -i "$DEV" --query-gpu=memory.used,utilization.gpu --format=csv,noheader
    } >> "$BASE/armC/timeline.log"
  done
done
echo "ARM-C-DONE $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$BASE/armC/timeline.log"
