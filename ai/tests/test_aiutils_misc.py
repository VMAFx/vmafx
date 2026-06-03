# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Targeted coverage for the small ``aiutils`` modules.

Covers the branches in ``time_utils`` and the package ``__init__``
lazy-import shim that the broader test suite does not exercise.
``subprocess_utils`` is covered in the dedicated ``test_subprocess_utils``
module.
"""

from __future__ import annotations

import datetime as _dt
import re

import pytest

import aiutils
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


# ------------------------------------------------------- aiutils package


def test_aiutils_lazy_imports_parquet_writer() -> None:
    """Attribute access on ``aiutils.write_parquet_atomic`` lazy-loads parquet_utils."""
    fn = aiutils.write_parquet_atomic
    assert callable(fn)
    from aiutils.parquet_utils import write_parquet_atomic

    assert fn is write_parquet_atomic


def test_aiutils_getattr_raises_for_unknown_name() -> None:
    """Unknown attribute on the package raises a normal AttributeError."""
    with pytest.raises(AttributeError, match="no attribute 'nonexistent_thing'"):
        _ = aiutils.nonexistent_thing  # type: ignore[attr-defined]
