#!/bin/bash
# Arm D campaign — silent-wait ladder, 6 rounds per device, with an external
# nvidia-smi sampler running alongside (out-of-band view of memory during waits).
export PATH="/c/msys64/mingw64/bin:/c/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.4/bin:$PATH"
cd /c/PLAN/VMAF/worktrees/_oom-master || exit 2
BASE=/c/PLAN/VMAF/verify/tmp/oom-r20
mkdir -p "$BASE/armD/dev0" "$BASE/armD/dev1"

# sampler: both GPUs every 5 s for the whole campaign (nvidia-smi -i: 0=4060, 1=5080)
(
  for i in $(seq 1 400); do
    echo "$(date -u +%H:%M:%S) | 4060: $(nvidia-smi -i 0 --query-gpu=memory.used,memory.free --format=csv,noheader) | 5080: $(nvidia-smi -i 1 --query-gpu=memory.used,memory.free --format=csv,noheader)"
    sleep 5
  done
) > "$BASE/armD/sampler.log" 2>&1 &
SAMPLER_PID=$!
trap 'kill $SAMPLER_PID 2>/dev/null' EXIT

: > "$BASE/armD/timeline.log"
for DEV in 0 1; do
  for i in $(seq -w 1 6); do
    {
      echo "=== armD dev$DEV run$i start $(date -u +%H:%M:%S)"
    } >> "$BASE/armD/timeline.log"
    CUDA_VISIBLE_DEVICES=$DEV ./build/test/test_cuda_oom_pictures_released.exe \
      > "$BASE/armD/dev$DEV/run-$i.out" 2> "$BASE/armD/dev$DEV/run-$i.err"
    echo "rc=$?" >> "$BASE/armD/dev$DEV/run-$i.err"
    echo "=== armD dev$DEV run$i end   $(date -u +%H:%M:%S)" >> "$BASE/armD/timeline.log"
  done
done
echo "ARM-D-DONE $(date -u +%H:%M:%S)" >> "$BASE/armD/timeline.log"
