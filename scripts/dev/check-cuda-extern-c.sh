#!/usr/bin/env bash
# check-cuda-extern-c.sh — CI gate: every __global__ kernel referenced
# by cuModuleGetFunction must be inside an extern "C" block.
#
# Exit 0 = all clear.  Exit 1 = at least one broken kernel found.
#
# Usage:
#   bash scripts/dev/check-cuda-extern-c.sh [repo-root]
#
# If repo-root is omitted, the script walks from the current directory.
#
# Algorithm:
#   1. Find all cuModuleGetFunction calls and extract the string-literal
#      kernel names.
#   2. For each name, find the __global__ definition in .cu files.
#   3. Check whether that definition falls inside an extern "C" block
#      by walking backwards from the definition line looking for a
#      not-yet-closed extern "C" {.
#
# Known limitation: the heuristic for "inside extern C" is a simple
# brace-balance scan backwards from the __global__ line.  It handles
# all patterns present in this repo (flat block, macro-expanded
# instantiation block) but would miss a kernel defined inside a
# class scope that is itself inside extern "C".  No such pattern
# currently exists in this tree.

set -euo pipefail

ROOT="${1:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
CUDA_DIRS=(
  "$ROOT/core/src/feature/cuda"
  "$ROOT/core/src/cuda"
)

# ── Step 1: collect kernel names from cuModuleGetFunction calls ──────────────
declare -A KERNEL_NAMES
while IFS= read -r line; do
  # Extract string literal: cuModuleGetFunction(..., "some_name")
  if [[ "$line" =~ cuModuleGetFunction[^,]+,[[:space:]]*\"([^\"]+)\" ]]; then
    KERNEL_NAMES["${BASH_REMATCH[1]}"]=1
  fi
done < <(grep -rn 'cuModuleGetFunction' "${CUDA_DIRS[@]}" 2>/dev/null)

if [[ ${#KERNEL_NAMES[@]} -eq 0 ]]; then
  echo "check-cuda-extern-c: no cuModuleGetFunction calls found — nothing to check." >&2
  exit 0
fi

# ── Step 2 + 3: for each kernel name, verify extern "C" wrapping ─────────────
FAIL=0

for kname in "${!KERNEL_NAMES[@]}"; do
  # Find the __global__ definition line(s)
  while IFS=: read -r filepath lineno _rest; do
    # Walk backwards through the file counting brace depth to detect
    # whether we are inside an extern "C" { } block.
    in_extern_c=0
    depth=0
    while IFS= read -r srcline; do
      # Count closing braces (going backwards → closing braces OPEN scope)
      opens=$(echo "$srcline" | grep -o '}' | wc -l)
      closes=$(echo "$srcline" | grep -o '{' | wc -l)
      depth=$((depth + opens - closes))
      # If depth goes negative at an extern "C" line, we've exited a block.
      if echo "$srcline" | grep -qE '^[[:space:]]*extern[[:space:]]+"C"[[:space:]]*\{'; then
        if ((depth <= 0)); then
          in_extern_c=1
          break
        fi
      fi
    done < <(head -n "$lineno" "$filepath" | tac)

    if [[ $in_extern_c -eq 0 ]]; then
      echo "ERROR: __global__ kernel '$kname' in $filepath:$lineno is NOT inside extern \"C\" { }" >&2
      FAIL=1
    fi
  done < <(grep -rn "__global__.*void[[:space:]]*${kname}[^a-zA-Z0-9_]" \
    "${CUDA_DIRS[@]}" 2>/dev/null | grep '\.cu:' || true)
done

if [[ $FAIL -eq 0 ]]; then
  echo "check-cuda-extern-c: all ${#KERNEL_NAMES[@]} looked-up kernels are wrapped in extern \"C\". OK."
fi

exit $FAIL
