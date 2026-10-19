# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Write or compare a set of generated files (ADR-2350 D13).

The tool runners of scripts/codegen (sqlc_generate.py, proto_generate.py,
crd_generate.py) generate into a scratch tree and hand the result here as
{repository path: text}: `differences` lists what the committed tree lacks or
holds stale, `write` makes the tree hold exactly the generated set.
"""

from __future__ import annotations

from pathlib import Path


def collect(root: Path, base: Path, pattern: str) -> dict[str, str]:
    """{path relative to root: text} of the files under base matching pattern."""
    return {
        p.relative_to(root).as_posix(): p.read_text(encoding="utf-8")
        for p in sorted(base.glob(pattern))
        if p.is_file()
    }


def differences(want: dict[str, str], have: dict[str, str]) -> list[str]:
    """Paths that are missing, stale or different."""
    out = [f"missing: {p}" for p in sorted(want.keys() - have.keys())]
    out += [f"stale: {p}" for p in sorted(have.keys() - want.keys())]
    out += [f"differs: {p}" for p in sorted(want.keys() & have.keys()) if want[p] != have[p]]
    return out


def write(root: Path, want: dict[str, str], have: dict[str, str]) -> None:
    """Make the tree hold exactly want: remove the stale paths of have."""
    for path in have.keys() - want.keys():
        (root / path).unlink()
    for path, text in want.items():
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_text(text, encoding="utf-8")
