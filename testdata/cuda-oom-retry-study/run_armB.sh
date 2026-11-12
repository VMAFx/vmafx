#!/bin/bash
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2

# Arm B campaign — probe-removed variant, 12 rounds per device, out-of-band memory logs.
# The saved no-probe binary makes the arm identity explicit (see SUMMARY.md):
# the shared test_cuda_oom_pictures_released.exe would silently measure arm A
# unless armB_edit.py was applied and the test rebuilt first.
#
# Usage: run_armB.sh <build-worktree> <results-root>
# The caller puts nvidia-smi and the CUDA runtime DLL directories on PATH;
# the script hard-codes no machine-specific location. Results land in
# <results-root>/armB/ with the same layout as the committed logs.

WORKTREE="${1:?usage: run_armB.sh <build-worktree> <results-root>}"
ROOT="${2:?usage: run_armB.sh <build-worktree> <results-root>}"
EXE="$WORKTREE/build/test/noprobe.exe"
[ -x "$EXE" ] || {
  echo "arm B binary missing: $EXE (see SUMMARY.md)" >&2
  exit 2
}
BASE="$ROOT/armB"
mkdir -p "$BASE/dev0" "$BASE/dev1"
: >"$BASE/timeline.log"
for DEV in 0 1; do
  for i in $(seq -w 1 12); do
    {
      echo "=== armB dev$DEV run$i start $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      nvidia-smi -i "$DEV" --query-gpu=memory.used,utilization.gpu --format=csv,noheader
    } >>"$BASE/timeline.log"
    CUDA_VISIBLE_DEVICES=$DEV "$EXE" \
      >"$BASE/dev$DEV/run-$i.out" 2>"$BASE/dev$DEV/run-$i.err"
    echo "rc=$?" >>"$BASE/dev$DEV/run-$i.err"
    {
      echo "=== armB dev$DEV run$i end   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      nvidia-smi -i "$DEV" --query-gpu=memory.used,utilization.gpu --format=csv,noheader
    } >>"$BASE/timeline.log"
  done
done
echo "ARM-B-DONE $(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$BASE/timeline.log"
