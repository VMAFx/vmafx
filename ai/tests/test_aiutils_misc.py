# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Targeted coverage for the small ``aiutils`` modules.

Closes uncovered branches in ``time_utils``, ``jsonl_utils``, and the
package ``__init__`` lazy-import shim that the broader test suite does
not exercise.
"""

from __future__ import annotations

import datetime as _dt
import math
import re
from pathlib import Path

import pytest

import aiutils
from aiutils.jsonl_utils import _sanitize_nonfinite, dumps_jsonl_row, iter_jsonl
from aiutils.time_utils import now_iso_8601

# -------------------------------------------------------------- time_utils


_ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\+00:00$")


def test_now_iso_8601_has_second_precision_and_utc_offset() -> None:
    """Output matches ISO-8601 UTC without microseconds."""
    stamp = now_iso_8601()
    assert _ISO_RE.match(stamp), stamp
    # Round-trip parses cleanly.
    parsed = _dt.datetime.fromisoformat(stamp)
    assert parsed.tzinfo is _dt.timezone.utc
    assert parsed.microsecond == 0


# -------------------------------------------------------------- jsonl_utils


def test_sanitize_nonfinite_recurses_into_list_branch() -> None:
    """List branch (line 24) recurses element-wise and replaces NaN with None."""
    raw = [1.0, math.nan, [math.inf, 2.5]]
    sanitized = _sanitize_nonfinite(raw)
    assert sanitized == [1.0, None, [None, 2.5]]


def test_dumps_jsonl_row_sanitizes_nested_list_of_nonfinite() -> None:
    """Round-trip via dumps_jsonl_row keeps lists structured but nulls non-finites."""
    out = dumps_jsonl_row({"vals": [math.nan, math.inf, -math.inf, 0.5]})
    assert "NaN" not in out and "Infinity" not in out
    assert '"vals": [null, null, null, 0.5]' in out


def test_iter_jsonl_raises_systemexit_on_invalid_line(tmp_path: Path) -> None:
    """Invalid JSON on a non-blank line raises SystemExit citing line number."""
    bad = tmp_path / "bad.jsonl"
    bad.write_text('{"ok": 1}\nnot json at all\n', encoding="utf-8")

    with pytest.raises(SystemExit) as excinfo:
        list(iter_jsonl(bad))

    message = str(excinfo.value)
    assert "bad.jsonl" in message
    assert ":2:" in message  # 1-indexed line number of the bad line


# ------------------------------------------------------- aiutils package


def test_aiutils_lazy_imports_parquet_writer() -> None:
    """Attribute access on ``aiutils.write_parquet_atomic`` lazy-loads parquet_utils."""
    fn = aiutils.write_parquet_atomic
    assert callable(fn)
    # Identity check against the underlying module export.
    from aiutils.parquet_utils import write_parquet_atomic

    assert fn is write_parquet_atomic


def test_aiutils_getattr_raises_for_unknown_name() -> None:
    """Unknown attribute on the package raises a normal AttributeError."""
    with pytest.raises(AttributeError, match="no attribute 'nonexistent_thing'"):
        _ = aiutils.nonexistent_thing  # type: ignore[attr-defined]
