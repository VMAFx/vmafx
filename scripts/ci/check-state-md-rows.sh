#!/usr/bin/env bash
# check-state-md-rows.sh — docs/state.md row hygiene (ADR-0165).
#
# Every bug id may appear as a table row exactly once. Two ways this breaks:
#
#   1. A PR moves a row from "## Open bugs" to "## Recently closed" and a later
#      rebase resolves the conflict by keeping BOTH sides — the stale
#      present-tense row stays under Open while the past-tense one lands under
#      Recently closed, so the bug reads as open forever. Five rows were in that
#      state on 2026-09-05 (T-CUDA-INIT-SUBMIT-LEAKS, T-UPSTREAM-1564,
#      T-SPEED-GPU-REGISTRY-ORPHAN, T-SIMD-BIT-EXACT-ROUND2,
#      T-SIMD-ICX-FP-CONTRACT).
#   2. The same keep-both resolution duplicates a row inside one section.
#
# Both are invisible to a reviewer reading a diff hunk, so they are gated here.
#
# The id is matched as the bold token that OPENS the first cell, not as the whole
# cell. Rows come in two shapes, `| **T-ID** | ...` and the far more common
# `| **T-ID** -- one-line description | ...`; anchoring on `\*\* \|` silently
# skips every row of the second shape. It did: 30 of 335 rows were invisible to
# the first version of this gate, and two duplicate pairs
# (T-SYCL-DMABUF-IMPORT-WIN32-ENOSYS, T-VK-VIF-1.4-RESIDUAL) survived its first
# sweep. The id character class includes `.` for the same reason
# (T-VK-VIF-1.4-RESIDUAL).
#
# Usage: check-state-md-rows.sh [PATH]   (default: docs/state.md)
# Exit:  0 clean; 1 a duplicate id; 2 the file is missing.
set -euo pipefail

file="${1:-docs/state.md}"
if [[ ! -f "$file" ]]; then
  echo "check-state-md-rows: $file not found" >&2
  exit 2
fi

# Row ids: a table line whose first cell OPENS with a bold T-… identifier.
dupes="$(grep -oE '^\| \*\*T-[A-Z0-9._-]+\*\*' "$file" |
  sed -E 's/^\| \*\*//; s/\*\*$//' |
  sort | uniq -d || true)"

if [[ -n "$dupes" ]]; then
  echo "::error title=state.md duplicate rows::each bug id must appear exactly once" >&2
  while IFS= read -r id; do
    [[ -z "$id" ]] && continue
    echo "  $id appears on lines:" >&2
    grep -nF "| **${id}**" "$file" | cut -d: -f1 | sed 's/^/    /' >&2
  done <<<"$dupes"
  echo "" >&2
  echo "A keep-both rebase resolution usually caused this: keep the row that" >&2
  echo "matches the bug's real state and delete the other (ADR-0165)." >&2
  exit 1
fi

count="$(grep -cE '^\| \*\*T-[A-Z0-9._-]+\*\*' "$file" || true)"
echo "check-state-md-rows: OK ($count rows, no duplicate ids)"
