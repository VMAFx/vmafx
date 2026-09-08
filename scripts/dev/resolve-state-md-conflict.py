#!/usr/bin/env python3
# scripts/dev/resolve-state-md-conflict.py — resolve a docs/state.md rebase
# conflict the way ADR-0165 requires.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
"""Resolve a docs/state.md rebase conflict the way ADR-0165 requires.

`docs/state.md` is deliberately excluded from the `merge=union` list in
`.gitattributes`, unlike the other append-only bookkeeping files. Its rows
MOVE between the "Open bugs" and "Recently closed" sections, so a keep-both
resolution duplicates the row AND leaves the closed bug reading as open
forever — which is exactly what `scripts/ci/check-state-md-rows.sh` gates.

That leaves every rebase of a state.md-touching branch needing a by-hand
resolution, and "keep both sides" is the tempting wrong answer. This script
applies the correct rule instead:

  * master's side ("ours" during a rebase) is the more advanced state — it
    already carries every row merged ahead of this branch, including any row
    this branch also touches but that master has since moved or reworded.
  * the branch's side ("theirs") contributes only the rows master does not
    have at all: this PR's own new bug id.

So it takes ours whole, then appends only those theirs-rows whose bug id is
not already present. Deduplication is by bug id rather than by line, so a row
master reworded is not re-added in its stale form.

Usage, mid-rebase:

    python3 scripts/dev/resolve-state-md-conflict.py docs/state.md
    scripts/ci/check-state-md-rows.sh      # always verify before continuing
    git add docs/state.md && git rebase --continue

The verification step is not optional. This script encodes the common case —
a branch adding one new row against a master that has moved others. It cannot
know that a row your branch MOVED to "Recently closed" should win over
master's older "Open bugs" copy; in that situation it keeps master's, and you
have to redo the move by hand. The row count in the gate's output is the
cheapest way to notice.
"""

import re
import sys
from pathlib import Path

ID_RE = re.compile(r"\*\*(T-[A-Z0-9-]+)\*\*")

# Built rather than written literally so this file does not itself trip the
# `no-conflict-markers` pre-commit hook.
OURS_MARK = "<" * 7
SPLIT_MARK = "=" * 7
THEIRS_MARK = ">" * 7


def bug_id(line):
    """Return the T-… bug id a state.md row declares, or None for other text."""
    match = ID_RE.search(line)
    return match.group(1) if match else None


def resolve(path):
    """Rewrite `path` in place, resolving every conflict hunk. Returns the count."""
    lines = Path(path).read_text(encoding="utf-8").split("\n")
    out = []
    index = 0
    hunks = 0
    while index < len(lines):
        if not lines[index].startswith(OURS_MARK):
            out.append(lines[index])
            index += 1
            continue

        ours = []
        theirs = []
        index += 1
        while not lines[index].startswith(SPLIT_MARK):
            ours.append(lines[index])
            index += 1
        index += 1
        while not lines[index].startswith(THEIRS_MARK):
            theirs.append(lines[index])
            index += 1
        index += 1
        hunks += 1

        # master's side wins wholesale; the branch adds only unseen bug ids.
        out.extend(ours)
        seen = {bug_id(line) for line in out if bug_id(line)}
        for line in theirs:
            bid = bug_id(line)
            if bid is None:
                # Non-row text (a blank line, a heading): keep it only when the
                # ours side did not already supply it.
                if line.strip() and line not in ours:
                    out.append(line)
            elif bid not in seen:
                out.append(line)
                seen.add(bid)

    Path(path).write_text("\n".join(out), encoding="utf-8")
    return hunks


MIN_ARGV = 2


def main(argv):
    if len(argv) < MIN_ARGV:
        print(f"usage: {argv[0]} docs/state.md [...]", file=sys.stderr)
        return 2
    for path in argv[1:]:
        count = resolve(path)
        print(f"resolved {count} hunk(s) in {path} (ours + branch-only rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
