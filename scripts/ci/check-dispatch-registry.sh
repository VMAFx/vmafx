#!/usr/bin/env bash
# check-dispatch-registry.sh — cross-reference vmaf_fex_*_<backend> symbols
# defined in core/src/feature/<backend>/ against feature_extractor_list[]
# in feature_extractor.c.
#
# Usage: scripts/ci/check-dispatch-registry.sh [repo-root]
#
# Exit 0 if every defined symbol appears in the list at least once.
# Exit 1 if any symbol is missing entirely.
# Duplicate entries are reported as warnings (non-fatal: first-match
# semantics make duplicates functionally harmless, but they are noise).
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

ROOT="${1:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
FEX="$ROOT/core/src/feature/feature_extractor.cpp"

if [[ ! -f "$FEX" ]]; then
  echo "ERROR: feature_extractor.cpp not found at $FEX" >&2
  exit 1
fi

list_block=$(sed -n '/static VmafFeatureExtractor \*feature_extractor_list\[\]/,/};/p' "$FEX")
if [[ -z "$list_block" ]]; then
  echo "ERROR: feature_extractor_list[] not found in $FEX" >&2
  exit 1
fi

rc=0

check_backend() {
  local backend="$1"
  local src_dir="$ROOT/core/src/feature/$backend"
  local found_any=0

  if [[ ! -d "$src_dir" ]]; then
    echo "SKIP: $backend — directory $src_dir not found"
    return
  fi

  echo "=== $backend ==="

  while IFS= read -r sym; do
    found_any=1
    local count
    count=$(grep -cF "&${sym}" <<<"$list_block" || true)
    if [[ "$count" -eq 0 ]]; then
      echo "  MISSING: $sym not in feature_extractor_list[]"
      rc=1
    elif [[ "$count" -gt 1 ]]; then
      echo "  WARNING: $sym appears $count times (duplicate entries)"
    else
      echo "  OK: $sym"
    fi
  done < <(grep -rh 'VmafFeatureExtractor vmaf_fex_' "$src_dir"/ 2>/dev/null |
    grep -oP 'vmaf_fex_\w+' | LC_ALL=C sort -u)

  if [[ "$found_any" -eq 0 ]]; then
    echo "  (no vmaf_fex_* symbols found — backend may not be built)"
  fi
}

for backend in cuda sycl hip metal; do
  check_backend "$backend"
done

echo ""
if [[ "$rc" -eq 0 ]]; then
  echo "PASS: all backend symbols present in feature_extractor_list[]"
else
  echo "FAIL: one or more backend symbols missing from feature_extractor_list[]"
fi
exit "$rc"
