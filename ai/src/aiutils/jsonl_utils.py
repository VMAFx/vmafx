# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""JSONL file I/O utilities."""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any, Iterator

from aiutils.tree_utils import map_tree


def _null_nonfinite(value: Any) -> Any:
    """Return ``None`` for a non-finite float, else ``value``."""
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def _sanitize_nonfinite(obj: Any) -> Any:
    """Replace non-finite floats (NaN, Infinity) nested in dicts/lists with None.

    Standard JSON does not support NaN or Infinity; replacing with null
    (None) keeps the document valid while preserving all other fields.
    """
    return map_tree(
        obj,
        _null_nonfinite,
        is_mapping=lambda v: isinstance(v, dict),
        is_sequence=lambda v: isinstance(v, list),
    )


def dumps_jsonl_row(obj: dict[str, Any], **kwargs: Any) -> str:
    """Serialise *obj* to a single compact, sorted, newline-terminated JSON line.

    Non-finite float values (``math.nan``, ``math.inf``, ``-math.inf``) are
    replaced with ``null`` so the output is valid RFC 8259 JSON.  Keys are
    sorted deterministically.  The result always ends with ``'\\n'``.

    Args:
        obj:     The dict to serialise.
        **kwargs: Extra keyword arguments forwarded to :func:`json.dumps`
                 (e.g. ``separators`` to control spacing).

    Returns:
        A JSON string terminated by a newline character.
    """
    sanitised = _sanitize_nonfinite(obj)
    kwargs.setdefault("sort_keys", True)
    return json.dumps(sanitised, **kwargs) + "\n"


def iter_jsonl(path: Path) -> Iterator[tuple[int, dict[str, Any]]]:
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
