#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Render the tables of ``docs/credits.md`` from ``docs/credits.yaml``.

The page keeps its hand-written prose; only the text between the
``<!-- credits:table ID -->`` and ``<!-- credits:end -->`` markers is generated.

Flags:
    --check   Exit 1 when the page differs from the rendering.
    --write   Rewrite the page.

Wired into ``make docs-fragments-check`` / ``make docs-fragments-write``
(ADR-0221 pattern, ADR-2485). ``scripts/docs/check-credits.py`` holds the list
to the repository.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.docs.credits_lib import (  # noqa: E402
    CreditsError,
    apply_blocks,
    blocks,
    load_entries,
)

LIST = Path("docs") / "credits.yaml"
PAGE = Path("docs") / "credits.md"


def render(root: Path) -> tuple[str, str]:
    """Return the page as it is and as the list renders it."""
    entries = load_entries(root / LIST)
    current = (root / PAGE).read_text(encoding="utf-8")
    return current, apply_blocks(current, blocks(entries))


def main(argv: list[str], root: Path = ROOT) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    args = parser.parse_args(argv)
    try:
        current, wanted = render(root)
    except (CreditsError, OSError) as err:
        print(f"generate-credits: {err}", file=sys.stderr)
        return 2
    if args.write:
        if wanted != current:
            (root / PAGE).write_text(wanted, encoding="utf-8")
            print(f"generate-credits: wrote {PAGE}")
        return 0
    if wanted != current:
        print(f"generate-credits: {PAGE} differs from {LIST}; run `make docs-fragments-write`")
        return 1
    print(f"generate-credits: {PAGE} matches {LIST}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
