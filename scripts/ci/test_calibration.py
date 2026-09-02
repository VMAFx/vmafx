#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the per-GPU-gen ULP calibration loader (ADR-0234).

Run with::

    python3 -m pytest scripts/ci/test_calibration.py -v

The tests exercise ``parse_table`` and ``CalibrationTable.lookup`` /
``tolerance_for`` directly against in-memory dicts — the on-disk YAML
is loaded by a single end-to-end test that imports the shipped table
to confirm it parses cleanly.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from cross_backend_calibration import (
    DEFAULT_CALIBRATION_PATH,
    CalibrationEntry,
    CalibrationTable,
    load_calibration_table,
    parse_table,
)

# ---------------------------------------------------------------------------
# parse_table — schema validation
# ---------------------------------------------------------------------------


def test_parse_table_minimal() -> None:
    """An empty ``gpus`` list parses to a no-op table."""

    table = parse_table({"version": 1, "gpus": []})
    assert table.version == 1
    assert table.entries == []
    assert table.default_fp32_tolerance == pytest.approx(5.0e-5)


def test_parse_table_full_row() -> None:
    payload = {
        "version": 1,
        "default_fp32_tolerance": 5.0e-5,
        "default_fp16_tolerance": 1.0e-2,
        "gpus": [
            {
                "id": "vulkan:0x10005:*",
                "label": "Mesa lavapipe",
                "status": "calibrated",
                "notes": "hosted CI baseline",
                "features": {
                    "vif": 5.0e-5,
                    "ciede": 5.0e-3,
                },
            }
        ],
    }
    table = parse_table(payload)
    assert len(table.entries) == 1
    entry = table.entries[0]
    assert entry.gpu_id_pattern == "vulkan:0x10005:*"
    assert entry.status == "calibrated"
    assert entry.features == {"vif": 5.0e-5, "ciede": 5.0e-3}


def test_parse_table_missing_id_rejected() -> None:
    with pytest.raises(ValueError, match="missing required 'id'"):
        parse_table({"version": 1, "gpus": [{"label": "no id"}]})


def test_parse_table_features_must_be_mapping() -> None:
    with pytest.raises(ValueError, match="features"):
        parse_table(
            {
                "version": 1,
                "gpus": [{"id": "x:y", "features": ["not", "a", "mapping"]}],
            }
        )


def test_parse_table_root_must_be_mapping() -> None:
    with pytest.raises(ValueError, match="root"):
        parse_table(["not", "a", "mapping"])  # type: ignore[arg-type]


def test_parse_table_gpus_must_be_list() -> None:
    with pytest.raises(ValueError, match="gpus"):
        parse_table({"version": 1, "gpus": {"not": "a list"}})


# ---------------------------------------------------------------------------
# CalibrationEntry.matches — glob semantics
# ---------------------------------------------------------------------------


def test_entry_matches_exact() -> None:
    entry = CalibrationEntry(
        gpu_id_pattern="cuda:8.6", label="Ampere", status="placeholder", features={}
    )
    assert entry.matches("cuda:8.6")
    assert not entry.matches("cuda:8.0")


def test_entry_matches_trailing_glob() -> None:
    entry = CalibrationEntry(
        gpu_id_pattern="vulkan:0x1002:0x73*",
        label="RDNA2",
        status="placeholder",
        features={},
    )
    assert entry.matches("vulkan:0x1002:0x7300")
    assert entry.matches("vulkan:0x1002:0x73ff")
    assert not entry.matches("vulkan:0x1002:0x7400")
    assert not entry.matches("vulkan:0x1003:0x7300")


def test_entry_specificity_ranks_exact_above_glob() -> None:
    exact = CalibrationEntry(gpu_id_pattern="cuda:8.6", label="", status="", features={})
    glob = CalibrationEntry(gpu_id_pattern="cuda:*", label="", status="", features={})
    assert exact.specificity() > glob.specificity()


def test_entry_specificity_ranks_longer_glob_above_shorter() -> None:
    longer = CalibrationEntry(
        gpu_id_pattern="vulkan:0x1002:0x744*", label="", status="", features={}
    )
    shorter = CalibrationEntry(gpu_id_pattern="vulkan:0x1002:*", label="", status="", features={})
    assert longer.specificity() > shorter.specificity()


# ---------------------------------------------------------------------------
# CalibrationTable.lookup — most-specific match
# ---------------------------------------------------------------------------


def _table_with(*patterns: tuple[str, dict[str, float]]) -> CalibrationTable:
    """Build a table from ``(pattern, features)`` tuples for tests."""

    entries = [
        CalibrationEntry(gpu_id_pattern=p, label=p, status="calibrated", features=feats)
        for p, feats in patterns
    ]
    return CalibrationTable(
        version=1,
        default_fp32_tolerance=5.0e-5,
        default_fp16_tolerance=1.0e-2,
        entries=entries,
    )


def test_lookup_returns_none_when_no_match() -> None:
    table = _table_with(("cuda:8.6", {"vif": 1e-6}))
    assert table.lookup("vulkan:0x10005:0x0") is None


def test_lookup_picks_most_specific() -> None:
    table = _table_with(
        ("vulkan:0x1002:*", {"vif": 1e-3}),
        ("vulkan:0x1002:0x73*", {"vif": 1e-4}),
        ("vulkan:0x1002:0x7300", {"vif": 1e-5}),
    )
    entry = table.lookup("vulkan:0x1002:0x7300")
    assert entry is not None
    assert entry.gpu_id_pattern == "vulkan:0x1002:0x7300"


def test_lookup_falls_back_to_glob_when_no_exact() -> None:
    table = _table_with(
        ("vulkan:0x1002:*", {"vif": 1e-3}),
        ("vulkan:0x1002:0x73*", {"vif": 1e-4}),
    )
    entry = table.lookup("vulkan:0x1002:0x7300")
    assert entry is not None
    assert entry.gpu_id_pattern == "vulkan:0x1002:0x73*"


# ---------------------------------------------------------------------------
# CalibrationTable.tolerance_for — fallback chain
# ---------------------------------------------------------------------------


def test_tolerance_for_returns_default_when_gpu_id_none() -> None:
    table = _table_with(("vulkan:0x10005:*", {"vif": 1e-6}))
    assert table.tolerance_for("vif", None, 5e-5) == pytest.approx(5e-5)


def test_tolerance_for_returns_override_when_matched() -> None:
    table = _table_with(("vulkan:0x10005:*", {"vif": 1e-6}))
    assert table.tolerance_for("vif", "vulkan:0x10005:0x0", 5e-5) == pytest.approx(1e-6)


def test_tolerance_for_returns_default_when_feature_absent() -> None:
    """Matched arch row but feature not in its ``features:`` block."""

    table = _table_with(("vulkan:0x10005:*", {"vif": 1e-6}))
    # ``ciede`` is not in the entry's features → fall back to default.
    assert table.tolerance_for("ciede", "vulkan:0x10005:0x0", 5e-3) == pytest.approx(5e-3)


def test_tolerance_for_returns_default_when_no_match() -> None:
    table = _table_with(("cuda:8.6", {"vif": 1e-6}))
    assert table.tolerance_for("vif", "vulkan:0x10005:0x0", 5e-5) == pytest.approx(5e-5)


# ---------------------------------------------------------------------------
# Shipped table — round-trip parse smoke test
# ---------------------------------------------------------------------------


def test_shipped_calibration_table_loads() -> None:
    """The committed YAML parses without errors and has a lavapipe row.

    Skipped if pyyaml is unavailable on the test host (the gate
    scripts treat that case as fall-back-to-defaults; the unit test
    must mirror that contract).
    """

    pytest.importorskip("yaml")
    table = load_calibration_table(DEFAULT_CALIBRATION_PATH)
    assert table is not None
    assert table.version == 1
    # Lavapipe is the calibrated baseline — enforce it stays present
    # so the hosted-CI lane never silently loses its calibration.
    lavapipe = table.lookup("vulkan:0x10005:0x0")
    assert lavapipe is not None
    assert lavapipe.status == "calibrated"
    assert "vif" in lavapipe.features


def test_shipped_calibration_table_includes_placeholder_arches() -> None:
    """Sanity: known arches from the ADR's coverage list are present."""

    pytest.importorskip("yaml")
    table = load_calibration_table(DEFAULT_CALIBRATION_PATH)
    assert table is not None
    # Spot-check a representative subset; the full list is the YAML.
    assert table.lookup("cuda:8.6") is not None  # Ampere
    assert table.lookup("cuda:7.5") is not None  # Turing
    assert table.lookup("cuda:9.0") is not None  # Hopper
    assert table.lookup("vulkan:0x1002:0x7300") is not None  # RDNA2
