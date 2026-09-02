#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Per-GPU-generation ULP calibration loader (ADR-0234).

Companion to ``cross_backend_vif_diff.py`` and
``cross_backend_parity_gate.py``. Loads the YAML calibration table
in ``scripts/ci/gpu_ulp_calibration.yaml`` and resolves a
``(feature, gpu_id)`` lookup to an absolute tolerance.

Why this lives in its own module:

* The two gate scripts share the lookup logic verbatim; one source
  of truth avoids drift.
* The lookup is unit-tested (``scripts/ci/test_calibration.py``)
  without spinning up the gates' subprocess scaffolding.
* The module is import-safe even on hosts without ``pyyaml``
  installed — falling back to the gate's per-feature default and
  emitting a one-line stderr advisory rather than crashing CI.

The data shape is documented in ``gpu_ulp_calibration.yaml`` itself.
This module is the loader, not the schema definition.
"""

from __future__ import annotations

import dataclasses
import sys
from collections.abc import Mapping
from pathlib import Path
from typing import Any

# pyyaml is universally available on the project's CI images and
# locally (see ``ai/src/vmaf_train/data/datasets.py`` etc.). We import
# it lazily so a misconfigured host produces a clear advisory rather
# than an opaque ImportError on script startup. The gate scripts pass
# a ``None`` table when calibration cannot load — the lookup falls
# back to the existing per-feature default in that case.
try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover - exercised only when pyyaml absent
    yaml = None  # type: ignore[assignment]


DEFAULT_CALIBRATION_PATH = Path(__file__).parent / "gpu_ulp_calibration.yaml"


@dataclasses.dataclass(frozen=True)
class CalibrationEntry:
    """One ``gpus:`` row from the calibration YAML."""

    gpu_id_pattern: str
    label: str
    status: str  # "calibrated" | "placeholder"
    features: Mapping[str, float]
    notes: str = ""

    def matches(self, gpu_id: str) -> bool:
        """Glob-match ``gpu_id`` against this entry's pattern.

        Only trailing ``*`` is supported — the patterns in the YAML
        are simple prefix-match expressions ("vulkan:0x1002:0x73*").
        ``*`` anywhere else is treated literally; we deliberately
        avoid ``fnmatch.fnmatch`` here to keep the matching surface
        small and the precedence rules trivial.
        """

        if self.gpu_id_pattern.endswith("*"):
            prefix = self.gpu_id_pattern[:-1]
            return gpu_id.startswith(prefix)
        return self.gpu_id_pattern == gpu_id

    def specificity(self) -> int:
        """Length of the non-wildcard prefix. Higher = more specific."""

        if self.gpu_id_pattern.endswith("*"):
            return len(self.gpu_id_pattern) - 1
        return len(self.gpu_id_pattern) + 1  # exact match beats any glob


@dataclasses.dataclass
class CalibrationTable:
    """Loaded calibration table (the YAML file's deserialised form)."""

    version: int
    default_fp32_tolerance: float
    default_fp16_tolerance: float
    entries: list[CalibrationEntry]

    def lookup(self, gpu_id: str) -> CalibrationEntry | None:
        """Most-specific match for ``gpu_id``, or ``None``.

        Specificity = length of the non-wildcard prefix. Ties are
        broken by file order (first match wins) — predictable and
        easy to audit when two patterns shadow each other.
        """

        best: CalibrationEntry | None = None
        for entry in self.entries:
            if not entry.matches(gpu_id):
                continue
            if best is None or entry.specificity() > best.specificity():
                best = entry
        return best

    def tolerance_for(
        self,
        feature: str,
        gpu_id: str | None,
        feature_default: float,
    ) -> float:
        """Resolve the tolerance for ``(feature, gpu_id)``.

        Resolution order:

        1. If ``gpu_id`` is None or no entry matches, return the
           caller-supplied per-feature default (existing
           ``FEATURE_TOLERANCE.get(feature, DEFAULT_FP32_TOLERANCE)``
           contract preserved).
        2. If the matched entry has a ``features:`` override for
           ``feature``, return that.
        3. Otherwise return the caller-supplied per-feature default
           (the matched arch is registered but lacks calibration
           data for this feature — typical of placeholder entries).
        """

        if gpu_id is None:
            return feature_default
        entry = self.lookup(gpu_id)
        if entry is None:
            return feature_default
        return float(entry.features.get(feature, feature_default))


def _coerce_features(raw: Any) -> Mapping[str, float]:
    """Normalise the YAML ``features:`` block into ``{name: float}``.

    ``raw`` is whatever ``yaml.safe_load`` returned for that field —
    expected to be a mapping ``{feature_name: tolerance}`` or
    ``None`` / missing for placeholder entries.
    """

    if raw is None:
        return {}
    if not isinstance(raw, Mapping):
        msg = f"calibration entry 'features' must be a mapping, got {type(raw).__name__}"
        raise ValueError(msg)
    out: dict[str, float] = {}
    for name, value in raw.items():
        if not isinstance(name, str):
            msg = f"calibration feature name must be str, got {type(name).__name__}"
            raise ValueError(msg)
        out[name] = float(value)
    return out


def parse_table(payload: Mapping[str, Any]) -> CalibrationTable:
    """Build a ``CalibrationTable`` from an already-deserialised mapping.

    Split out from ``load_calibration_table`` so the unit test can
    exercise the parser without touching the filesystem.
    """

    if not isinstance(payload, Mapping):
        msg = f"calibration table root must be a mapping, got {type(payload).__name__}"
        raise ValueError(msg)
    version = int(payload.get("version", 1))
    default_fp32 = float(payload.get("default_fp32_tolerance", 5.0e-5))
    default_fp16 = float(payload.get("default_fp16_tolerance", 1.0e-2))
    raw_gpus = payload.get("gpus", [])
    if not isinstance(raw_gpus, list):
        msg = f"calibration 'gpus' must be a list, got {type(raw_gpus).__name__}"
        raise ValueError(msg)
    entries: list[CalibrationEntry] = []
    for row in raw_gpus:
        if not isinstance(row, Mapping):
            msg = f"calibration gpu row must be a mapping, got {type(row).__name__}"
            raise ValueError(msg)
        pattern = row.get("id")
        if not isinstance(pattern, str) or not pattern:
            msg = "calibration gpu row missing required 'id' field"
            raise ValueError(msg)
        entries.append(
            CalibrationEntry(
                gpu_id_pattern=pattern,
                label=str(row.get("label", pattern)),
                status=str(row.get("status", "placeholder")),
                features=_coerce_features(row.get("features")),
                notes=str(row.get("notes", "")),
            )
        )
    return CalibrationTable(
        version=version,
        default_fp32_tolerance=default_fp32,
        default_fp16_tolerance=default_fp16,
        entries=entries,
    )


def load_calibration_table(path: Path | None = None) -> CalibrationTable | None:
    """Load and parse the YAML calibration table.

    Returns ``None`` (with a stderr advisory) when ``pyyaml`` is
    unavailable or the file is missing — the gate scripts treat
    ``None`` as "no calibration; use built-in defaults", preserving
    backward compatibility with hosts that haven't installed the
    optional dependency.

    Raises ``ValueError`` on a malformed table — a corrupt
    calibration file is a CI-visible bug, not a silent fallback.
    """

    if path is None:
        path = DEFAULT_CALIBRATION_PATH
    if yaml is None:
        sys.stderr.write(
            "calibration: pyyaml not installed; falling back to per-feature defaults\n"
        )
        return None
    if not path.exists():
        sys.stderr.write(
            f"calibration: table not found at {path}; falling back to per-feature defaults\n"
        )
        return None
    with path.open() as fh:
        payload = yaml.safe_load(fh)
    return parse_table(payload)
