# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the Phase-G corpus benchmark report."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.benchmark import (
    render_csv,
    render_markdown,
    summaries_to_dicts,
    summarize_benchmark,
)
from vmaftune.cli import main as cli_main


def _row(
    *,
    encoder: str,
    preset: str = "medium",
    crf: int = 28,
    vmaf: float = 92.0,
    bitrate: float = 2000.0,
    exit_status: int = 0,
) -> dict:
    return {
        "src": "clip_a.yuv",
        "encoder": encoder,
        "preset": preset,
        "crf": crf,
        "vmaf_score": vmaf,
        "bitrate_kbps": bitrate,
        "exit_status": exit_status,
        "duration_s": 10.0,
        "framerate": 24.0,
        "encode_time_ms": 5000.0,
        "score_time_ms": 2000.0,
        "vmaf_model": "vmaf_v0.6.1",
    }


def _write_jsonl(path: Path, rows: list[dict]) -> Path:
    path.write_text(
        "".join(json.dumps(row) + "\n" for row in rows),
        encoding="utf-8",
    )
    return path


def test_summarize_benchmark_picks_lowest_bitrate_clearing_target():
    summaries = summarize_benchmark(
        [
            _row(encoder="libx264", crf=24, vmaf=95.0, bitrate=4000.0),
            _row(encoder="libx264", crf=28, vmaf=92.5, bitrate=2500.0),
            _row(encoder="libx265", crf=26, vmaf=93.0, bitrate=1900.0),
            _row(encoder="libx265", crf=30, vmaf=91.0, bitrate=1300.0),
        ],
        target_vmaf=92.0,
        baseline_encoder="libx264",
    )

    payload = summaries_to_dicts(summaries)
    assert [item["encoder"] for item in payload] == ["libx265", "libx264"]
    assert payload[0]["best"]["crf"] == 26
    assert payload[0]["status"] == "ok"
    assert payload[0]["bitrate_delta_pct"] == pytest.approx(-24.0)
    assert payload[1]["best"]["crf"] == 28


def test_summarize_benchmark_reports_unmet_encoder_as_closest_miss():
    summaries = summarize_benchmark(
        [
            _row(encoder="libx264", vmaf=92.1, bitrate=2500.0),
            _row(encoder="libaom-av1", crf=38, vmaf=89.0, bitrate=1200.0),
            _row(encoder="libaom-av1", crf=34, vmaf=90.5, bitrate=1800.0),
        ],
        target_vmaf=92.0,
    )

    unmet = next(item for item in summaries if item.encoder == "libaom-av1")
    assert unmet.status == "unmet"
    assert unmet.best_row["crf"] == 34
    assert unmet.margin == pytest.approx(-1.5)


def test_summarize_benchmark_filters_failed_and_nan_rows():
    summaries = summarize_benchmark(
        [
            _row(encoder="libx264", vmaf=float("nan"), bitrate=1000.0),
            _row(encoder="libx264", vmaf=99.0, bitrate=100.0, exit_status=1),
            _row(encoder="libx264", vmaf=93.0, bitrate=2000.0),
        ],
        target_vmaf=92.0,
    )

    assert len(summaries) == 1
    assert summaries[0].rows == 1
    assert summaries[0].bitrate_kbps == pytest.approx(2000.0)


def test_render_markdown_and_csv_include_delta_columns():
    summaries = summarize_benchmark(
        [
            _row(encoder="libx264", vmaf=93.0, bitrate=2000.0),
            _row(encoder="libx265", vmaf=93.0, bitrate=1600.0),
        ],
        target_vmaf=92.0,
        baseline_encoder="libx264",
    )

    markdown = render_markdown(summaries)
    csv_text = render_csv(summaries)
    assert "| Encoder | Status |" in markdown
    assert "libx265" in markdown
    assert "bitrate_delta_pct" in csv_text
    assert "-20.000" in csv_text


def test_cli_benchmark_json_from_corpus(tmp_path: Path, capsys):
    corpus = _write_jsonl(
        tmp_path / "corpus.jsonl",
        [
            _row(encoder="libx264", vmaf=93.0, bitrate=2000.0),
            _row(encoder="libx265", vmaf=94.0, bitrate=1500.0),
        ],
    )

    rc = cli_main(
        [
            "benchmark",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "92",
            "--baseline-encoder",
            "libx264",
            "--format",
            "json",
        ]
    )

    assert rc == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload[0]["encoder"] == "libx265"
    assert payload[0]["bitrate_delta_pct"] == pytest.approx(-25.0)


def test_cli_benchmark_missing_corpus_returns_usage_error(capsys):
    rc = cli_main(["benchmark", "--from-corpus", "/does/not/exist.jsonl"])

    assert rc == 2
    assert "corpus file not found" in capsys.readouterr().err
