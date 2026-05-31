# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage tests for the lazy ``__getattr__`` in :mod:`aiutils`.

Also nails down the remaining missing line in :mod:`aiutils.jsonl_utils`
(``_sanitize_nonfinite`` recursing through a list).
"""

from __future__ import annotations

import math

import pytest

import aiutils
from aiutils.jsonl_utils import _sanitize_nonfinite, dumps_jsonl_row


def test_lazy_write_parquet_atomic_resolves_via_getattr() -> None:
    """``aiutils.write_parquet_atomic`` is only imported on first access."""
    fn = aiutils.write_parquet_atomic  # exercises the ``__getattr__`` path
    assert callable(fn)
    # Importing the eager module yields the same callable.
    from aiutils.parquet_utils import write_parquet_atomic as direct

    assert fn is direct


def test_lazy_getattr_raises_on_unknown_attribute() -> None:
    """Unknown attributes must raise AttributeError, not return None or fall through."""
    # Use the variable name to defeat ruff B009 (constant attr name in getattr).
    name = "no_such_thing"
    with pytest.raises(AttributeError, match="has no attribute 'no_such_thing'"):
        getattr(aiutils, name)


def test_aiutils_all_lists_lazy_export() -> None:
    """``write_parquet_atomic`` is declared in ``__all__`` (lazy or not)."""
    assert "write_parquet_atomic" in aiutils.__all__


def test_sanitize_nonfinite_recurses_into_list_with_nan() -> None:
    """Cover the ``isinstance(obj, list)`` recursion branch in jsonl_utils."""
    out = _sanitize_nonfinite([1.0, math.nan, [math.inf, "ok"]])
    assert out == [1.0, None, [None, "ok"]]


def test_dumps_jsonl_row_top_level_list_inside_dict_value() -> None:
    """Round-trip a dict containing a list of non-finite floats."""
    raw = dumps_jsonl_row({"xs": [1.0, math.inf, -math.inf, math.nan]})
    assert "NaN" not in raw
    assert "Infinity" not in raw
    import json

    parsed = json.loads(raw)
    assert parsed == {"xs": [1.0, None, None, None]}
