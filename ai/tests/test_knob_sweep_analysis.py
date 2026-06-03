# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for ``ai/scripts/analyze_knob_sweep.py`` (ADR-0305 / Research-0077).

Synthetic 20-row JSONL fixture; the live 12,636-cell sweep at
``runs/phase_a/full_grid/comprehensive.jsonl`` is not required.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "ai" / "scripts" / "analyze_knob_sweep.py"


def _load_module():
    """Import ``analyze_knob_sweep`` from its scripts/ path."""
    spec = importlib.util.spec_from_file_location("analyze_knob_sweep", SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["analyze_knob_sweep"] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def aks():
    return _load_module()


# ---------------------------------------------------------------------------
# Synthetic fixture
# ---------------------------------------------------------------------------


def _synthetic_rows() -> list[dict]:
    """20-row synthetic JSONL fixture covering every code path.

    Layout:

    - 2 sources × 2 codecs × 2 rc_modes = 8 stratification slices,
      each with 2–3 rows including one bare default.
    - One slice (source_a / libx264 / cq) carries an obvious
      regression: a "tuned" recipe that delivers lower VMAF than
      the bare default at matched bitrate. Used by
      ``test_recipe_regression_detection``.
    - One slice (source_a / libx265 / vbr) has two rows that tie on
      vmaf at the same bitrate; one has lower encode_time_ms. The
      tiebreaker should pick the cheaper one.
    """
    rows: list[dict] = []

    # source_a / libx264 / cq — three rows, includes a regression
    rows.append(
        _row(
            "source_a",
            "libx264",
            "cq",
            "p4",
            "30",
            "knob=bare",
            bitrate=2000,
            vmaf=92.0,
            enc_ms=4000,
            bare=True,
        )
    )
    rows.append(
        _row(
            "source_a",
            "libx264",
            "cq",
            "p4",
            "30",
            "knob=hq_recipe",
            bitrate=2010,
            vmaf=88.0,
            enc_ms=5500,
            bare=False,
        )
    )
    rows.append(
        _row(
            "source_a",
            "libx264",
            "cq",
            "p4",
            "26",
            "knob=better",
            bitrate=3500,
            vmaf=95.5,
            enc_ms=4500,
            bare=False,
        )
    )

    # source_a / libx264 / vbr — bare + one improvement
    rows.append(
        _row(
            "source_a",
            "libx264",
            "vbr",
            "p4",
            "vbr2M",
            "knob=bare",
            bitrate=2000,
            vmaf=89.0,
            enc_ms=3800,
            bare=True,
        )
    )
    rows.append(
        _row(
            "source_a",
            "libx264",
            "vbr",
            "p4",
            "vbr2M",
            "knob=hq_recipe",
            bitrate=2050,
            vmaf=92.5,
            enc_ms=5200,
            bare=False,
        )
    )

    # source_a / libx265 / cq
    rows.append(
        _row(
            "source_a",
            "libx265",
            "cq",
            "medium",
            "28",
            "knob=bare",
            bitrate=1500,
            vmaf=93.0,
            enc_ms=8000,
            bare=True,
        )
    )
    rows.append(
        _row(
            "source_a",
            "libx265",
            "cq",
            "slow",
            "28",
            "knob=better",
            bitrate=1450,
            vmaf=94.2,
            enc_ms=12000,
            bare=False,
        )
    )

    # source_a / libx265 / vbr — tiebreaker case (two rows tie on vmaf
    # at the same bitrate; lower enc_ms wins)
    rows.append(
        _row(
            "source_a",
            "libx265",
            "vbr",
            "medium",
            "vbr1.5M",
            "knob=bare",
            bitrate=1500,
            vmaf=91.0,
            enc_ms=8000,
            bare=True,
        )
    )
    rows.append(
        _row(
            "source_a",
            "libx265",
            "vbr",
            "medium",
            "vbr1.5M",
            "knob=tieA",
            bitrate=1500,
            vmaf=91.0,
            enc_ms=7500,
            bare=False,
        )
    )
    rows.append(
        _row(
            "source_a",
            "libx265",
            "vbr",
            "medium",
            "vbr1.5M",
            "knob=tieB",
            bitrate=1500,
            vmaf=91.0,
            enc_ms=9000,
            bare=False,
        )
    )

    # source_b / libx264 / cq
    rows.append(
        _row(
            "source_b",
            "libx264",
            "cq",
            "p4",
            "30",
            "knob=bare",
            bitrate=4000,
            vmaf=88.0,
            enc_ms=3500,
            bare=True,
        )
    )
    rows.append(
        _row(
            "source_b",
            "libx264",
            "cq",
            "p4",
            "26",
            "knob=better",
            bitrate=6000,
            vmaf=93.0,
            enc_ms=4200,
            bare=False,
        )
    )

    # source_b / libx264 / vbr
    rows.append(
        _row(
            "source_b",
            "libx264",
            "vbr",
            "p4",
            "vbr3M",
            "knob=bare",
            bitrate=3000,
            vmaf=85.0,
            enc_ms=3300,
            bare=True,
        )
    )
    rows.append(
        _row(
            "source_b",
            "libx264",
            "vbr",
            "p4",
            "vbr3M",
            "knob=hq_recipe",
            bitrate=3050,
            vmaf=89.0,
            enc_ms=4800,
            bare=False,
        )
    )

    # source_b / libx265 / cq
    rows.append(
        _row(
            "source_b",
            "libx265",
            "cq",
            "medium",
            "28",
            "knob=bare",
            bitrate=2500,
            vmaf=90.5,
            enc_ms=8500,
            bare=True,
        )
    )
    rows.append(
        _row(
            "source_b",
            "libx265",
            "cq",
            "slow",
            "28",
            "knob=better",
            bitrate=2400,
            vmaf=91.8,
            enc_ms=11000,
            bare=False,
        )
    )
    rows.append(
        _row(
            "source_b",
            "libx265",
            "cq",
            "veryslow",
            "26",
            "knob=best",
            bitrate=3500,
            vmaf=94.0,
            enc_ms=18000,
            bare=False,
        )
    )

    # source_b / libx265 / vbr
    rows.append(
        _row(
            "source_b",
            "libx265",
            "vbr",
            "medium",
            "vbr2.5M",
            "knob=bare",
            bitrate=2500,
            vmaf=89.0,
            enc_ms=8200,
            bare=True,
        )
    )
    rows.append(
        _row(
            "source_b",
            "libx265",
            "vbr",
            "medium",
            "vbr2.5M",
            "knob=hq_recipe",
            bitrate=2550,
            vmaf=91.5,
            enc_ms=10500,
            bare=False,
        )
    )
    rows.append(
        _row(
            "source_b",
            "libx265",
            "vbr",
            "slow",
            "vbr2.5M",
            "knob=hq_recipe",
            bitrate=2530,
            vmaf=92.2,
            enc_ms=14000,
            bare=False,
        )
    )

    return rows


def _row(source, codec, rc_mode, preset, quality, knob_combo, *, bitrate, vmaf, enc_ms, bare):
    return {
        "source": source,
        "codec": codec,
        "rc_mode": rc_mode,
        "preset": preset,
        "quality": quality,
        "knob_combo": knob_combo,
        "bitrate_kbps": bitrate,
        "vmaf_score": vmaf,
        "encode_time_ms": enc_ms,
        "is_bare_default": bare,
    }


@pytest.fixture
def synthetic_jsonl(tmp_path: Path) -> Path:
    path = tmp_path / "synth.jsonl"
    with path.open("w", encoding="utf-8") as fh:
        for row in _synthetic_rows():
            fh.write(json.dumps(row) + "\n")
    return path


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_pareto_frontier_smoke(aks, synthetic_jsonl, tmp_path):
    """End-to-end smoke: load → stratify → hull → CSV → summary."""
    rows = aks.load_jsonl(synthetic_jsonl)
    assert len(rows) == 20

    out_dir = tmp_path / "reports"
    report = aks.analyze(synthetic_jsonl, out_dir)

    assert report["n_rows"] == 20
    # 8 stratification slices in the synthetic fixture
    assert report["n_slices"] == 8
    # Every slice produces a CSV file
    assert len(report["csv_paths"]) == 8
    for csv_path in report["csv_paths"]:
        assert csv_path.exists()
        assert csv_path.read_text().startswith("source,codec,rc_mode,")

    # Tiebreaker case: source_a / libx265 / vbr at bitrate 1500 has
    # three rows with vmaf 91.0; the one with the lowest encode_time_ms
    # (knob=tieA, 7500 ms) is the one that should land on the hull.
    grouped = aks.stratify(rows)
    tie_slice = grouped[("source_a", "libx265", "vbr")]
    hull = aks.pareto_frontier(tie_slice)
    chosen = [r for r in hull if abs(r.bitrate_kbps - 1500) < 1.0]
    assert chosen, "tiebreaker hull entry missing"
    assert (
        chosen[0].knob_combo == "knob=tieA"
    ), f"expected tieA (lowest enc_ms) but got {chosen[0].knob_combo!r}"

    # Summary file is present and references the regression we expect.
    summary = report["summary_path"].read_text()
    assert "Per-slice hull sizes" in summary
    assert "regressions" in summary.lower()


def test_stratification_keys(aks, synthetic_jsonl):
    """Stratification groups by exactly (source, codec, rc_mode)."""
    rows = aks.load_jsonl(synthetic_jsonl)
    grouped = aks.stratify(rows)

    # Order is load-bearing: STRATIFICATION_KEYS pins it.
    assert aks.STRATIFICATION_KEYS == ("source", "codec", "rc_mode")

    # Every group key is a 3-tuple.
    for key in grouped:
        assert isinstance(key, tuple) and len(key) == 3

    # The synthetic fixture covers 2 sources × 2 codecs × 2 rc_modes.
    assert len(grouped) == 8
    assert {k[0] for k in grouped} == {"source_a", "source_b"}
    assert {k[1] for k in grouped} == {"libx264", "libx265"}
    assert {k[2] for k in grouped} == {"cq", "vbr"}

    # Rows preserve their slice membership.
    for key, slice_rows in grouped.items():
        for row in slice_rows:
            assert (row.source, row.codec, row.rc_mode) == key


def test_recipe_regression_detection(aks, synthetic_jsonl):
    """The hq_recipe at source_a/libx264/cq must be flagged as regressing."""
    rows = aks.load_jsonl(synthetic_jsonl)
    regressions = aks.detect_recipe_regressions(rows)

    # The synthetic fixture seeds exactly one regression: source_a /
    # libx264 / cq, knob=hq_recipe (88.0 vs bare 92.0 at bitrate ~2000).
    matching = [
        r
        for r in regressions
        if r["source"] == "source_a"
        and r["codec"] == "libx264"
        and r["rc_mode"] == "cq"
        and r["candidate_knob_combo"] == "knob=hq_recipe"
    ]
    assert len(matching) == 1, f"expected one regression, got {len(matching)}: {regressions!r}"
    reg = matching[0]
    assert reg["vmaf_delta"] < -3.0, f"regression delta should be ~-4.0, got {reg['vmaf_delta']}"
    assert reg["bare_vmaf"] == pytest.approx(92.0)
    assert reg["candidate_vmaf"] == pytest.approx(88.0)

    # No false positives on the lift cases (e.g. source_a/libx264/vbr,
    # source_b/libx265/vbr — those recipes lift VMAF over the bare).
    for r in regressions:
        assert not (
            r["source"] == "source_a" and r["codec"] == "libx264" and r["rc_mode"] == "vbr"
        ), f"false positive on lift case: {r!r}"
