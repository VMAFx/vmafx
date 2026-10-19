#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fail when a tracked translation unit is read by no clang-tidy lane.

Every tracked C, C++, CUDA, HIP, Objective-C++ and Metal translation unit must
be in the ``measured_sources`` of at least one ``scripts/ci/tidy-baseline-*.json``
or in the shared lint exception list ``.config/lint-exceptions.d/clang-tidy-coverage.toml``
(format and expiry rules: ``scripts/ci/lint_exceptions.py``). An expired entry no longer
excuses its file; an entry for a file a lane reads after all is stale and fails.

Exit 0 covered, 1 a coverage gap or a bad exception list, 2 unreadable input.
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lint_exceptions  # noqa: E402 -- the shared exception list reader

UNIT_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".cu", ".hip", ".mm", ".metal")
# Data fragments named like sources (one file per declared exact twin).
DATA_PREFIXES = ("scripts/ci/exact_twins.d/",)
# Files named like a translation unit that are headers: textually included by
# measured units, so clang-tidy reads them through their includers.
INCLUDED_HEADERS = frozenset(
    {"core/src/feature/hip/integer_adm/adm_decouple_inline.hip"}  # adm_cm.hip, adm_csf.hip
)
RULE = "clang-tidy-coverage"


def tracked_units(root: Path) -> list[str]:
    return sorted(
        p
        for p in lint_exceptions.tracked_files(root)
        if p.endswith(UNIT_SUFFIXES)
        and not p.startswith(DATA_PREFIXES)
        and p not in INCLUDED_HEADERS
    )


def measured(root: Path) -> set[str]:
    seen: set[str] = set()
    for path in sorted((root / "scripts/ci").glob("tidy-baseline-*.json")):
        seen.update(json.loads(path.read_text(encoding="utf-8")).get("measured_sources", []))
    return seen


def problems(
    units: list[str], covered: set[str], entries: list[lint_exceptions.Entry], today: datetime.date
) -> list[str]:
    found: list[str] = []
    live = {e.path for e in entries if e.rule == RULE and e.expires >= today}
    for entry in entries:
        if entry.rule != RULE:
            continue
        if entry.path not in units:
            found.append(f"{entry.path}: exception for a file that is not a tracked unit")
        elif entry.path in covered:
            found.append(f"{entry.path}: exception for a file a lane reads; delete the entry")
        elif entry.expires < today:
            found.append(f"{entry.path}: exception expired on {entry.expires}")
    for path in units:
        if path not in covered and path not in live:
            found.append(f"{path}: read by no clang-tidy lane and not in the exception list")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--today", type=datetime.date.fromisoformat, default=None)
    args = parser.parse_args(argv)
    root: Path = args.root
    try:
        today = args.today or datetime.date.today()
        entries, malformed = lint_exceptions.load(root)
        found = malformed + problems(tracked_units(root), measured(root), entries, today)
    except (OSError, ValueError, SystemExit) as exc:
        print(f"check-tidy-coverage: {exc}", file=sys.stderr)
        return 2
    for line in found:
        print(f"check-tidy-coverage: {line}")
    if found:
        print(f"check-tidy-coverage: {len(found)} problem(s)")
        return 1
    print("check-tidy-coverage: every tracked translation unit is read or excepted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
