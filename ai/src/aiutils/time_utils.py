# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Time and datetime utilities."""

from __future__ import annotations

import datetime as _dt


def now_iso_8601() -> str:
    """Return current UTC time as ISO-8601 string with second-precision.

    Returns:
        ISO-8601 UTC timestamp without microseconds, e.g. "2026-05-16T12:34:56+00:00".
    """
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()
