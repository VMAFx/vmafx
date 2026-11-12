#!/bin/bash
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2

# Arm C campaign — poll-until-back with timing (bounded 60 s), 12 rounds per device.
# This script runs the shared test binary: apply armC_edit.patch to the build
# worktree and rebuild first, or it measures the as-landed arm instead (see
# SUMMARY.md). Revert the source afterwards.
#
# Usage: run_armC.sh <build-worktree> <results-root>
# The caller puts nvidia-smi and the CUDA runtime DLL directories on PATH;
# the script hard-codes no machine-specific location. Results land in
# <results-root>/armC/ with the same layout as the committed logs.

WORKTREE="${1:?usage: run_armC.sh <build-worktree> <results-root>}"
ROOT="${2:?usage: run_armC.sh <build-worktree> <results-root>}"
EXE="$WORKTREE/build/test/test_cuda_oom_pictures_released.exe"
[ -x "$EXE" ] || {
  echo "test binary missing: $EXE" >&2
  exit 2
}
BASE="$ROOT/armC"
mkdir -p "$BASE/dev0" "$BASE/dev1"
: >"$BASE/timeline.log"
for DEV in 0 1; do
  for i in $(seq -w 1 12); do
    {
      echo "=== armC dev$DEV run$i start $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      nvidia-smi -i "$DEV" --query-gpu=memory.used,utilization.gpu --format=csv,noheader
    } >>"$BASE/timeline.log"
    CUDA_VISIBLE_DEVICES=$DEV "$EXE" \
      >"$BASE/dev$DEV/run-$i.out" 2>"$BASE/dev$DEV/run-$i.err"
    echo "rc=$?" >>"$BASE/dev$DEV/run-$i.err"
    {
      echo "=== armC dev$DEV run$i end   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      nvidia-smi -i "$DEV" --query-gpu=memory.used,utilization.gpu --format=csv,noheader
    } >>"$BASE/timeline.log"
  done
done
echo "ARM-C-DONE $(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$BASE/timeline.log"
