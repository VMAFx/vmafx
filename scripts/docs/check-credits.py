#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hold ``docs/credits.yaml`` and ``docs/credits.md`` to the repository (ADR-2485).

Fails on: page drift, a vendored or inherited third-party path with no entry, a
``LICENSES/*.txt`` licence nothing uses, a skill or agent file derived from an
upstream with no entry, and an entry path that is not in the checkout. See
``scripts/docs/credits_checks.py`` for what each check reads.

Exit status: 0 clean, 1 findings, 2 the list or the checkout cannot be read.
Wired into ``make docs-fragments-check``.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.docs.credits_checks import run_all  # noqa: E402
from scripts.docs.credits_lib import (  # noqa: E402
    CreditsError,
    load_entries,
    load_waivers,
    tracked_files,
)


def main(argv: list[str], root: Path = ROOT) -> int:
    argparse.ArgumentParser(description=__doc__.split("\n", 1)[0]).parse_args(argv)
    try:
        listing = root / "docs" / "credits.yaml"
        entries = load_entries(listing)
        findings = run_all(root, tracked_files(root), entries, load_waivers(listing))
    except (CreditsError, OSError) as err:
        print(f"check-credits: {err}", file=sys.stderr)
        return 2
    failed = 0
    for check, lines in findings.items():
        for line in lines:
            print(f"::error title=credits {check}::{line}")
            failed += 1
    if failed:
        print(f"check-credits: {failed} finding(s). See docs/credits.md, 'Add an entry'.")
        return 1
    print(f"check-credits: OK - {len(entries)} entries, five checks clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
