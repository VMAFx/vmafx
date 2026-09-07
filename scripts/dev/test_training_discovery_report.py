#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for training_discovery_report.py.

Tests use in-memory fixtures only — no ONNX files, no corpus, no network.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import training_discovery_report as tdr

# ---------------------------------------------------------------------------
# Helpers — build well-formed fixture data in tmp_path
# ---------------------------------------------------------------------------

_CARD_TEMPLATE = """\
# `{codec}` — VMAF predictor model card

- **Codec adapter**: `{codec}`
- **Training date**: 2026-01-01
- **Corpus kind**: `{corpus_kind}`

## 3. Validation

| Metric | Value |
|--------|-------|
| PLCC   | {plcc} |
| SROCC  | {srocc} |
| RMSE   | {rmse} VMAF |
"""

_FR_V2_JSON = {
    "id": "fr_regressor_v2",
    "kind": "fr_regressor",
    "training": {
        "n_rows": 1200,
        "in_sample_plcc": 0.9412,
        "in_sample_srocc": 0.9388,
        "in_sample_rmse": 4.1230,
    },
}

_FR_V3_JSON = {
    "id": "fr_regressor_v3",
    "kind": "fr_regressor",
    "training": {
        "n_rows": 2400,
        "loso_mean_plcc": 0.9510,
        "loso_mean_srocc": 0.9490,
        "loso_mean_rmse": 3.8800,
    },
}

_PROMOTE_JSON = {
    "schema_version": 1,
    "gate": {
        "mean_plcc": 0.9411,
        "mean_plcc_pass": True,
        "mean_plcc_threshold": 0.90,
        "plcc_spread": 0.002500,
        "plcc_spread_max": 0.01,
        "plcc_spread_pass": True,
        "passed": True,
        "failing_seeds": [],
        "per_seed_min": 0.9385,
        "per_seed_pass": True,
        "per_seed_plccs": [0.9385, 0.9411, 0.9420],
    },
}

_SAL_V1_JSON = {
    "kind": "saliency_student",
    "name": "saliency_student_v1",
    "training": {
        "best_val_iou": 0.6120,
        "param_count": 185_000,
        "decoder_upsampler": "ConvTranspose2d",
    },
}

_SAL_V2_JSON = {
    "kind": "saliency_student",
    "name": "saliency_student_v2",
    "training": {
        "best_val_iou": 0.6480,
        "param_count": 320_000,
        "decoder_upsampler": "PixelShuffle decoder",
    },
}


def _write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")


def _write_card(repo: Path, codec: str, corpus_kind: str, plcc: str, srocc: str, rmse: str) -> None:
    card = _CARD_TEMPLATE.format(
        codec=codec,
        corpus_kind=corpus_kind,
        plcc=plcc,
        srocc=srocc,
        rmse=rmse,
    )
    path = repo / "model" / f"predictor_{codec}_card.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(card, encoding="utf-8")


def _build_minimal_repo(tmp_path: Path) -> Path:
    """Build a minimal repo tree with all files that render_report requires."""
    tiny = tmp_path / "model" / "tiny"
    tiny.mkdir(parents=True, exist_ok=True)

    _write_json(tiny / "fr_regressor_v2.json", _FR_V2_JSON)
    _write_json(tiny / "fr_regressor_v3.json", _FR_V3_JSON)
    _write_json(tiny / "fr_regressor_v2_ensemble_v1_seed_flip_PROMOTE.json", _PROMOTE_JSON)
    _write_json(tiny / "saliency_student_v1.json", _SAL_V1_JSON)
    _write_json(tiny / "saliency_student_v2.json", _SAL_V2_JSON)

    _write_card(tmp_path, "h264_nvenc", "real_hw", "0.9200", "0.9100", "5.1000")
    _write_card(tmp_path, "h264_qsv", "real_hw", "0.9250", "0.9150", "4.9000")
    _write_card(tmp_path, "hevc_nvenc", "real_hw", "0.9100", "0.9000", "5.5000")
    _write_card(tmp_path, "hevc_qsv", "real_hw", "0.9120", "0.9020", "5.4000")

    return tmp_path


# ---------------------------------------------------------------------------
# _extract_metric_table_value
# ---------------------------------------------------------------------------


def test_extract_metric_table_value_success() -> None:
    text = "| PLCC   | 0.9412 |\n| SROCC  | 0.9388 |\n"
    assert tdr._extract_metric_table_value(text, "PLCC") == pytest.approx(0.9412)
    assert tdr._extract_metric_table_value(text, "SROCC") == pytest.approx(0.9388)


def test_extract_metric_table_value_missing_raises() -> None:
    with pytest.raises(ValueError, match="missing RMSE"):
        tdr._extract_metric_table_value("| PLCC | 0.9 |", "RMSE")


def test_extract_metric_table_value_rejects_non_number() -> None:
    # A line where the value field starts with a letter should not match.
    text = "| PLCC | n/a |\n"
    with pytest.raises(ValueError, match="missing PLCC"):
        tdr._extract_metric_table_value(text, "PLCC")


# ---------------------------------------------------------------------------
# _extract_bold_field
# ---------------------------------------------------------------------------


def test_extract_bold_field_success() -> None:
    text = "- **Codec adapter**: `h264_nvenc`\n- **Corpus kind**: `real_hw`\n"
    assert tdr._extract_bold_field(text, "Codec adapter") == "h264_nvenc"
    assert tdr._extract_bold_field(text, "Corpus kind") == "real_hw"


def test_extract_bold_field_missing_raises() -> None:
    with pytest.raises(ValueError, match="missing Codec adapter"):
        tdr._extract_bold_field("- **Other field**: `value`\n", "Codec adapter")


def test_extract_bold_field_strips_whitespace() -> None:
    text = "- **Codec adapter**:   h264_nvenc  \n"
    assert tdr._extract_bold_field(text, "Codec adapter") == "h264_nvenc"


# ---------------------------------------------------------------------------
# _float_or_none
# ---------------------------------------------------------------------------


def test_float_or_none_with_int() -> None:
    assert tdr._float_or_none(5) == pytest.approx(5.0)


def test_float_or_none_with_float() -> None:
    assert tdr._float_or_none(0.95) == pytest.approx(0.95)


def test_float_or_none_with_none() -> None:
    assert tdr._float_or_none(None) is None


def test_float_or_none_with_string() -> None:
    assert tdr._float_or_none("0.95") is None


# ---------------------------------------------------------------------------
# _fmt
# ---------------------------------------------------------------------------


def test_fmt_default_digits() -> None:
    assert tdr._fmt(0.9412) == "0.9412"


def test_fmt_custom_digits() -> None:
    assert tdr._fmt(0.9412, digits=2) == "0.94"


def test_fmt_none() -> None:
    assert tdr._fmt(None) == "-"


# ---------------------------------------------------------------------------
# _markdown_table
# ---------------------------------------------------------------------------


def test_markdown_table_shape() -> None:
    result = tdr._markdown_table(["A", "B"], [["x", "y"], ["1", "2"]])
    lines = result.splitlines()
    assert lines[0] == "| A | B |"
    assert lines[1] == "| --- | --- |"
    assert "| x | y |" in lines
    assert "| 1 | 2 |" in lines


def test_markdown_table_empty_rows() -> None:
    headers = ["A", "B"]
    result = tdr._markdown_table(headers, [])
    lines = result.splitlines()
    # Header row + separator row only (no data rows)
    expected_line_count = 2  # header + separator
    assert len(lines) == expected_line_count


# ---------------------------------------------------------------------------
# PredictorCard.family
# ---------------------------------------------------------------------------


def test_predictor_card_family_with_underscore() -> None:
    card = tdr.PredictorCard(
        codec="h264_nvenc",
        corpus_kind="real_hw",
        plcc=0.92,
        srocc=0.91,
        rmse=5.1,
        path=Path("model/predictor_h264_nvenc_card.md"),
    )
    assert card.family == "nvenc"


def test_predictor_card_family_without_underscore() -> None:
    card = tdr.PredictorCard(
        codec="h264",
        corpus_kind="real_hw",
        plcc=0.92,
        srocc=0.91,
        rmse=5.1,
        path=Path("model/predictor_h264_card.md"),
    )
    assert card.family == "h264"


# ---------------------------------------------------------------------------
# load_predictor_cards
# ---------------------------------------------------------------------------


def test_load_predictor_cards_empty_when_no_cards(tmp_path: Path) -> None:
    (tmp_path / "model").mkdir(parents=True, exist_ok=True)
    cards = tdr.load_predictor_cards(tmp_path)
    assert cards == []


def test_load_predictor_cards_returns_correct_fields(tmp_path: Path) -> None:
    _write_card(tmp_path, "h264_nvenc", "real_hw", "0.9200", "0.9100", "5.1000")
    cards = tdr.load_predictor_cards(tmp_path)
    assert len(cards) == 1
    card = cards[0]
    assert card.codec == "h264_nvenc"
    assert card.corpus_kind == "real_hw"
    assert card.plcc == pytest.approx(0.92)
    assert card.srocc == pytest.approx(0.91)
    assert card.rmse == pytest.approx(5.1)


def test_load_predictor_cards_rejects_synthetic_stub(tmp_path: Path) -> None:
    stub_text = (
        "# stub card\n\n"
        "This is a synthetic-stub model card.\n\n"
        "- **Codec adapter**: `stub_codec`\n"
        "- **Corpus kind**: `synthetic`\n"
        "| PLCC | 0.1 |\n| SROCC | 0.1 |\n| RMSE | 99.0 |\n"
    )
    card_path = tmp_path / "model" / "predictor_stub_codec_card.md"
    card_path.parent.mkdir(parents=True, exist_ok=True)
    card_path.write_text(stub_text, encoding="utf-8")
    with pytest.raises(ValueError, match="synthetic-stub model cards are not allowed"):
        tdr.load_predictor_cards(tmp_path)


# ---------------------------------------------------------------------------
# _predictor_rows
# ---------------------------------------------------------------------------


def test_predictor_rows_sorted_by_family_then_codec() -> None:
    cards = [
        tdr.PredictorCard("h264_qsv", "real_hw", 0.92, 0.91, 5.0, Path("q.md")),
        tdr.PredictorCard("h264_nvenc", "real_hw", 0.93, 0.92, 4.9, Path("n.md")),
        tdr.PredictorCard("hevc_nvenc", "real_hw", 0.91, 0.90, 5.5, Path("hn.md")),
    ]
    rows = tdr._predictor_rows(cards)
    # Sorted by (family, codec): nvenc cards first by family=nvenc, then qsv
    codecs = [row[0] for row in rows]
    assert codecs == sorted(codecs, key=lambda c: (c.rsplit("_", 1)[-1], c))


# ---------------------------------------------------------------------------
# _qsv_nvenc_delta_rows — delta between QSV and NVENC predictor metrics
# ---------------------------------------------------------------------------


def test_qsv_nvenc_delta_rows_with_matching_pairs(tmp_path: Path) -> None:
    cards = [
        tdr.PredictorCard("h264_nvenc", "real_hw", 0.9200, 0.9100, 5.1000, Path("a.md")),
        tdr.PredictorCard("h264_qsv", "real_hw", 0.9250, 0.9150, 4.9000, Path("b.md")),
    ]
    rows = tdr._qsv_nvenc_delta_rows(cards)
    assert len(rows) == 1
    assert rows[0][0] == "h264"
    # Delta column = qsv.plcc - nvenc.plcc
    delta = float(rows[0][3])
    assert delta == pytest.approx(0.9250 - 0.9200, abs=1e-4)


def test_qsv_nvenc_delta_rows_skips_incomplete_families() -> None:
    # Only nvenc for hevc, no qsv — should produce no delta row.
    cards = [
        tdr.PredictorCard("hevc_nvenc", "real_hw", 0.91, 0.90, 5.5, Path("a.md")),
    ]
    rows = tdr._qsv_nvenc_delta_rows(cards)
    assert rows == []


def test_qsv_nvenc_delta_rows_multiple_families(tmp_path: Path) -> None:
    expected_families = {"h264", "hevc"}
    cards = [
        tdr.PredictorCard("h264_nvenc", "real_hw", 0.92, 0.91, 5.1, Path("a.md")),
        tdr.PredictorCard("h264_qsv", "real_hw", 0.93, 0.92, 4.9, Path("b.md")),
        tdr.PredictorCard("hevc_nvenc", "real_hw", 0.90, 0.89, 5.8, Path("c.md")),
        tdr.PredictorCard("hevc_qsv", "real_hw", 0.91, 0.90, 5.6, Path("d.md")),
    ]
    rows = tdr._qsv_nvenc_delta_rows(cards)
    assert len(rows) == len(expected_families)
    families = {row[0] for row in rows}
    assert families == expected_families


# ---------------------------------------------------------------------------
# Full render_report — needs all tiny JSON files + predictor cards
# ---------------------------------------------------------------------------


def test_render_report_sections_present(tmp_path: Path) -> None:
    _build_minimal_repo(tmp_path)
    report = tdr.render_report(tmp_path)
    assert "# Training Discovery Report" in report
    assert "## Tiny FR Regressors" in report
    assert "## Saliency Students" in report
    assert "## Real Hardware Predictor Cards" in report
    assert "## QSV vs NVENC Predictor Delta" in report


def test_render_report_fr_regressor_data(tmp_path: Path) -> None:
    _build_minimal_repo(tmp_path)
    report = tdr.render_report(tmp_path)
    # in-sample row from fr_regressor_v2
    assert "fr_regressor_v2" in report
    assert "in-sample" in report
    # LOSO row from fr_regressor_v3
    assert "fr_regressor_v3" in report
    assert "LOSO" in report
    # Ensemble promote row
    assert "fr_regressor_v2_ensemble_v1" in report


def test_render_report_saliency_data(tmp_path: Path) -> None:
    _build_minimal_repo(tmp_path)
    report = tdr.render_report(tmp_path)
    assert "saliency_student_v1" in report
    assert "saliency_student_v2" in report


def test_render_report_predictor_card_data(tmp_path: Path) -> None:
    _build_minimal_repo(tmp_path)
    report = tdr.render_report(tmp_path)
    assert "h264_nvenc" in report
    assert "h264_qsv" in report
    assert "h264" in report  # delta section


# ---------------------------------------------------------------------------
# main() — CLI wiring
# ---------------------------------------------------------------------------


def test_main_prints_to_stdout(tmp_path: Path, capsys) -> None:
    _build_minimal_repo(tmp_path)
    rc = tdr.main(["--repo-root", str(tmp_path)])
    assert rc == 0
    captured = capsys.readouterr()
    assert "Training Discovery Report" in captured.out


def test_main_writes_to_file(tmp_path: Path) -> None:
    _build_minimal_repo(tmp_path)
    out_file = tmp_path / "out" / "report.md"
    rc = tdr.main(["--repo-root", str(tmp_path), "--output", str(out_file)])
    assert rc == 0
    assert out_file.exists()
    assert "Training Discovery Report" in out_file.read_text(encoding="utf-8")


def test_main_creates_parent_directory(tmp_path: Path) -> None:
    _build_minimal_repo(tmp_path)
    out_file = tmp_path / "nested" / "deep" / "report.md"
    rc = tdr.main(["--repo-root", str(tmp_path), "--output", str(out_file)])
    assert rc == 0
    assert out_file.exists()
