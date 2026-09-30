#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# check-no-non-header-includes.sh — guard against non-header source inclusions (.c/.cpp)
# in unit tests under core/test/.
#
# Prevents CodeQL alert cpp/include-non-header regressions.
# Tests must use internal header declarations (e.g., core/src/libvmaf_priv.h)
# and link seams instead of directly including implementation sources.
#
# Pre-commit usage: scripts/ci/check-no-non-header-includes.sh [files...]
# CI/standalone:   scripts/ci/check-no-non-header-includes.sh [--root <path>]
#
# Exit 0 = clean (no unallowlisted non-header includes found).
# Exit 1 = non-header include found outside the grandfathered allowlist.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"

# Grandfathered test files with historical non-header includes.
# Do not add new files here; eliminate remaining inclusions over time.
declare -A ALLOWLIST=(
  ["core/test/test_ciede.c"]=1
  ["core/test/test_delta_e_itp.c"]=1
  ["core/test/test_feature_collector.c"]=1
  ["core/test/test_mcp_compute_vmaf_allowlist.c"]=1
  ["core/test/test_psnr.c"]=1
  ["core/test/test_pu21.c"]=1
  ["core/test/test_thread_pool_backpressure.c"]=1
)

files_to_check=()

if [[ $# -gt 0 ]]; then
  if [[ "$1" == "--root" && $# -ge 2 ]]; then
    REPO_ROOT="$2"
    shift 2
  fi
fi

if [[ $# -gt 0 ]]; then
  for arg in "$@"; do
    # Normalize relative path to repo root if possible
    rel_path="$arg"
    if [[ "$rel_path" = /* ]]; then
      rel_path="${rel_path#"$REPO_ROOT"/}"
    fi
    if [[ "$rel_path" =~ ^core/test/.*\.(c|cpp)$ ]]; then
      if [[ -f "$REPO_ROOT/$rel_path" ]]; then
        files_to_check+=("$rel_path")
      fi
    fi
  done
else
  # Discover all C/C++ files under core/test/
  while IFS= read -r -d '' f; do
    rel_path="${f#"$REPO_ROOT"/}"
    files_to_check+=("$rel_path")
  done < <(find "$REPO_ROOT/core/test" -type f \( -name "*.c" -o -name "*.cpp" \) -print0)
fi

if [[ ${#files_to_check[@]} -eq 0 ]]; then
  exit 0
fi

errors=0

for rel_path in "${files_to_check[@]}"; do
  if [[ -n "${ALLOWLIST[$rel_path]:-}" ]]; then
    continue
  fi

  full_path="$REPO_ROOT/$rel_path"
  if [[ ! -f "$full_path" ]]; then
    continue
  fi

  # Match lines containing: #include ... .c" or #include ... .cpp"
  # Anchored to start of line allowing leading whitespace.
  while IFS=: read -r lineno match_line; do
    echo "ERROR: $rel_path:$lineno: non-header source inclusion: $match_line" >&2
    errors=$((errors + 1))
  done < <(grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]+["<][^">]+\.(c|cpp)[">]' "$full_path" 2>/dev/null || true)
done

if [[ $errors -gt 0 ]]; then
  echo "" >&2
  echo "Found $errors unallowlisted non-header source inclusion(s) in core/test/." >&2
  echo "Tests must link against libvmaf or declare internal accessors in headers" >&2
  echo "(e.g., core/src/libvmaf_priv.h) rather than unity-including .c/.cpp sources." >&2
  echo "See docs/research/2093-codeql-include-non-header-alerts.md." >&2
  exit 1
fi

echo "PASS: no unallowlisted non-header includes in core/test/ (${#files_to_check[@]} files checked)"
exit 0
