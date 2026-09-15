#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Require every ADR to have one index fragment with valid ADR references."""

import re
import sys
from pathlib import Path


def check(root: Path) -> list[str]:
    adrs = {path.name for path in root.glob("[0-9][0-9][0-9][0-9]-*.md")}
    expected = adrs - {"0000-template.md"}
    fragments = root / "_index_fragments"
    found = {path.name for path in fragments.glob("[0-9][0-9][0-9][0-9]-*.md")}
    errors = [f"missing ADR index fragment: {name}" for name in sorted(expected - found)]
    errors += [f"index fragment has no ADR: {name}" for name in sorted(found - expected)]
    for name in sorted(found):
        text = (fragments / name).read_text()
        primary = re.match(r"\| \[ADR-(\d{4})\]\(([^)]+)\) \|", text)
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
