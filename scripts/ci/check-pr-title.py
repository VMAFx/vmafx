#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""The PR Title Gate (issue #2750).

A pull request's title becomes the subject of the squashed commit on
``master`` — and release-please builds the changelog and the next version
from those subjects. The commit-msg hook only sees commits made on a
machine with the hooks installed, so a contributor without them gets no
signal before review. This gate holds every title to the same rules the
hook applies, from the one shared definition in
``scripts/lib/conventional_commits.py`` (test-pinned to the hook).

Usage:

* ``check-pr-title.py "<title>"`` (or ``PR_TITLE`` env, as the workflow
  passes it) — exit 0 when the title is a Conventional Commit the hook
  would accept, exit 1 with ``::error`` lines naming the missing
  components otherwise.
* ``check-pr-title.py --emit-label "<title>"`` — print the ``type:*``
  label of the leading type for the label workflow; exit 3 when the
  prefix is not a known type (the workflow answers with a notice).
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.conventional_commits import (
    LABELS,
    errors,
    is_conventional,
    parse_type,
)

GATE_DOC = "scripts/lib/conventional_commits.py"


def _emit_label(title: str) -> int:
    label_type = parse_type(title)
    if label_type is None:
        print(
            "no known Conventional-Commit type prefix (%s names the shared list)" % GATE_DOC,
            file=sys.stderr,
        )
        return 3
    print(LABELS[label_type])
    return 0


def main(argv: list[str]) -> int:
    emit_label = "--emit-label" in argv
    args = [a for a in argv if a != "--emit-label"]
    title = args[0] if args else os.environ.get("PR_TITLE", "")

    if emit_label:
        return _emit_label(title)

    if is_conventional(title):
        return 0
    missing = errors(title)
    print(
        "::error title=PR Title Gate::the title is not a Conventional Commit the commit-msg hook would accept",
        flush=True,
    )
    for name in missing:
        detail = {
            "type": "a known type from scripts/lib/conventional_commits.py (feat, fix, ...; see TYPES)",
            "delim": "the colon delimiter, with an optional ! before it for a breaking change",
            "subject": "a subject after the colon (`type: subject`)",
        }.get(name, name)
        print("::error title=PR Title Gate::missing %s: %s" % (name, detail), flush=True)
    print(
        "::error title=PR Title Gate::"
        "the title becomes the squashed commit's subject and release-please reads it; "
        "see docs/principles.md and the commit-msg hook",
        flush=True,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
