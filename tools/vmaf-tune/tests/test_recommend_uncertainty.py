# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the uncertainty-aware recommend predicate (ADR-0279).

Exercises :func:`vmaftune.recommend.pick_target_vmaf_with_uncertainty`
across the three confidence bands documented in
:mod:`vmaftune.uncertainty`:

* Tight interval -> early short-circuit on the first row whose
  conformal lower bound clears the target.
* Wide interval -> forced full scan with the result tagged
  ``(UNCERTAIN)``.
* Middle band / no calibration -> defer to the native point-estimate
  predicate.
* Interval-excluded corpus -> UNMET branch surfaces the highest-VMAF
  best-effort row.
* Per-call ``sample_uncertainty`` override is honoured ahead of
  embedded ``vmaf_interval`` blocks.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.cli import main as cli_main  # noqa: E402
from vmaftune.recommend import (  # noqa: E402
    UncertaintyAwareRequest,
    pick_target_vmaf_with_uncertainty,
)
from vmaftune.uncertainty import ConfidenceDecision, ConfidenceThresholds  # noqa: E402


def _row(
    *,
    crf: int,
    vmaf: float,
    low: float | None = None,
    high: float | None = None,
    encoder: str = "libx264",
    preset: str = "medium",
    bitrate: float = 2000.0,
    exit_status: int = 0,
) -> dict:
    row: dict = {
        "encoder": encoder,
        "preset": preset,
        "crf": crf,
        "vmaf_score": vmaf,
        "bitrate_kbps": bitrate,
        "exit_status": exit_status,
    }
    if low is not None and high is not None:
        row["vmaf_interval"] = {"low": low, "high": high, "alpha": 0.05}
    return row


def test_tight_interval_short_circuits_search():
    """Tight interval whose ``low >= target`` triggers immediate promotion."""
    rows = [
        _row(crf=18, vmaf=96.0, low=95.5, high=96.5),  # tight, clears
        _row(crf=23, vmaf=92.0, low=91.5, high=92.5),
        _row(crf=28, vmaf=88.0, low=87.5, high=88.5),
    ]
    req = UncertaintyAwareRequest(target_vmaf=95.0)
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert result.decision is ConfidenceDecision.TIGHT
    assert result.row["crf"] == 18
    assert result.visited == 1, "tight interval should short-circuit on first hit"
    assert "TIGHT" in result.predicate


def test_wide_interval_forces_full_scan_and_flags_uncertain():
    """A WIDE-band row never short-circuits; final result is tagged UNCERTAIN."""
    # All rows have a 6.0-VMAF wide interval (>= default 5.0).
    rows = [
        _row(crf=18, vmaf=96.0, low=93.0, high=99.0),
        _row(crf=23, vmaf=92.0, low=89.0, high=95.0),
        _row(crf=28, vmaf=88.0, low=85.0, high=91.0),
    ]
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert result.decision is ConfidenceDecision.WIDE
    assert result.visited == len(rows), "wide interval must force the full scan"
    assert "UNCERTAIN" in result.predicate
    # The point-estimate fallback picks the smallest CRF clearing 90,
    # which is crf=18 (vmaf=96).
    assert result.row["crf"] == 18


def test_middle_band_defers_to_point_estimate_recipe():
    """Mid-band intervals reproduce :func:`pick_target_vmaf` exactly."""
    rows = [
        _row(crf=18, vmaf=96.0, low=94.0, high=97.5),  # width=3.5 -> middle
        _row(crf=23, vmaf=92.0, low=90.0, high=93.5),
        _row(crf=28, vmaf=88.0, low=86.0, high=89.5),
    ]
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert result.decision is ConfidenceDecision.MIDDLE
    # Point-estimate predicate picks smallest CRF clearing 90 = crf=18.
    assert result.row["crf"] == 18
    assert "UNCERTAIN" not in result.predicate


def test_no_interval_payload_falls_back_to_point_recipe():
    """Rows without ``vmaf_interval`` degrade to MIDDLE / point semantics."""
    rows = [
        _row(crf=18, vmaf=96.0),
        _row(crf=23, vmaf=92.0),
        _row(crf=28, vmaf=88.0),
    ]
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    result = pick_target_vmaf_with_uncertainty(rows, req)
    # Zero-width interval at the point => MIDDLE band; point recipe wins.
    assert result.decision is ConfidenceDecision.MIDDLE
    assert result.row["crf"] == 18


def test_interval_excluded_corpus_returns_unmet_with_best_effort():
    """When every interval lies below target, surface the closest miss."""
    rows = [
        _row(crf=33, vmaf=80.0, low=78.0, high=82.0),
        _row(crf=38, vmaf=75.0, low=73.0, high=77.0),
    ]
    req = UncertaintyAwareRequest(target_vmaf=95.0)
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert "UNMET" in result.predicate
    assert "interval-excluded" in result.predicate
    # Best-effort row is the highest VMAF.
    assert result.row["crf"] == 33


def test_sample_uncertainty_override_takes_precedence():
    """``sample_uncertainty`` overrides any embedded ``vmaf_interval``."""
    rows = [
        # Embedded interval would say "wide"; override declares "tight".
        _row(crf=20, vmaf=94.0, low=90.0, high=98.0),
        _row(crf=25, vmaf=90.0, low=87.0, high=93.0),
    ]
    overrides = {
        20: (94.0, 93.5, 94.5),  # tight, clears 93
        25: (90.0, 89.5, 90.5),  # tight
    }
    req = UncertaintyAwareRequest(
        target_vmaf=93.0,
        sample_uncertainty=overrides,
    )
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert result.decision is ConfidenceDecision.TIGHT
    assert result.row["crf"] == 20
    assert result.visited == 1


def test_custom_thresholds_change_band_boundaries():
    """Sidecar-style threshold overrides re-classify the same widths."""
    rows = [
        _row(crf=18, vmaf=96.0, low=94.0, high=98.0),  # width=4
    ]
    # Default thresholds (2/5) => MIDDLE for width=4.
    default_req = UncertaintyAwareRequest(target_vmaf=93.0)
    default_result = pick_target_vmaf_with_uncertainty(rows, default_req)
    assert default_result.decision is ConfidenceDecision.MIDDLE
    # Tight thresholds (5/6) => TIGHT for width=4.
    tight_req = UncertaintyAwareRequest(
        target_vmaf=93.0,
        thresholds=ConfidenceThresholds(
            tight_interval_max_width=5.0,
            wide_interval_min_width=6.0,
            source="test",
        ),
    )
    tight_result = pick_target_vmaf_with_uncertainty(rows, tight_req)
    assert tight_result.decision is ConfidenceDecision.TIGHT


def test_filters_skip_failed_rows():
    """``exit_status != 0`` and missing VMAF values are dropped."""
    rows = [
        _row(crf=18, vmaf=99.0, exit_status=1),  # failed encode
        _row(crf=20, vmaf=float("nan")),
        _row(crf=23, vmaf=92.0, low=91.5, high=92.5),
    ]
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert result.row["crf"] == 23


def test_empty_eligible_set_raises():
    """An empty filtered set raises like the point-estimate recipe."""
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    with pytest.raises(ValueError, match="no eligible rows"):
        pick_target_vmaf_with_uncertainty([], req)


# ---------------------------------------------------------------------------
# CLI-level tests: --from-corpus + --with-uncertainty (ADR-0279 bug fix)
# ---------------------------------------------------------------------------


def _write_corpus(path: Path, rows: list[dict]) -> Path:
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row) + "\n")
    return path


def _corpus_row(
    *,
    crf: int,
    vmaf: float,
    low: float | None = None,
    high: float | None = None,
    encoder: str = "libx264",
    preset: str = "medium",
    bitrate: float = 2000.0,
    exit_status: int = 0,
) -> dict:
    row: dict = {
        "encoder": encoder,
        "preset": preset,
        "crf": crf,
        "vmaf_score": vmaf,
        "bitrate_kbps": bitrate,
        "exit_status": exit_status,
    }
    if low is not None and high is not None:
        row["vmaf_interval"] = {"low": low, "high": high, "alpha": 0.05}
    return row


def test_from_corpus_with_uncertainty_uses_interval_aware_predictor(tmp_path: Path, capsys):
    """--with-uncertainty on --from-corpus must route through the
    interval-aware predictor and produce output that differs from the
    bare point-estimate path when tight interval fires early.

    The corpus has three rows (crf 18, 23, 28).  Without
    ``--with-uncertainty`` the point-estimate path examines all rows
    and picks crf=18 (smallest CRF clearing the 90-VMAF bar).  With
    ``--with-uncertainty`` the tight interval on crf=18 fires on the
    first row examined and the output must include the ``decision=``
    and ``visited=`` fields that signal the uncertainty-aware code path
    ran.
    """
    rows = [
        _corpus_row(crf=18, vmaf=96.0, low=95.5, high=96.5),  # tight, clears 90
        _corpus_row(crf=23, vmaf=92.0, low=91.5, high=92.5),
        _corpus_row(crf=28, vmaf=88.0, low=87.5, high=88.5),
    ]
    corpus = _write_corpus(tmp_path / "corpus.jsonl", rows)

    # Bare (no --with-uncertainty): plain point-estimate output — no decision field.
    rc_plain = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "90",
        ]
    )
    out_plain = capsys.readouterr().out
    assert rc_plain == 0
    assert "decision=" not in out_plain, "plain path must not emit decision= field"

    # With --with-uncertainty: interval-aware output must include decision= and visited=.
    rc_ua = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "90",
            "--with-uncertainty",
        ]
    )
    out_ua = capsys.readouterr().out
    assert rc_ua == 0
    assert (
        "decision=" in out_ua
    ), "--with-uncertainty must emit decision= field (interval-aware path was skipped)"
    assert "visited=" in out_ua, "--with-uncertainty must emit visited= field"
    # The tight-interval short-circuit must pick crf=18 on the first row.
    assert "crf=18" in out_ua
    assert "TIGHT" in out_ua


def test_from_corpus_with_uncertainty_json_output(tmp_path: Path, capsys):
    """--with-uncertainty + --json on --from-corpus emits the winning row as JSON."""
    rows = [
        _corpus_row(crf=20, vmaf=95.0, low=94.5, high=95.5),  # tight, clears 90
    ]
    corpus = _write_corpus(tmp_path / "corpus.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "90",
            "--with-uncertainty",
            "--json",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out.strip())
    assert payload["crf"] == 20


def test_from_corpus_with_uncertainty_and_target_bitrate_falls_back(tmp_path: Path, capsys):
    """--with-uncertainty + --target-bitrate warns and falls back to point-estimate.

    The interval-aware predictor only implements ``target_vmaf`` logic;
    combining it with ``--target-bitrate`` must produce a warning on
    stderr and still return a valid row via the point-estimate path.
    """
    rows = [
        _corpus_row(crf=22, vmaf=95.0, low=94.0, high=96.0, bitrate=5000.0),
        _corpus_row(crf=26, vmaf=92.0, low=91.0, high=93.0, bitrate=3000.0),
    ]
    corpus = _write_corpus(tmp_path / "corpus.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-bitrate",
            "4000",
            "--with-uncertainty",
        ]
    )
    assert rc == 0
    err = capsys.readouterr().err
    assert "falling back to point-estimate" in err
