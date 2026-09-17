#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

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
# The id is matched as the token that OPENS the first cell, not as the whole
# cell. Rows come in two shapes, `| **T-ID** | ...` and the far more common
# `| **T-ID** -- one-line description | ...`; anchoring on `\*\* \|` silently
# skips every row of the second shape. It did: 30 of 335 rows were invisible to
# the first version of this gate, and two duplicate pairs
# (T-SYCL-DMABUF-IMPORT-WIN32-ENOSYS, T-VK-VIF-1.4-RESIDUAL) survived its first
# sweep. The id character class includes `.` for the same reason
# (T-VK-VIF-1.4-RESIDUAL).
#
# The id is ALSO not always bold, and not always a `T-` token. Requiring `**`
# hid 95 of 576 id-bearing rows, among them a byte-identical duplicate of
# T-CUDA-MUL24-AUDIT-2026-05-28; requiring `T-` hid the 34 `Netflix#NNN` rows,
# which had accumulated 13 duplicate pairs, and the `**T6-1**` / `**T7-16**`
# tranche ids, which had 4 more. All four shapes are matched below. Every
# widening of this gate so far has immediately found real duplicates that the
# narrower version reported as clean — which is the argument for matching
# shapes generously rather than exactly.
#
# ~143 rows open with prose and carry no id at all, so they cannot be
# deduplicated by id. Those are covered by the second check: an exactly
# repeated row line. The two are complementary — the id check catches the same
# bug re-filed with edited text, the row check catches a verbatim copy-paste of
# anything at all, id or no id.
#
# Detection and reporting share one extraction rule (the awk program below)
# rather than a grep pattern plus a separate grep to locate the hits. The
# earlier split meant the reporting pattern could disagree with the detecting
# one, and it did: it assumed `**`, so every non-bold duplicate printed "appears
# on lines:" followed by nothing.
#
# Usage: check-state-md-rows.sh [PATH]   (default: docs/state.md)
# Exit:  0 clean; 1 a duplicate id or a repeated row; 2 the file is missing.
set -euo pipefail

file="${1:-docs/state.md}"
if [[ ! -f "$file" ]]; then
  echo "check-state-md-rows: $file not found" >&2
  exit 2
fi

report="$(awk '
  # A |---|---| separator means the PREVIOUS line was a column header, not a
  # data row. Headers repeat once per section by design (`| ID | Description |`
  # appears above every table), so counting them as duplicate rows is a false
  # positive -- and was: it failed this gate\047s own "unique ids pass" fixture.
  /^\|[[:space:]]*:?-+:?[[:space:]]*\|/ {
    if (prev != "") { rowcount[prev]--; rowlines[prev] = "" }
    prev = ""
    next
  }

  # A table row, but not a |---|---| separator.
  /^\|/ {
    row = $0
    sub(/[[:space:]]+$/, "", row)
    # Normalise away the `_(verified YYYY-MM-DD: ...)_` annotations a later
    # verification sweep appends. Without this the row check compares a row
    # against its own annotated copy, finds them unequal and reports clean:
    # 13 prose-led rows were duplicated in exactly that shape and were
    # invisible to both checks, because a prose-led row has no id either.
    gsub(/_\(verified [0-9]{4}-[0-9]{2}-[0-9]{2}:[^)]*\)_/, "", row)
    gsub(/[[:space:]]+/, " ", row)
    sub(/[[:space:]]+$/, "", row)
    rowcount[row]++
    rowlines[row] = rowlines[row] " " NR
    prev = row

    # The id opens the first cell, optionally bold. Shapes in use:
    #   **T-ID**  T-ID  **T7-16**  Netflix#NNN  **Netflix/vmaf#NNN**
    line = $0
    if (match(line, /^\| \*{0,2}(T-[A-Z0-9._-]+|T[0-9]+-[0-9]+|Netflix(\/vmaf)?#[0-9]+)/)) {
      id = substr(line, RSTART, RLENGTH)
      sub(/^\| \*{0,2}/, "", id)
      idcount[id]++
      idlines[id] = idlines[id] " " NR
      ids++
    }
    next
  }
  !/^\|/ { prev = "" }

  END {
    bad = 0
    for (id in idcount)
      if (idcount[id] > 1) { printf "ID\t%s\t%s\n", id, idlines[id]; bad = 1 }
    for (row in rowcount)
      if (rowcount[row] > 1) { printf "ROW\t%s\t%s\n", substr(row, 1, 100), rowlines[row]; bad = 1 }
    printf "COUNT\t%d\n", ids
  }
' "$file")"

dupes="$(printf '%s\n' "$report" | grep -E '^(ID|ROW)' || true)"

if [[ -n "$dupes" ]]; then
  echo "::error title=state.md duplicate rows::each bug id and each row must appear exactly once" >&2
  while IFS=$'\t' read -r kind what where; do
    [[ -z "$kind" ]] && continue
    if [[ "$kind" == "ID" ]]; then
      echo "  id '$what' appears on lines:$where" >&2
    else
      echo "  row repeated verbatim on lines:$where" >&2
      echo "    ${what}..." >&2
    fi
  done <<<"$dupes"
  echo "" >&2
  echo "A keep-both rebase resolution usually caused this: keep the row that" >&2
  echo "matches the bug's real state and delete the other (ADR-0165). The two" >&2
  echo "copies are often NOT interchangeable and the newer one is not always" >&2
  echo "the later line -- read both before deleting either." >&2
  echo "" >&2
  echo "Mid-rebase, this resolves the common case for you:" >&2
  echo "  python3 scripts/dev/resolve-state-md-conflict.py docs/state.md" >&2
  echo "then re-run this check before 'git rebase --continue'." >&2
  exit 1
fi

count="$(printf '%s\n' "$report" | awk -F'\t' '$1=="COUNT"{print $2}')"
echo "check-state-md-rows: OK ($count id-bearing rows, no duplicate ids or rows)"
