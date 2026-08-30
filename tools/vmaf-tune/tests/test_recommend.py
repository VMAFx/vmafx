# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for the ``recommend`` subcommand and library API.

Validates the predicate semantics from Buckets #4 + #5 of
Research-0061 — picks the smallest-CRF row whose VMAF clears a
threshold (``--target-vmaf``) and the row whose bitrate is closest
to a target (``--target-bitrate``). Mocks all binaries.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install (mirrors test_corpus.py).
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.cli import main as cli_main
from vmaftune.recommend import (
    RecommendRequest,
    pick_target_bitrate,
    pick_target_vmaf,
    recommend,
    validate_request,
)


def _row(
    *,
    encoder="libx264",
    preset="medium",
    crf=23,
    vmaf=90.0,
    bitrate=2000.0,
    exit_status=0,
) -> dict:
    return {
        "encoder": encoder,
        "preset": preset,
        "crf": crf,
        "vmaf_score": vmaf,
        "bitrate_kbps": bitrate,
        "exit_status": exit_status,
    }


def test_validate_request_rejects_both_targets():
    with pytest.raises(ValueError, match="mutually exclusive"):
        validate_request(RecommendRequest(target_vmaf=92.0, target_bitrate_kbps=5000.0))


def test_validate_request_rejects_no_target():
    with pytest.raises(ValueError, match="missing target"):
        validate_request(RecommendRequest())


def test_validate_request_accepts_either():
    validate_request(RecommendRequest(target_vmaf=92.0))
    validate_request(RecommendRequest(target_bitrate_kbps=5000.0))


def test_pick_target_vmaf_smallest_crf_clearing_bar():
    rows = [
        _row(crf=18, vmaf=98.0, bitrate=8000),
        _row(crf=22, vmaf=95.0, bitrate=5000),
        _row(crf=26, vmaf=92.5, bitrate=3000),
        _row(crf=30, vmaf=89.0, bitrate=2000),
    ]
    result = pick_target_vmaf(rows, target=92.0)
    # 18, 22, 26 all clear; smallest CRF wins.
    assert result.row["crf"] == 18
    assert "UNMET" not in result.predicate
    assert result.margin == pytest.approx(98.0 - 92.0)


def test_pick_target_vmaf_falls_back_to_max_when_unmet():
    rows = [
        _row(crf=22, vmaf=85.0, bitrate=5000),
        _row(crf=26, vmaf=80.0, bitrate=3000),
    ]
    result = pick_target_vmaf(rows, target=92.0)
    assert result.row["crf"] == 22  # highest VMAF
    assert "UNMET" in result.predicate
    assert result.margin < 0


def test_pick_target_bitrate_min_distance():
    rows = [
        _row(crf=22, vmaf=95.0, bitrate=8000),
        _row(crf=26, vmaf=92.0, bitrate=5200),  # 200 above target
        _row(crf=28, vmaf=90.0, bitrate=4800),  # 200 below target
        _row(crf=30, vmaf=88.0, bitrate=3000),
    ]
    result = pick_target_bitrate(rows, target_kbps=5000.0)
    # Two rows tie on |distance|=200; smaller CRF (26) wins.
    assert result.row["crf"] == 26
    assert result.margin == pytest.approx(200.0)


def test_recommend_filters_by_encoder_and_preset():
    rows = [
        _row(encoder="libx264", preset="medium", crf=22, vmaf=95.0, bitrate=5000),
        _row(encoder="libx265", preset="medium", crf=22, vmaf=96.0, bitrate=4000),
        _row(encoder="libx264", preset="slow", crf=22, vmaf=97.0, bitrate=4500),
    ]
    req = RecommendRequest(target_vmaf=90.0, encoder="libx264", preset="medium")
    result = recommend(rows, req)
    # libx265 + slow filtered out; only one row left, which clears.
    assert result.row["encoder"] == "libx264"
    assert result.row["preset"] == "medium"
    assert result.row["crf"] == 22


def test_recommend_drops_failed_rows():
    rows = [
        _row(crf=22, vmaf=95.0, bitrate=5000, exit_status=1),  # failed encode
        _row(crf=26, vmaf=92.5, bitrate=3000, exit_status=0),
    ]
    result = recommend(rows, RecommendRequest(target_vmaf=90.0))
    assert result.row["crf"] == 26


def test_recommend_drops_nan_vmaf():
    rows = [
        _row(crf=22, vmaf=float("nan"), bitrate=5000),
        _row(crf=26, vmaf=92.5, bitrate=3000),
    ]
    result = recommend(rows, RecommendRequest(target_vmaf=90.0))
    assert result.row["crf"] == 26
    assert not math.isnan(result.row["vmaf_score"])


def test_recommend_raises_on_empty_eligible():
    with pytest.raises(ValueError, match="no eligible rows"):
        recommend([], RecommendRequest(target_vmaf=90.0))


# ---------- CLI-level tests (argparse + exit codes) ----------


def _write_corpus(path: Path, rows: list[dict]) -> Path:
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row) + "\n")
    return path


def test_cli_rejects_both_targets(capsys):
    # Both --target-vmaf and --target-bitrate conflict; handler returns 2.
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            "/nonexistent.jsonl",
            "--target-vmaf",
            "92",
            "--target-bitrate",
            "5000",
        ]
    )
    assert rc == 2


def test_cli_rejects_no_target(capsys):
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            "/nonexistent.jsonl",
        ]
    )
    assert rc == 2


def test_cli_target_vmaf_from_corpus(tmp_path: Path, capsys):
    rows = [
        _row(crf=22, vmaf=95.0, bitrate=5000),
        _row(crf=26, vmaf=92.5, bitrate=3000),
        _row(crf=30, vmaf=88.0, bitrate=2000),
    ]
    corpus = _write_corpus(tmp_path / "c.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "92.0",
            "--json",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    payload = json.loads(out.strip())
    # Smallest CRF clearing 92 is 22 (vmaf=95).
    assert payload["crf"] == 22


def test_cli_target_bitrate_from_corpus(tmp_path: Path, capsys):
    rows = [
        _row(crf=22, vmaf=95.0, bitrate=8000),
        _row(crf=26, vmaf=92.0, bitrate=5200),
        _row(crf=28, vmaf=90.0, bitrate=4800),
        _row(crf=30, vmaf=88.0, bitrate=3000),
    ]
    corpus = _write_corpus(tmp_path / "c.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-bitrate",
            "5000",
            "--json",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out.strip())
    # Tie on |distance|=200; smaller CRF (26) wins.
    assert payload["crf"] == 26


def test_cli_from_corpus_drops_failed_rows(tmp_path: Path, capsys):
    rows = [
        _row(crf=18, vmaf=99.0, bitrate=9000, exit_status=1),
        _row(crf=26, vmaf=92.5, bitrate=3000, exit_status=0),
    ]
    corpus = _write_corpus(tmp_path / "c.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "92.0",
            "--json",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out.strip())
    assert payload["crf"] == 26


def test_cli_from_corpus_drops_nan_vmaf_rows(tmp_path: Path, capsys):
    rows = [
        _row(crf=18, vmaf=float("nan"), bitrate=9000),
        _row(crf=26, vmaf=92.5, bitrate=3000),
    ]
    corpus = _write_corpus(tmp_path / "c.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "92.0",
            "--json",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out.strip())
    assert payload["crf"] == 26
    assert not math.isnan(payload["vmaf_score"])


def test_cli_from_corpus_honours_encoder_filter(tmp_path: Path, capsys):
    rows = [
        _row(encoder="libx264", crf=22, vmaf=95.0, bitrate=5000),
        _row(encoder="libx265", crf=18, vmaf=98.0, bitrate=4500),
    ]
    corpus = _write_corpus(tmp_path / "c.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "92.0",
            "--encoder",
            "libx264",
            "--json",
        ]
    )
    assert rc == 0
    payload = json.loads(capsys.readouterr().out.strip())
    assert payload["encoder"] == "libx264"
    assert payload["crf"] == 22


def test_cli_human_readable_output(tmp_path: Path, capsys):
    rows = [_row(crf=22, vmaf=95.0, bitrate=5000)]
    corpus = _write_corpus(tmp_path / "c.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "90",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "crf=22" in out
    assert "vmaf=95.000" in out
    assert "predicate=" in out


def test_cli_unmet_target_still_returns_zero(tmp_path: Path, capsys):
    """When no row clears the bar, we still return the closest miss."""
    rows = [_row(crf=30, vmaf=80.0, bitrate=2000)]
    corpus = _write_corpus(tmp_path / "c.jsonl", rows)
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "92",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    assert "UNMET" in out


def test_cli_empty_corpus_returns_error(tmp_path: Path, capsys):
    corpus = _write_corpus(tmp_path / "c.jsonl", [])
    rc = cli_main(
        [
            "recommend",
            "--from-corpus",
            str(corpus),
            "--target-vmaf",
            "92",
        ]
    )
    assert rc == 2
    err = capsys.readouterr().err
    assert "no eligible rows" in err
