#!/usr/bin/env bash
# check-smoke-probe-flags.sh — cross-reference the vmaf CLI flags used by
# dev/scripts/smoke-probe-loop.sh against the long_opts[] table registered
# in core/tools/cli_parse.cpp.
#
# The probe loop shells out to the real vmaf binary; a flag typo (e.g. a
# bare --cuda instead of --backend=cuda, or --no_prediction_flags instead
# of --no_prediction) fails silently at container-runtime with no CI
# signal, because the script is never exercised by the Python test suite.
# This check catches that class of drift without needing a GPU, the vmaf
# binary, or the golden test YUVs.
#
# Usage: scripts/ci/check-smoke-probe-flags.sh [repo-root]
#
# Exit 0 if every --flag literal used by smoke-probe-loop.sh's
# probe_backend() function is a registered long option. Exit 1 otherwise.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

set -euo pipefail

ROOT="${1:-$(git rev-parse --show-toplevel 2>/dev/null || pwd)}"
CLI_PARSE="$ROOT/core/tools/cli_parse.cpp"
PROBE_SCRIPT="$ROOT/dev/scripts/smoke-probe-loop.sh"

if [[ ! -f "$CLI_PARSE" ]]; then
  echo "ERROR: cli_parse.cpp not found at $CLI_PARSE" >&2
  exit 1
fi
if [[ ! -f "$PROBE_SCRIPT" ]]; then
  echo "ERROR: smoke-probe-loop.sh not found at $PROBE_SCRIPT" >&2
  exit 1
fi

# Extract the long_opts[] table and pull out every registered --name.
opts_block=$(awk '/^const struct option long_opts\[\] = \{/,/^\};/' "$CLI_PARSE")
if [[ -z "$opts_block" ]]; then
  echo "ERROR: long_opts[] not found in $CLI_PARSE" >&2
  exit 1
fi

mapfile -t valid_flags < <(grep -oP '\.name\s*=\s*"\K[^"]+' <<<"$opts_block" | LC_ALL=C sort -u)
if [[ "${#valid_flags[@]}" -eq 0 ]]; then
  echo "ERROR: no .name = \"...\" entries parsed from long_opts[]" >&2
  exit 1
fi

is_valid_flag() {
  local needle="$1"
  local f
  for f in "${valid_flags[@]}"; do
    [[ "$f" == "$needle" ]] && return 0
  done
  return 1
}

rc=0

# The probe_backend() function body is where every literal vmaf CLI flag
# (plus the backend_flag case-statement assignments consumed via
# ${backend_flag}) lives. Extracting by function name avoids brittle
# pattern-matching on lines that themselves contain shell variables.
fn_body=$(awk '/^probe_backend\(\) \{/{p=1} p{print} p && /^\}/{exit}' "$PROBE_SCRIPT")
if [[ -z "$fn_body" ]]; then
  echo "ERROR: probe_backend() function not found in $PROBE_SCRIPT" >&2
  exit 1
fi
# Drop full-line comments (indented `#`) so prose that happens to mention
# a --flag-shaped token (e.g. "there is no bare --cuda") is not mistaken
# for a literal CLI argument.
fn_body=$(grep -v '^\s*#' <<<"$fn_body")

# 1. Literal --flag tokens used directly as vmaf CLI arguments (excludes
#    ${...} shell-variable references, which are not literal flag text).
while IFS= read -r flag; do
  base="${flag#--}"
  base="${base%%=*}"
  if ! is_valid_flag "$base"; then
    echo "  MISSING: --$base is not registered in cli_parse.cpp long_opts[]"
    rc=1
  else
    echo "  OK: --$base"
  fi
done < <(grep -oP '(?<!\$\{)--[a-zA-Z][a-zA-Z0-9_-]*' <<<"$fn_body" | LC_ALL=C sort -u)

# 2. Flag literals assigned to backend_flag in the case statement — these
#    reach vmaf via the ${backend_flag} expansion step 1 above skips.
while IFS= read -r flag; do
  base="${flag#--}"
  base="${base%%=*}"
  if ! is_valid_flag "$base"; then
    echo "  MISSING (backend_flag case): --$base is not registered in cli_parse.cpp long_opts[]"
    rc=1
  else
    echo "  OK (backend_flag case): --$base"
  fi
done < <(grep -oP 'backend_flag="\K--[a-zA-Z][a-zA-Z0-9_=-]*' <<<"$fn_body" |
  sed 's/=.*//' | LC_ALL=C sort -u)

echo ""
if [[ "$rc" -eq 0 ]]; then
  echo "PASS: every smoke-probe-loop.sh vmaf flag is registered in cli_parse.cpp"
else
  echo "FAIL: smoke-probe-loop.sh uses a vmaf CLI flag cli_parse.cpp does not register"
fi
exit "$rc"
