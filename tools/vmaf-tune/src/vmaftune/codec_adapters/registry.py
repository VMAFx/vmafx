# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Read the encoder registry (ADR-3388).

``registry.gen.json`` next to this module is generated from
``api/vmafx-encoders.toml`` by ``scripts/codegen/vmafx-api.py``; never edit it.
The Go ``pkg/encoderregistry`` embeds a byte-identical copy, so the Python
adapters, ``pkg/encoder`` and ``pkg/codecadapter`` read each codec's quality
knob, windows, presets and capability flags from one document.
``tests/test_encoder_registry.py`` holds every adapter in ``_REGISTRY`` equal
to its implemented row.
"""

from __future__ import annotations

import functools
import json
from pathlib import Path
from typing import Any

GENERATED = Path(__file__).with_name("registry.gen.json")


@functools.cache
def document() -> dict[str, Any]:
    """The parsed registry document."""
    with GENERATED.open(encoding="utf-8") as handle:
        return json.load(handle)


def row(encoder_id: str) -> dict[str, Any]:
    """The registry row for ``encoder_id``; ``KeyError`` names unknown ids."""
    for entry in document()["encoders"]:
        if entry["id"] == encoder_id:
            return entry
    raise KeyError(f"no encoder registry row {encoder_id!r}")


def implemented() -> tuple[dict[str, Any], ...]:
    """The rows that carry an adapter table, in registry order."""
    return tuple(e for e in document()["encoders"] if e["adapter"] is not None)


def absolute_range(encoder_id: str) -> tuple[int, int] | None:
    """The encoder's full accepted quality range, or ``None`` without a row."""
    try:
        spec = row(encoder_id)["adapter"]
    except KeyError:
        return None
    if spec is None:
        return None
    low, high = spec["absolute_range"]
    return int(low), int(high)
