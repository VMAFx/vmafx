# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for shared AI JSONL helpers."""

from __future__ import annotations

import json
import math
from pathlib import Path

from aiutils.jsonl_utils import dumps_jsonl_row, iter_jsonl


def test_dumps_jsonl_row_is_strict_and_sorted() -> None:
    raw = dumps_jsonl_row({"z": math.inf, "a": 1.5, "nested": {"nan": math.nan}})

    assert raw.endswith("\n")
    assert "Infinity" not in raw
    assert "NaN" not in raw
    assert raw.startswith('{"a":')
    assert json.loads(raw) == {"a": 1.5, "nested": {"nan": None}, "z": None}


def test_dumps_jsonl_row_preserves_compact_separator_option() -> None:
    assert dumps_jsonl_row({"b": 2, "a": 1}, separators=(",", ":")) == '{"a":1,"b":2}\n'


def test_iter_jsonl_skips_blank_lines(tmp_path: Path) -> None:
    path = tmp_path / "rows.jsonl"
    path.write_text('{"a": 1}\n\n{"b": 2}\n', encoding="utf-8")

    assert list(iter_jsonl(path)) == [(1, {"a": 1}), (3, {"b": 2})]
