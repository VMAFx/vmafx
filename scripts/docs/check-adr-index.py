#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Require every ADR to have one lint-clean index fragment."""

import re
import sys
from pathlib import Path

FRAGMENT_PREFIX = (
    "# ADR index entry",
    "",
    "| ID | Title | Status | Tags |",
    "| --- | --- | --- | --- |",
)
DATA_ROW_COUNT = 1
DATA_ROW_CELL_COUNT = 4


def split_table_row(row: str) -> list[str]:
    """Split a generated table row on unescaped pipes."""
    if not row.startswith("|") or not row.endswith("|"):
        return []
    cells: list[str] = []
    start = 1
    for index, character in enumerate(row[1:-1], start=1):
        if character != "|":
            continue
        backslashes = 0
        cursor = index - 1
        while cursor >= 0 and row[cursor] == "\\":
            backslashes += 1
            cursor -= 1
        if backslashes % 2 == 0:
            cells.append(row[start:index].strip())
            start = index + 1
    cells.append(row[start:-1].strip())
    return cells


def check(root: Path) -> list[str]:
    adrs = {path.name for path in root.glob("[0-9][0-9][0-9][0-9]-*.md")}
    expected = adrs - {"0000-template.md"}
    fragments = root / "_index_fragments"
    found = {path.name for path in fragments.glob("[0-9][0-9][0-9][0-9]-*.md")}
    errors = [f"missing ADR index fragment: {name}" for name in sorted(expected - found)]
    errors += [f"index fragment has no ADR: {name}" for name in sorted(found - expected)]
    for name in sorted(found):
        text = (fragments / name).read_text()
        lines = text.splitlines()
        if tuple(lines[:4]) != FRAGMENT_PREFIX:
            errors.append(f"index fragment is not a standalone canonical document: {name}")
        data_rows = [line for line in lines if line.startswith("| [ADR-")]
        if len(data_rows) != DATA_ROW_COUNT or len(lines) != len(FRAGMENT_PREFIX) + DATA_ROW_COUNT:
            errors.append(f"index fragment must contain exactly one data row: {name}")
            continue
        cells = split_table_row(data_rows[0])
        if len(cells) != DATA_ROW_CELL_COUNT or any(not cell for cell in cells):
            errors.append(f"index fragment data row must contain four non-empty cells: {name}")
            continue
        primary = re.fullmatch(r"\[ADR-(\d{4})\]\(([^)]+)\)", cells[0])
        if primary is None or primary.group(1) != name[:4] or primary.group(2) != name:
            errors.append(f"index fragment primary ID/link does not match filename: {name}")
        for target in re.findall(r"\]\((\d{4}-[^)#]+\.md)(?:#[^)]*)?\)", text):
            if target not in adrs:
                errors.append(f"{name}: missing ADR reference {target}")
    order = fragments / "_order.txt"
    if order.exists():
        seen: set[str] = set()
        for slug in order.read_text().splitlines():
            if not slug or slug.startswith("#"):
                continue
            if slug in seen:
                errors.append(f"duplicate ADR index order entry: {slug}")
            if f"{slug}.md" not in found:
                errors.append(f"ADR index order entry has no fragment: {slug}")
            seen.add(slug)
    return errors


def main() -> int:
    root = Path(__file__).resolve().parents[2] / "docs/adr"
    errors = check(root)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
