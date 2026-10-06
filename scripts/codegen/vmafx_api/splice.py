# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Generated regions inside hand-written files.

A hand-written page or contract keeps its prose and gets a generated table or
schema block between two marker lines. The generator rewrites only the lines
between the markers; the drift check compares the whole file, so a hand edit
inside the region fails like an edit of any generated file. A file that lost a
marker stops generation with the file and the marker named.
"""

from __future__ import annotations

from pathlib import Path

from .model import DefinitionError


def _find(lines: list[str], marker: str, start: int, where: str) -> int:
    for index in range(start, len(lines)):
        if lines[index].strip() == marker:
            return index
    raise DefinitionError(f"{where}: marker line {marker!r} not found")


def splice(text: str, begin: str, end: str, body: list[str], where: str) -> str:
    """`text` with the lines between the `begin` and `end` marker lines replaced."""
    lines = text.split("\n")
    first = _find(lines, begin, 0, where)
    last = _find(lines, end, first + 1, where)
    return "\n".join([*lines[: first + 1], *body, *lines[last:]])


def spliced(root: Path, path: str, begin: str, end: str, body: list[str]) -> str:
    """The file at `root / path` with its generated region rendered."""
    target = root / path
    if not target.exists():
        raise DefinitionError(f"{path}: file with generated region not found")
    return splice(target.read_text(encoding="utf-8"), begin, end, body, path)
