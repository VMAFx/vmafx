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
# A duplicate row is only one of the two ways a closed bug can read as open
# forever. The other needs no duplicate at all: the row is filed under
# "## Open bugs" while its own rightmost cell already says `closed` or `fixed`.
# That happens when a PR appends its row to the section it was reading instead
# of moving it, or when a rebase drops the move hunk but keeps the status edit.
# Nothing about it is a duplicate, so the two checks above cannot see it, and
# 24 of the 62 rows under "## Open bugs" were in that state on 2026-09-21 --
# including this gate's own row. The third check below reads the section
# heading each row sits under and the status token in its last cell, and
# requires them to agree: `closed` / `fixed` / `resolved` / `done` may not sit
# under "## Open bugs", and `open` may not sit under "## Recently closed".
#
# The judged cell is the one the table header calls `Status`, or the last
# non-empty cell when no header names one, and only the token that OPENS that
# cell is read. Requiring the whole cell to BE a status token judged `| fixed |`
# and skipped `| fixed (PR #1425) |` -- the same bug in the same shape, one
# parenthetical later -- along with 20 other live rows whose status leads a
# longer cell. Several row shapes make no status claim at all: a verification
# date, a branch name, prose. Those lead with no status token and are left alone
# rather than guessed at; guessing at them fabricates failures, and eight live
# rows end in a branch name.
#
# What the status check does NOT guarantee is that every misfiled row is seen.
# It reads one cell per row, so a status the extraction does not recognise --
# buried mid-cell, sitting in a column that is neither the last nor headed
# `Status`, or spelled outside the closed/fixed/resolved/done/open vocabulary --
# is passed over in silence. The check is a floor, not a ceiling: it catches the
# shapes below and says nothing about the rest.
#
# It does fail closed on one specific way it would otherwise be silently
# disabled: renaming or deleting a section heading. If any row in the file
# claims a status that belongs to a section, that section's heading has to
# exist -- otherwise every such row would land in an ungated section and the
# check would pass by doing nothing. That is one blind spot closed, not all of
# them.
#
# Usage: check-state-md-rows.sh [PATH]   (default: docs/state.md)
# Exit:  0 clean; 1 a duplicate id, a repeated row, or a row whose status
#        disagrees with its section; 2 the file is missing.
set -euo pipefail

file="${1:-docs/state.md}"
if [[ ! -f "$file" ]]; then
  echo "check-state-md-rows: $file not found" >&2
  exit 2
fi

report="$(awk '
  # Section headings partition the ledger. A row\047s status token only means
  # anything relative to the section the row is filed under, so the heading is
  # tracked as the rows stream past.
  /^## / {
    sec = substr($0, 4)
    sub(/[[:space:]]+$/, "", sec)
    if (sec == "Open bugs") have_open = 1
    else if (sec ~ /^Recently closed/) have_closed = 1
    prev = ""
    prevline = 0
    statcol = 0
    next
  }

  # A |---|---| separator means the PREVIOUS line was a column header, not a
  # data row. Headers repeat once per section by design (`| ID | Description |`
  # appears above every table), so counting them as duplicate rows is a false
  # positive -- and was: it failed this gate\047s own "unique ids pass" fixture.
  /^\|[[:space:]]*:?-+:?[[:space:]]*\|/ {
    if (prev != "") { rowcount[prev]--; rowlines[prev] = "" }
    # The header carries a column label, not a status, in its judged cell
    # (`| Bug | Summary | Reproducer | Owner | Target |`). Drop it the same way.
    # `prev` and `prevline` are set together and cleared together on every path,
    # so this guard is the same one as the retraction above, not a laxer one.
    if (prevline != 0) { delete statword[prevline] }

    # The header also names the columns, and the status column is not always
    # the last: `| ... | Date | Status |` becomes `| ... | Status | PR |` the
    # moment one column is appended, and the trailing cell then carries no
    # status claim. When a header names a `Status` column, every row of that
    # table is judged on it instead. No table in docs/state.md has such a
    # header today, so this changes nothing there -- it stops the gate going
    # blind the day one does.
    statcol = 0
    headsrc = prev
    gsub(/\\\|/, "", headsrc)
    nhead = split(headsrc, hcell, "|")
    for (hi = 2; hi < nhead; hi++) {
      hname = tolower(hcell[hi])
      gsub(/[[:space:]]|\*|_|`/, "", hname)
      if (hname == "status") statcol = hi
    }
    prev = ""
    prevline = 0
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
    prevline = NR

    # The status lives in the `Status` column when the table header named one,
    # and in the last non-empty cell otherwise. `\|` inside an inline code span
    # is an escaped pipe, not a cell boundary -- docs/state.md spells it that
    # way (`\|img - blur(img)\|`) -- so it is dropped before the split, which
    # keeps the cell indices aligned with what the renderer shows. An UNescaped
    # pipe in a code span still mis-splits: harmless for the trailing cell,
    # since a mis-split only makes the tail shorter and a cell that does not
    # lead with a status token is skipped anyway, but it would shift a named
    # `Status` column, and that row would then be judged on the wrong cell.
    cellsrc = $0
    gsub(/\\\|/, "", cellsrc)
    ncell = split(cellsrc, cell, "|")
    if (statcol > 1 && statcol <= ncell) {
      last = cell[statcol]
    } else {
      last = cell[ncell]
      if (last ~ /^[[:space:]]*$/ && ncell > 1) last = cell[ncell - 1]
    }

    # Read the token that OPENS the cell, not the whole cell. Demanding the
    # whole cell judged `| fixed |` and skipped `| fixed (PR #1425) |`, which is
    # the same misfiled row one parenthetical later, plus `| Fixed locally; ...|`
    # and `| CLOSED 2026-06-20: fixed by PR #1022 ... |`. Reading only the
    # leading word still leaves the no-claim shapes unjudged: a branch name
    # leads with `fix`, not `fixed` (`fix/hip-pageable-upload-race`), a date or
    # a parenthetical leads with no word at all (`(2026-06-04)`), and prose
    # leads with a word outside the vocabulary (`Closes when the parity test
    # passes`).
    gsub(/[*_`]/, "", last)
    sub(/^[[:space:]]+/, "", last)
    last = tolower(last)
    statword[NR] = (match(last, /^[a-z]+/) ? substr(last, 1, RLENGTH) : "")
    statsec[NR] = sec
    statrow[NR] = substr(row, 1, 70)

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
  # A non-table line ends the table: the next `|---|` separator belongs to a
  # different header, and `prevline` must not still point at a data row from
  # the table above -- deleting that row\047s status would silently unjudge it.
  !/^\|/ { prev = ""; prevline = 0; statcol = 0 }

  END {
    bad = 0
    for (id in idcount)
      if (idcount[id] > 1) { printf "ID\t%s\t%s\n", id, idlines[id]; bad = 1 }
    for (row in rowcount)
      if (rowcount[row] > 1) { printf "ROW\t%s\t%s\n", substr(row, 1, 100), rowlines[row]; bad = 1 }

    # A status token claims one of the two gated sections. Anything else --
    # `deferred`, `watching`, a verification date, a branch name, prose -- makes
    # no section claim and is not judged.
    resolved = " closed fixed resolved done "
    for (ln in statword) {
      s = statword[ln]
      if (s == "") continue
      claims = ""
      if (s == "open") claims = "open"
      else if (index(resolved, " " s " ") > 0) claims = "closed"
      if (claims == "") continue
      if (claims == "open") {
        used_open = 1
      } else {
        used_closed = 1
      }

      if (statsec[ln] == "Open bugs") here = "open"
      else if (statsec[ln] ~ /^Recently closed/) here = "closed"
      else continue

      checked++
      if (here != claims) {
        printf "SECTION\t%d\t%s\t%s\t%s\n", ln, s, statsec[ln], statrow[ln]
        bad = 1
      }
    }

    # Fail closed: a status that claims a section, with no such heading in the
    # file, means every row making that claim sits in an ungated section.
    if (used_open && !have_open)
      printf "NOSECTION\topen\tOpen bugs\n"
    if (used_closed && !have_closed)
      printf "NOSECTION\tclosed/fixed\tRecently closed\n"

    printf "COUNT\t%d\t%d\n", ids, checked
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

# A row whose status disagrees with the section it is filed under. No duplicate
# is involved, so the two checks above cannot see it.
misfiled="$(printf '%s\n' "$report" | grep -E '^SECTION' | sort -t$'\t' -k2,2n || true)"
nosection="$(printf '%s\n' "$report" | grep -E '^NOSECTION' || true)"

if [[ -n "$misfiled" || -n "$nosection" ]]; then
  echo "::error title=state.md misfiled rows::a row's status must match its section (ADR-0165)" >&2
  while IFS=$'\t' read -r _kind line status section rowtext; do
    [[ -z "${line:-}" ]] && continue
    echo "  line $line is filed under '## $section' but its status says '$status'" >&2
    echo "    ${rowtext}..." >&2
  done <<<"$misfiled"
  while IFS=$'\t' read -r _kind status section; do
    [[ -z "${status:-}" ]] && continue
    echo "  rows claim status '$status' but there is no '## $section' heading" >&2
    echo "    every such row would sit in an ungated section and this check" >&2
    echo "    would pass by doing nothing -- restore the heading" >&2
  done <<<"$nosection"
  echo "" >&2
  echo "Move the row into the section its status claims rather than editing the" >&2
  echo "status to match where it landed: 'closed' / 'fixed' / 'resolved' /" >&2
  echo "'done' belong under '## Recently closed', 'open' under '## Open bugs'." >&2
  echo "A PR that fixes a bug MOVES its row (ADR-0165 update protocol step 1);" >&2
  echo "appending a resolved row to '## Open bugs' leaves the bug reading as" >&2
  echo "open forever, which is the failure this file exists to prevent." >&2
  exit 1
fi

count="$(printf '%s\n' "$report" | awk -F'\t' '$1=="COUNT"{print $2}')"
checked="$(printf '%s\n' "$report" | awk -F'\t' '$1=="COUNT"{print $3}')"
echo "check-state-md-rows: OK ($count id-bearing rows, no duplicate ids or rows;" \
  "$checked status-bearing rows, each in the section its status claims)"
