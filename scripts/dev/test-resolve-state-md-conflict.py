#!/usr/bin/env python3
# scripts/dev/test-resolve-state-md-conflict.py — regression test for the
# docs/state.md conflict resolver.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
"""Feed the resolver a synthetic conflict covering the three cases that matter.

  * a row only master has  -> survives untouched
  * a row both sides have  -> master's wording wins, exactly once
  * a row only the branch has, duplicated by an earlier bad rebase
                           -> kept exactly once

Usage:
    python3 scripts/dev/test-resolve-state-md-conflict.py \
        [scripts/dev/resolve-state-md-conflict.py]
"""

import subprocess
import sys
import tempfile
from pathlib import Path

# Assembled rather than written literally so this file does not itself trip the
# `no-conflict-markers` pre-commit hook.
CONFLICT = "\n".join(
    [
        "# state",
        "## Open bugs",
        "",
        "| **T-MASTER-ROW-2026-09-01** | master's own new bug. | ref |",
        "<" * 7 + " ours",
        "| **T-SHARED-ROW-2026-08-01** | master reworded this row. | ref |",
        "=" * 7,
        "| **T-SHARED-ROW-2026-08-01** | the branch's stale copy. | ref |",
        "| **T-BRANCH-ONLY-2026-09-07** | the branch's own new bug. | ref |",
        "| **T-BRANCH-ONLY-2026-09-07** | duplicated by a bad rebase. | ref |",
        ">" * 7 + " theirs",
        "",
    ]
)

DEFAULT_SCRIPT = Path(__file__).with_name("resolve-state-md-conflict.py")


def check(out):
    """Return the list of assertion failures for a resolved state.md body."""
    failures = []
    if any(mark * 7 in out for mark in ("<", "=", ">")):
        failures.append("conflict markers survived")
    if out.count("T-SHARED-ROW-2026-08-01") != 1:
        failures.append("shared row not deduplicated")
    if "master reworded this row" not in out:
        failures.append("master's wording lost")
    if "the branch's stale copy" in out:
        failures.append("branch's stale copy kept")
    if out.count("T-BRANCH-ONLY-2026-09-07") != 1:
        failures.append("branch-only row not deduplicated")
    if "T-MASTER-ROW-2026-09-01" not in out:
        failures.append("untouched master row lost")
    return failures


def main(argv):
    script = Path(argv[1]).resolve() if len(argv) > 1 else DEFAULT_SCRIPT.resolve()
    with tempfile.TemporaryDirectory() as tmp:
        target = Path(tmp) / "state.md"
        target.write_text(CONFLICT, encoding="utf-8")
        subprocess.run(  # noqa: S603 — fixed argv, no shell, no user input
            [sys.executable, str(script), str(target)],
            check=True,
        )
        failures = check(target.read_text(encoding="utf-8"))

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: all 6 assertions")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
