# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for shared AI JSONL helpers (aiutils.jsonl_utils)."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from aiutils.jsonl_utils import iter_jsonl


def test_iter_jsonl_yields_indexed_rows(tmp_path: Path) -> None:
    """Happy-path: rows are yielded with 1-indexed line numbers."""
    path = tmp_path / "rows.jsonl"
    path.write_text('{"a": 1}\n{"b": 2}\n', encoding="utf-8")

    rows = list(iter_jsonl(path))

    assert rows == [(1, {"a": 1}), (2, {"b": 2})]


def test_iter_jsonl_skips_blank_lines(tmp_path: Path) -> None:
    """Blank lines between records are silently skipped."""
    path = tmp_path / "rows.jsonl"
    path.write_text('{"a": 1}\n\n{"b": 2}\n', encoding="utf-8")

    assert list(iter_jsonl(path)) == [(1, {"a": 1}), (3, {"b": 2})]


def test_iter_jsonl_empty_file(tmp_path: Path) -> None:
    """An empty file produces no rows."""
    path = tmp_path / "empty.jsonl"
    path.write_text("", encoding="utf-8")

    assert list(iter_jsonl(path)) == []


def test_iter_jsonl_raises_systemexit_on_invalid_line(tmp_path: Path) -> None:
    """Invalid JSON on a non-blank line raises SystemExit citing line number."""
    bad = tmp_path / "bad.jsonl"
    bad.write_text('{"ok": 1}\nnot json at all\n', encoding="utf-8")

    with pytest.raises(SystemExit) as excinfo:
        list(iter_jsonl(bad))

    message = str(excinfo.value)
    assert "bad.jsonl" in message
    assert ":2:" in message  # 1-indexed line number


def test_iter_jsonl_only_blank_lines(tmp_path: Path) -> None:
    """A file containing only blank lines produces no rows."""
    path = tmp_path / "blanks.jsonl"
    path.write_text("\n\n\n", encoding="utf-8")

    assert list(iter_jsonl(path)) == []


def test_iter_jsonl_preserves_nested_json(tmp_path: Path) -> None:
    """Nested objects are preserved exactly."""
    row = {"nested": {"key": [1, 2, 3]}, "top": "value"}
    path = tmp_path / "nested.jsonl"
    path.write_text(json.dumps(row) + "\n", encoding="utf-8")

    result = list(iter_jsonl(path))

    assert result == [(1, row)]
