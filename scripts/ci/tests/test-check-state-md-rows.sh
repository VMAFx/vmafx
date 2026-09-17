#!/usr/bin/env bash
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

# Tests for scripts/ci/check-state-md-rows.sh (ADR-0165 row hygiene).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
script="$here/../check-state-md-rows.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

expect() { # $1 label, $2 expected rc, $3 file
  local rc=0
  bash "$script" "$3" >/dev/null 2>&1 || rc=$?
  if [ "$rc" -ne "$2" ]; then
    echo "FAIL $1: expected rc=$2 got rc=$rc" >&2
    exit 1
  fi
  echo "ok   $1 (rc=$rc)"
}

cat >"$tmp/clean.md" <<'MD'
## Open bugs

| ID | Description |
| --- | --- |
| **T-ALPHA-2026-01-01** | still broken |

## Recently closed

| ID | Description |
| --- | --- |
| **T-BETA-2026-01-02** | fixed |
MD
expect "a file with unique ids passes" 0 "$tmp/clean.md"

# the keep-both artefact: the same id under Open and under Recently closed
sed 's/T-BETA-2026-01-02/T-ALPHA-2026-01-01/' "$tmp/clean.md" >"$tmp/cross.md"
expect "an id in two sections fails" 1 "$tmp/cross.md"

# the same id twice inside one section
{
  cat "$tmp/clean.md"
  printf '| **T-BETA-2026-01-02** | fixed again |\n'
} >"$tmp/same.md"
expect "an id twice in one section fails" 1 "$tmp/same.md"

# Regression: the id opens the first cell but a description follows it, so the
# cell does not end right after the bold token. This is the majority row shape in
# docs/state.md (30 of 335 rows were invisible to the first version of the gate,
# which anchored on `\*\* \|`), and it hid two real duplicate pairs.
cat >"$tmp/described.md" <<'MD'
## Open bugs

| ID | Description |
| --- | --- |
| **T-GAMMA-2026-01-03** — `core/src/x.c:12` returns `-ENOSYS` | still broken |

## Deferred (waiting on external trigger)

| ID | Description |
| --- | --- |
| **T-GAMMA-2026-01-03** — the same bug, filed a second time | no action required |
MD
expect "an id with a trailing description in the first cell fails" 1 "$tmp/described.md"

# Regression: ids are not restricted to [A-Z0-9-]; T-VK-VIF-1.4-RESIDUAL carries a
# dot and slipped through the first version of the character class.
cat >"$tmp/dotted.md" <<'MD'
## Recently closed

| ID | Description |
| --- | --- |
| **T-DELTA-VIF-1.4-RESIDUAL** — dotted id | fixed |
| **T-DELTA-VIF-1.4-RESIDUAL** — dotted id, kept twice by a rebase | fixed |
MD
expect "an id containing a dot fails" 1 "$tmp/dotted.md"

# Regression: the id is not always bold. Requiring `**` hid 95 of 576
# id-bearing rows in docs/state.md, among them a byte-identical duplicate of
# T-CUDA-MUL24-AUDIT-2026-05-28 that this gate reported as clean.
cat >"$tmp/plain.md" <<'MD'
## Recently closed

| ID | Description |
| --- | --- |
| T-EPSILON-2026-01-04 | not bold, and filed twice |
| T-EPSILON-2026-01-04 | not bold, and filed twice |
MD
expect "a duplicate NON-BOLD id fails" 1 "$tmp/plain.md"

# Regression: upstream rows are keyed by the Netflix issue, not by a T- tag.
# 34 such rows existed and 13 of them were duplicated.
cat >"$tmp/netflix.md" <<'MD'
## Confirmed not-affected (or already-fixed upstream of the fork's master)

| ID | Description |
| --- | --- |
| Netflix#1234 — some upstream report | not affected |
| Netflix#1234 — the same report, kept twice by a rebase | not affected |
MD
expect "a duplicate Netflix# id fails" 1 "$tmp/netflix.md"

# Regression: tranche ids (**T6-1**, **T7-16**) are neither `T-` nor Netflix#.
cat >"$tmp/tranche.md" <<'MD'
## Deferred (waiting on external trigger)

| ID | Description |
| --- | --- |
| **T7-42** — a tranche id | deferred |
| **T7-42** — the same tranche id | deferred |
MD
expect "a duplicate tranche id fails" 1 "$tmp/tranche.md"

# Regression: ~143 rows open with prose and carry no id at all, so the id check
# cannot see them. A later verification sweep appends `_(verified ...)_` to one
# copy, which also defeats a naive verbatim-row comparison. 13 rows were
# duplicated in exactly this shape and were invisible to both checks.
cat >"$tmp/prose.md" <<'MD'
## Recently closed

| Item | Description |
| --- | --- |
| **`foo.c` overflowed on empty input** — one-line summary | fixed in #1 |
| **`foo.c` overflowed on empty input** — one-line summary _(verified 2026-05-09: PR #1 MERGED 2026-05-01.)_ | fixed in #1 |
MD
expect "a prose-led row duplicated modulo a verified-suffix fails" 1 "$tmp/prose.md"

# The row check must NOT fire on the column header, which repeats once per
# section by design. This is the false positive that the first version of the
# row check produced.
cat >"$tmp/headers.md" <<'MD'
## Open bugs

| ID | Description |
| --- | --- |
| **T-ZETA-2026-01-05** | open |

## Recently closed

| ID | Description |
| --- | --- |
| **T-ETA-2026-01-06** | closed |

## Deferred (waiting on external trigger)

| ID | Description |
| --- | --- |
| **T-THETA-2026-01-07** | deferred |
MD
expect "repeated column headers do NOT fail" 0 "$tmp/headers.md"

expect "a missing file is rc=2" 2 "$tmp/nope.md"

# the real file must be clean
expect "docs/state.md is clean" 0 "$here/../../../docs/state.md"
echo "all check-state-md-rows cases passed"
