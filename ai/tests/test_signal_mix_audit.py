# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the signal-mix audit CLI.

The audit is intentionally table-only: synthetic JSONL fixtures are enough to
exercise family coverage, missing-signal detection, redundancy, and candidate
intersections without building libvmaf or touching corpus data.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

# pylint: disable=wrong-import-position
from signal_mix_audit import _is_numeric_series, audit_table, main, render_markdown


def _write_jsonl(path: Path, rows: list[dict]) -> Path:
    path.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
    return path


def _fixture_rows() -> list[dict]:
    rows: list[dict] = []
    for idx in range(24):
        target = 50.0 + idx
        alternating = -1.0 if idx % 2 else 1.0
        rows.append(
            {
                "src": f"clip-{idx:02d}.mp4",
                "vmaf": target,
                "adm2": 0.50 + 0.01 * idx,
                "vif_scale0": 0.50 + 0.01 * idx + 0.00001 * alternating,
                "motion2": float(idx % 7),
                "float_ms_ssim": 0.70 + 0.005 * idx,
                "ssimulacra2": 20.0 + 0.75 * idx,
                "cambi": 1.0 + 0.05 * idx,
                "ciede2000": 3.0 + 0.04 * (23 - idx),
                "bitdepth": 10,
                "chug_bitrate_mbps": 2.0 + 0.20 * idx + 0.75 * alternating,
                "mos": 1.0 + 4.0 * idx / 23.0,
            }
        )
    return rows


def test_signal_family_coverage_and_missing_rows(tmp_path):
    table = _write_jsonl(tmp_path / "features.jsonl", _fixture_rows())
    audit = audit_table("synthetic", table)

    assert audit.target == "vmaf"
    assert "mos" not in audit.signal_columns
    assert audit.family_health["canonical_fr"]["status"] == "covered"
    assert audit.family_health["texture_deep_perceptual"]["status"] == "covered"
    assert audit.family_health["banding_tone_mapping"]["status"] == "covered"
    assert audit.family_health["saliency_roi"]["status"] == "missing"
    assert audit.family_health["noise_grain_blur"]["status"] == "missing"
    assert audit.family_health["hdr_display_panel"]["status"] == "weak"


def test_redundancy_and_complementary_intersections(tmp_path):
    table = _write_jsonl(tmp_path / "features.jsonl", _fixture_rows())
    audit = audit_table("synthetic", table, complement_threshold=0.995)

    redundant_names = {frozenset((row["left"], row["right"])) for row in audit.redundant_pairs}
    assert frozenset(("adm2", "vif_scale0")) in redundant_names

    intersection_names = {
        frozenset((row["left_family"], row["right_family"])) for row in audit.intersections
    }
    assert frozenset(("canonical_fr", "codec_encoder")) in intersection_names


def test_markdown_reports_blind_spots_and_candidate_metrics(tmp_path):
    table = _write_jsonl(tmp_path / "features.jsonl", _fixture_rows())
    audit = audit_table("synthetic", table)
    report = render_markdown([audit], top_k=4)

    assert "Signal-mix audit" in report
    assert "Saliency and ROI weighting" in report
    assert "U2NetP" in report
    assert "Complementary Intersections" in report
    assert "`adm2`" in report


def test_signal_mix_main_writes_json_and_markdown(tmp_path, monkeypatch):
    table = _write_jsonl(tmp_path / "features.jsonl", _fixture_rows())
    out_json = tmp_path / "audit.json"
    out_md = tmp_path / "audit.md"

    monkeypatch.setattr(
        sys,
        "argv",
        [
            "signal_mix_audit.py",
            "--input",
            f"syn={table}",
            "--out-json",
            str(out_json),
            "--out-md",
            str(out_md),
        ],
    )
    assert main() == 0
    payload = json.loads(out_json.read_text())
    assert payload["tables"][0]["label"] == "syn"
    assert payload["tables"][0]["missing_families"]
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/signal_mix_audit.py"
    assert provenance["inputs"]["tables"][0]["kind"] == "file"
    assert provenance["outputs"]["json_report"] == str(out_json)
    assert "Recommended Next Actions" in out_md.read_text()


def test_custom_target_is_honoured(tmp_path):
    rows = _fixture_rows()
    for row in rows:
        row["mos_target"] = row.pop("mos")
        row.pop("vmaf")
    table = _write_jsonl(tmp_path / "mos_features.jsonl", rows)

    audit = audit_table("mos", table, target="mos_target")
    assert audit.target == "mos_target"
    assert audit.target_correlations


def test_second_opinion_columns_count_as_nr_mos_signal(tmp_path):
    rows = _fixture_rows()
    for idx, row in enumerate(rows):
        row["second_opinion_dover_mobile_score"] = 60.0 + idx
        row["second_opinion_q_align_score"] = 62.0 + 0.5 * idx
    table = _write_jsonl(tmp_path / "second_opinion.jsonl", rows)

    audit = audit_table("second-opinion", table)

    health = audit.family_health["no_reference_ugc"]
    assert health["status"] == "covered"
    assert "second_opinion_dover_mobile_score" in health["matched_columns"]
    assert "second_opinion_q_align_score" in health["matched_columns"]


def test_invalid_target_leaves_report_targetless(tmp_path):
    table = _write_jsonl(tmp_path / "features.jsonl", _fixture_rows())
    audit = audit_table("synthetic", table, target="not_present")

    assert audit.target is None
    assert audit.target_correlations == []
    assert "No target column found" in render_markdown([audit])


def test_pandas_string_dtype_is_not_numeric():
    pd = pytest.importorskip("pandas")

    assert not _is_numeric_series(pd.Series(["clip-a", "clip-b"], dtype="string"))
    assert _is_numeric_series(pd.Series([1.0, 2.0], dtype="float64"))
