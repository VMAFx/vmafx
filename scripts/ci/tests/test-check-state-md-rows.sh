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

# Regression: a closed bug reads as open forever without any duplicate at all
# when its row is filed under "## Open bugs" while its own status cell already
# says `closed` or `fixed`. 24 of the 62 rows under "## Open bugs" were in that
# state on 2026-09-21 -- including the row for this gate's own bug,
# T-STATE-MD-ROW-GATE-BLIND-2026-09-16 -- and both checks above reported the
# file clean, because nothing about it is a duplicate.
cat >"$tmp/misfiled.md" <<'MD'
## Open bugs

| ID | Description | ADR | PR | Date | Status |
| --- | --- | --- | --- | --- | --- |
| **T-STATE-MD-ROW-GATE-BLIND-2026-09-16** | the gate was blind | — | PR #1425 | 2026-09-16 | fixed |

## Recently closed

| ID | Description | ADR | PR | Date | Status |
| --- | --- | --- | --- | --- | --- |
| **T-IOTA-2026-01-08** | genuinely closed | — | PR #2 | 2026-01-08 | closed |
MD
expect "a 'fixed' row under Open bugs fails" 1 "$tmp/misfiled.md"

# Regression: a bookkeeping move can leave an open row in place while adding a
# tombstone that explicitly says the same id moved to Recently closed. The row
# has no Status column, so status-token checks cannot infer the contradiction.
cat >"$tmp/moved-tombstone.md" <<'MD'
## Open bugs

| ID | Description |
| --- | --- |
| **T-MOVED-2026-01-17** | stale open wording |
<!-- T-MOVED-2026-01-17 moved to Recently closed — fixed by PR #17 -->

## Recently closed

| ID | Description |
| --- | --- |
| **T-OTHER-2026-01-18** | fixed |
MD
expect "an Open row contradicted by a moved-to-closed tombstone fails" 1 \
  "$tmp/moved-tombstone.md"

cat >"$tmp/valid-move.md" <<'MD'
## Open bugs

<!-- T-MOVED-2026-01-17 moved to Recently closed — fixed by PR #17 -->

## Recently closed

| ID | Description | Status |
| --- | --- | --- |
| **T-MOVED-2026-01-17** | authoritative moved row | closed |
MD
expect "a tombstone plus its single Recently closed row passes" 0 \
  "$tmp/valid-move.md"

# The mirror case: a row that still says `open` filed under Recently closed.
sed -e 's/| fixed |/| open |/' -e 's/| closed |/| open |/' \
  "$tmp/misfiled.md" >"$tmp/reopened.md"
expect "an 'open' row under Recently closed fails" 1 "$tmp/reopened.md"

# Regression: the status is the token that OPENS the judged cell, not the whole
# cell. The first version compared the whole normalised cell against the
# vocabulary, so it caught `| fixed |` and passed `| fixed (PR #1425) |` -- the
# same misfiled row, one parenthetical later. The fixture is misfiled.md with
# exactly that one edit, and it passed (rc=0) before the widening, so rc=1 here
# is attributable to the suffix and nothing else. 21 live rows in docs/state.md
# carry their status in that shape.
sed 's/| fixed |/| fixed (PR #1425) |/' "$tmp/misfiled.md" >"$tmp/suffixed.md"
expect "a 'fixed (...)' row under Open bugs fails" 1 "$tmp/suffixed.md"

# Regression: the status column is not always the last one. Appending a single
# column after `Status` moved the status out of the judged cell, and the same
# misfiled row passed. The header names the column, so it is read by name.
cat >"$tmp/trailing.md" <<'MD'
## Open bugs

| ID | Description | ADR | Status | PR |
| --- | --- | --- | --- | --- |
| **T-STATE-MD-ROW-GATE-BLIND-2026-09-16** | the gate was blind | — | fixed | PR #1425 |

## Recently closed

| ID | Description | ADR | Status | PR |
| --- | --- | --- | --- | --- |
| **T-IOTA-2026-01-08** | genuinely closed | — | closed | PR #2 |
MD
expect "a misfiled row whose Status column is not last fails" 1 "$tmp/trailing.md"

# Regression: `\|` inside an inline code span is an escaped pipe, not a cell
# boundary -- docs/state.md spells it that way (`\|img - blur(img)\|`). Splitting
# the raw line on `|` shifts every later cell by one, so the column the header
# named `Status` is read from the wrong place and the misfiled row passes.
cat >"$tmp/escaped.md" <<'MD'
## Open bugs

| ID | Description | Status |
| --- | --- | --- |
| **T-RHO-2026-01-16** | matched `\| **T-ID**` only | fixed |
MD
expect "an escaped pipe does not shift the judged column" 1 "$tmp/escaped.md"

# Regression: a `|---|` separator retracts the record for the line above it,
# which is its column header. When the header is gone -- a rebase that drops the
# header hunk leaves the separator behind -- the retraction used to reach past
# the blank line and delete the status of a real data row, silently unjudging
# it. This file passed (rc=0) before `prevline` was reset alongside `prev`.
cat >"$tmp/orphan.md" <<'MD'
## Open bugs

| **T-OMICRON-2026-01-14** | the gate was blind | fixed |

| --- | --- | --- |
| **T-PI-2026-01-15** | still broken | open |
MD
expect "a headerless separator does not unjudge the row above it" 1 "$tmp/orphan.md"

# Fail closed: renaming or deleting the heading must not turn the check into a
# no-op by moving every status-bearing row into an ungated section. The fixture
# has to isolate that. Deriving it from misfiled.md did not: that file already
# fails on its misfiled row, so the case passed whether the fail-closed branch
# ran or not. Here every row agrees with the section it is filed under, the
# control passes, and the two files differ by the heading alone -- so the
# failure can only come from the missing heading.
cat >"$tmp/heading-control.md" <<'MD'
## Open bugs

| ID | Description | Status |
| --- | --- | --- |
| **T-NU-2026-01-12** | still broken | open |

## Recently closed

| ID | Description | Status |
| --- | --- | --- |
| **T-XI-2026-01-13** | genuinely closed | closed |
MD
expect "the fail-closed control passes with both headings present" 0 "$tmp/heading-control.md"
sed 's/^## Recently closed$/## Closed a while ago/' \
  "$tmp/heading-control.md" >"$tmp/renamed.md"
expect "a renamed section heading fails instead of skipping" 1 "$tmp/renamed.md"

# Rows whose last cell is a verification date, a branch name or prose make no
# status claim. Guessing at them would fabricate failures, so they pass.
cat >"$tmp/nostatus.md" <<'MD'
## Open bugs

| ID | Description | Verification |
| --- | --- | --- |
| **T-KAPPA-2026-01-09** | still broken | Closes when the parity test passes |

## Recently closed

| ID | Description | Verification | Branch |
| --- | --- | --- | --- |
| **T-LAMBDA-2026-01-10** | closed | 12 of 12 runs pass | `fix/lambda` |
| **T-MU-2026-01-11** | closed | reducer no longer reproduces | (2026-01-11) |
MD
expect "rows with no status token in the last cell pass" 0 "$tmp/nostatus.md"

expect "a missing file is rc=2" 2 "$tmp/nope.md"

# the real file must be clean
expect "docs/state.md is clean" 0 "$here/../../../docs/state.md"
echo "all check-state-md-rows cases passed"
