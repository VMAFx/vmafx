# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the predictor v2 real-corpus LOSO trainer + ADR-0303 gate.

Pins three load-bearing properties:

1. **Gate enforcement is honest.** A synthetic FoldResult set with mean
   PLCC = 0.85 must be reported as FAIL — the gate cannot be silently
   relaxed (per CLAUDE.md §13 / ``feedback_no_test_weakening``).
2. **LOSO partitioning is by source, not by row.** A held-out fold's
   sources do not appear in the training fold, even when the same
   source contributes many rows.
3. **Trainer-logic primitives** (corpus discovery, row loading, source
   counting, report rendering) are usable on a host without torch /
   onnxruntime — the gate logic alone runs in CI.

The real-corpus path itself (per-fold MLP training) is not exercised
here — that needs torch + a real corpus, both out of scope for the
generic CI runner. The fold-training body is covered by the existing
``tools/vmaf-tune/tests/test_predictor_train.py`` suite from PR #450.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "ai" / "scripts"))

import train_predictor_v2_realcorpus as trainer  # noqa: E402

# ---------------------------------------------------------------------
# 1. Gate enforcement — the load-bearing constraint
# ---------------------------------------------------------------------


def _make_fold(
    idx: int, plcc: float, *, srocc: float = 0.95, rmse: float = 1.0
) -> trainer.FoldResult:
    return trainer.FoldResult(
        fold_index=idx,
        held_out_sources=(f"src_{idx}",),
        plcc=plcc,
        srocc=srocc,
        rmse=rmse,
        n_train=80,
        n_val=20,
    )


def test_gate_passes_on_clean_results() -> None:
    """Mean PLCC = 0.96 across 5 folds with spread 0.002 -> PASS."""
    folds = [_make_fold(i, p) for i, p in enumerate([0.961, 0.960, 0.962, 0.959, 0.961])]
    passed, reasons = trainer.evaluate_gate("libx264", folds)
    assert passed, f"expected PASS, got reasons={reasons}"
    assert reasons == ()


def test_gate_fails_on_low_mean_plcc() -> None:
    """Mean PLCC = 0.85 -> FAIL — the load-bearing test.

    Per CLAUDE.md §13 / ``feedback_no_test_weakening``: the gate must
    NOT be silently relaxed. If a real-corpus run produces 0.85, the
    trainer reports FAIL and the model card stays Status: Proposed.
    """
    folds = [_make_fold(i, 0.85) for i in range(5)]
    passed, reasons = trainer.evaluate_gate("libx264", folds)
    assert not passed
    # Reason must cite the actual threshold so the operator can audit.
    assert any("0.95" in r and "ADR-0303" in r for r in reasons), reasons


def test_gate_fails_on_oversized_spread() -> None:
    """Mean PLCC = 0.96 but spread = 0.04 -> FAIL part 2."""
    folds = [_make_fold(i, p) for i, p in enumerate([0.97, 0.93, 0.97, 0.97, 0.97])]
    passed, reasons = trainer.evaluate_gate("libx264", folds)
    assert not passed
    # Spread reason should be one of the failure reasons.
    assert any("spread" in r for r in reasons), reasons


def test_gate_fails_when_one_fold_below_per_fold_min() -> None:
    """Even with mean PLCC >= 0.95, a single fold below the per-fold
    floor blocks the codec — mirrors ensemble_prod_gate's --per-seed-min.
    """
    folds = [_make_fold(i, p) for i, p in enumerate([0.960, 0.961, 0.962, 0.949, 0.961])]
    passed, reasons = trainer.evaluate_gate("libx264", folds)
    assert not passed
    assert any("per-fold" in r for r in reasons), reasons


def test_gate_constants_match_adr_0303() -> None:
    """Constants must match ADR-0303 §Decision (and ensemble_prod_gate.py).

    A future ADR may supersede; lowering these in code without an ADR
    change is the failure mode this test catches.
    """
    assert trainer.SHIP_GATE_MEAN_PLCC == 0.95
    assert trainer.SHIP_GATE_PLCC_SPREAD_MAX == 0.005
    assert trainer.SHIP_GATE_PER_FOLD_MIN == 0.95
    assert trainer.LOSO_FOLD_COUNT == 5


def test_gate_reports_no_folds_as_failure() -> None:
    """Empty fold list -> FAIL with a helpful reason, never silent PASS."""
    passed, reasons = trainer.evaluate_gate("libx264", [])
    assert not passed
    assert any("no folds" in r for r in reasons), reasons


# ---------------------------------------------------------------------
# 2. LOSO partitioning — source-level, not row-level
# ---------------------------------------------------------------------


def _row(src: str, score: float, **extra) -> dict:
    return {
        "encoder": "libx264",
        "src": src,
        "vmaf_score": score,
        "exit_status": 0,
        **extra,
    }


def test_loso_folds_partition_by_source() -> None:
    """Each fold's val sources are absent from its train sources."""
    rows = [_row(f"src_{i:02d}", 80.0 + i) for i in range(20)]
    # Multiple rows per source — ensure the partition is by source.
    rows.extend(_row(f"src_{i:02d}", 81.0 + i) for i in range(20))
    folds = trainer.loso_folds(rows, n_folds=5, seed=0)
    assert len(folds) == 5
    for train, val in folds:
        train_srcs = {trainer.row_source(r) for r in train}
        val_srcs = {trainer.row_source(r) for r in val}
        assert train_srcs.isdisjoint(val_srcs), f"LOSO leak: {train_srcs & val_srcs}"


def test_loso_returns_empty_for_too_few_sources() -> None:
    """Corpus with fewer than 5 distinct sources -> no folds."""
    rows = [_row(f"src_{i}", 80.0) for i in range(3)]
    assert trainer.loso_folds(rows, n_folds=5) == []


def test_loso_seed_is_deterministic() -> None:
    """Same seed -> identical fold partitioning (reproducibility)."""
    rows = [_row(f"src_{i:02d}", 80.0 + i) for i in range(10)]
    a = trainer.loso_folds(rows, n_folds=5, seed=7)
    b = trainer.loso_folds(rows, n_folds=5, seed=7)
    a_val_sources = [tuple(sorted({trainer.row_source(r) for r in v})) for _, v in a]
    b_val_sources = [tuple(sorted({trainer.row_source(r) for r in v})) for _, v in b]
    assert a_val_sources == b_val_sources


# ---------------------------------------------------------------------
# 3. Corpus loading + filtering
# ---------------------------------------------------------------------


def test_load_rows_filters_by_codec(tmp_path: Path) -> None:
    """Only rows where ``encoder == codec`` survive."""
    corpus = tmp_path / "mixed.jsonl"
    rows = [
        {"encoder": "libx264", "src": "a", "vmaf_score": 90.0, "exit_status": 0},
        {"encoder": "libx265", "src": "a", "vmaf_score": 88.0, "exit_status": 0},
        {"encoder": "libx264", "src": "b", "vmaf_score": 91.0, "exit_status": 0},
    ]
    corpus.write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
    cfile = trainer.CorpusFile(path=corpus, root=tmp_path)
    loaded = trainer.load_rows([cfile], "libx264")
    assert len(loaded) == 2
    assert all(r["encoder"] == "libx264" for r in loaded)
    # Provenance is tagged for the report.
    assert all(r["_source_corpus"] == str(corpus) for r in loaded)


def test_load_rows_drops_failed_encodes(tmp_path: Path) -> None:
    """Rows with non-zero exit_status are dropped (corpus hygiene)."""
    corpus = tmp_path / "mixed.jsonl"
    rows = [
        {"encoder": "libx264", "src": "a", "vmaf_score": 90.0, "exit_status": 0},
        {"encoder": "libx264", "src": "b", "vmaf_score": 91.0, "exit_status": 1},
        {"encoder": "libx264", "src": "c", "vmaf_score": float("nan"), "exit_status": 0},
    ]
    corpus.write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
    cfile = trainer.CorpusFile(path=corpus, root=tmp_path)
    loaded = trainer.load_rows([cfile], "libx264")
    assert len(loaded) == 1
    assert loaded[0]["src"] == "a"


def test_load_rows_accepts_canonical6_vmaf_field(tmp_path: Path) -> None:
    """Canonical-6 rows use ``vmaf`` (not ``vmaf_score``); both supported."""
    corpus = tmp_path / "canonical6.jsonl"
    rows = [
        {"encoder": "libx264", "src": "a", "vmaf": 90.5, "exit_status": 0},
    ]
    corpus.write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
    cfile = trainer.CorpusFile(path=corpus, root=tmp_path)
    loaded = trainer.load_rows([cfile], "libx264")
    assert len(loaded) == 1
    assert loaded[0]["vmaf_score"] == pytest.approx(90.5)


# ---------------------------------------------------------------------
# 4. Corpus discovery
# ---------------------------------------------------------------------


def test_discover_corpora_skips_missing_roots(tmp_path: Path) -> None:
    """Missing roots do not raise — operators may have only one corpus."""
    present = tmp_path / "present"
    present.mkdir()
    (present / "a.jsonl").write_text("{}\n", encoding="utf-8")
    missing = tmp_path / "missing"
    discovered = trainer.discover_corpora([present, missing])
    assert len(discovered) == 1
    assert discovered[0].path.name == "a.jsonl"


def test_discover_corpora_recurses(tmp_path: Path) -> None:
    """Nested *.jsonl files surface."""
    sub = tmp_path / "sub" / "nested"
    sub.mkdir(parents=True)
    (sub / "deep.jsonl").write_text("{}\n", encoding="utf-8")
    discovered = trainer.discover_corpora([tmp_path])
    assert len(discovered) == 1
    assert discovered[0].path.name == "deep.jsonl"


# ---------------------------------------------------------------------
# 5. Report rendering — JSON schema is consumed by the shell wrapper
# ---------------------------------------------------------------------


def test_render_report_carries_gate_constants() -> None:
    """The report must surface the gate values it was trained against."""
    folds = [_make_fold(i, 0.96) for i in range(5)]
    result = trainer.CodecResult(
        codec="libx264",
        status="pass",
        folds=tuple(folds),
        mean_plcc=0.96,
        plcc_spread=0.001,
        mean_srocc=0.95,
        mean_rmse=1.0,
        n_rows_total=100,
        n_distinct_sources=5,
        failure_reasons=(),
        corpus_provenance=("fixtures/c.jsonl",),
    )
    report = trainer.render_report([result], corpus_files=[])
    assert report["gate"]["mean_plcc_threshold"] == 0.95
    assert report["gate"]["plcc_spread_max"] == 0.005
    assert report["gate"]["per_fold_min"] == 0.95
    assert report["gate"]["adr"] == "ADR-0303"
    assert report["summary"]["n_pass"] == 1
    assert report["summary"]["n_fail"] == 0
    codec_payload = report["codecs"][0]
    assert codec_payload["status"] == "pass"
    assert len(codec_payload["folds"]) == 5


def test_main_empty_report_records_run_provenance(tmp_path: Path) -> None:
    """Diagnostic reports identify the command and corpus roots that made them."""
    report_out = tmp_path / "report.json"
    missing_root = tmp_path / "missing-corpus-root"

    rc = trainer.main(
        [
            "--allow-empty",
            "--codec",
            "libx264",
            "--corpus-root",
            str(missing_root),
            "--report-out",
            str(report_out),
        ]
    )

    assert rc == 1
    payload = json.loads(report_out.read_text(encoding="utf-8"))
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/train_predictor_v2_realcorpus.py"
    assert provenance["argv"] == [
        "--allow-empty",
        "--codec",
        "libx264",
        "--corpus-root",
        str(missing_root),
        "--report-out",
        str(report_out),
    ]
    assert provenance["args"]["allow_empty"] is True
    assert provenance["args"]["codec"] == ["libx264"]
    assert provenance["inputs"]["corpus_roots"] == [
        {"exists": False, "kind": "missing", "path": str(missing_root)}
    ]
    assert provenance["inputs"]["resolved_corpus_files"] == []
    assert provenance["outputs"]["report_target"] == str(report_out)
    assert payload["summary"]["n_missing_rows"] == 1
    codec_payload = payload["codecs"][0]
    assert codec_payload["status"] == "missing-rows"
    assert codec_payload["folds"] == []


def test_render_report_summarises_failure() -> None:
    """A FAIL codec lands in n_fail with reasons preserved."""
    result = trainer.CodecResult(
        codec="libvvenc",
        status="fail",
        folds=(),
        mean_plcc=0.85,
        plcc_spread=0.02,
        mean_srocc=0.84,
        mean_rmse=3.5,
        n_rows_total=50,
        n_distinct_sources=5,
        failure_reasons=("mean PLCC 0.8500 < 0.9500 (ADR-0303 part 1)",),
        corpus_provenance=(),
    )
    report = trainer.render_report([result], corpus_files=[])
    assert report["summary"]["n_fail"] == 1
    assert report["codecs"][0]["status"] == "fail"
    assert "ADR-0303" in report["codecs"][0]["failure_reasons"][0]


def test_render_human_summary_marks_failures_visibly() -> None:
    """Human summary must clearly mark FAIL codecs (no silent pass)."""
    result = trainer.CodecResult(
        codec="libvvenc",
        status="fail",
        folds=(),
        mean_plcc=0.85,
        plcc_spread=0.02,
        mean_srocc=0.84,
        mean_rmse=3.5,
        n_rows_total=50,
        n_distinct_sources=5,
        failure_reasons=("mean PLCC 0.8500 < 0.9500 (ADR-0303 part 1)",),
        corpus_provenance=(),
    )
    report = trainer.render_report([result], corpus_files=[])
    text = trainer.render_human_summary(report)
    assert "FAIL" in text
    assert "libvvenc" in text
    assert "ADR-0303" in text


# ---------------------------------------------------------------------
# 6. CodecResult status semantics — every value is honest
# ---------------------------------------------------------------------


def test_train_codec_loso_returns_missing_rows_on_empty() -> None:
    """No rows -> status missing-rows, never crashes the batch."""
    result = trainer.train_codec_loso("libx264", [])
    assert result.status == "missing-rows"
    assert result.folds == ()
    assert "no rows" in " ".join(result.failure_reasons)


def test_train_codec_loso_returns_insufficient_sources() -> None:
    """Rows but < 5 distinct sources -> status insufficient-sources."""
    rows = [_row(f"src_{i}", 80.0) for i in range(3)]
    result = trainer.train_codec_loso("libx264", rows)
    assert result.status == "insufficient-sources"
    assert "5 distinct sources" in " ".join(result.failure_reasons)


# ---------------------------------------------------------------------
# 7. CLI surface
# ---------------------------------------------------------------------


def test_cli_rejects_unknown_codec(capsys: pytest.CaptureFixture[str]) -> None:
    """Unknown --codec values are rejected before any training."""
    with pytest.raises(SystemExit):
        trainer.main(["--codec", "not_a_real_codec", "--allow-empty"])
    captured = capsys.readouterr()
    assert "unknown codec" in captured.err.lower()


def test_cli_allow_empty_emits_diagnostic_report(tmp_path: Path) -> None:
    """``--allow-empty`` produces a complete report listing every codec.

    The trainer must not silently skip codecs; even with no corpus on
    disk the report enumerates all 14 with status='missing-rows'.
    """
    report_path = tmp_path / "report.json"
    rc = trainer.main(
        [
            "--allow-empty",
            "--report-out",
            str(report_path),
            "--corpus-root",
            str(tmp_path / "nonexistent"),
        ]
    )
    assert rc == 1  # no codec passed
    payload = json.loads(report_path.read_text(encoding="utf-8"))
    assert len(payload["codecs"]) == len(trainer.CODECS)
    assert all(c["status"] == "missing-rows" for c in payload["codecs"])
    # Every codec listed by name.
    seen = {c["codec"] for c in payload["codecs"]}
    assert seen == set(trainer.CODECS)


def test_codec_list_matches_pr450_runtime_contract() -> None:
    """The 14-codec list must mirror the runtime predictor's _DEFAULT_COEFFS.

    The trainer's _resolve_codecs() either imports the runtime list
    (when vmaf-tune is on the path) or falls back to a hard-coded
    mirror. Either way the count stays at 14 — drift is the failure
    this test catches.
    """
    assert len(trainer.CODECS) == 14
    # Spot-check load-bearing entries the runtime contract pins.
    assert "libx264" in trainer.CODECS
    assert "h264_nvenc" in trainer.CODECS
    assert "av1_qsv" in trainer.CODECS
