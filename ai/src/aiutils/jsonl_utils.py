# Copyright 2026 Lusoris and Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""JSONL file I/O utilities."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Iterator


def iter_jsonl(path: Path) -> Iterator[tuple[int, dict]]:
    """Yield (line_no, row) tuples from a JSONL file. Skips blank lines.

    Args:
        path: Path to a JSONL file (newline-delimited JSON objects).

    Yields:
        Tuple of (line_no, parsed_dict) for each non-blank line.
        line_no is 1-indexed.

    Raises:
        SystemExit: If a non-blank line contains invalid JSON.
    """
    with path.open("r", encoding="utf-8") as fp:
        for line_no, line in enumerate(fp, start=1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                yield line_no, json.loads(stripped)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"error: {path}:{line_no}: invalid JSON ({exc})") from exc
