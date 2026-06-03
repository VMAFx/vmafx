# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Per-seed training schema test for the ensemble LOSO trainer (ADR-0319).

Trains a 2-epoch sanity model on a synthetic 12-row JSONL corpus and
verifies the returned summary dict matches the schema
``scripts/ci/ensemble_prod_gate.py`` consumes (``mean_plcc`` plus
per-fold list, mean computed correctly). CPU-only — no CUDA needed.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

pd = pytest.importorskip("pandas")
np = pytest.importorskip("numpy")
torch = pytest.importorskip("torch")

from conftest import requires_pytorch_lightning  # noqa: E402

requires_pytorch_lightning()

import train_fr_regressor_v2_ensemble_loso as loso  # noqa: E402
from train_fr_regressor_v2_ensemble_loso import _load_corpus, _train_one_seed  # noqa: E402


def _write_synthetic_corpus(path: Path) -> None:
    """3 sources × 4 cqs × 2 frames = 24 rows, learnable cq → vmaf signal."""
    sources = ["srcA", "srcB", "srcC"]
    cqs = [19, 25, 31, 37]
    rng = np.random.default_rng(123)
    rows = []
    for src in sources:
        for cq in cqs:
            for f in range(2):
                rows.append(
                    {
                        "src": src,
                        "encoder": "h264_nvenc",
                        "cq": cq,
                        "frame_index": f,
                        "vmaf": float(95.0 - (cq - 19) * 0.6 + rng.normal(0, 0.3)),
                        "adm2": float(0.95 - (cq - 19) * 0.005),
                        "vif_scale0": float(0.85 - (cq - 19) * 0.003),
                        "vif_scale1": float(0.92 - (cq - 19) * 0.002),
                        "vif_scale2": float(0.97 - (cq - 19) * 0.001),
                        "vif_scale3": float(0.99 - (cq - 19) * 0.0005),
                        "motion2": float(rng.uniform(0.0, 5.0)),
                    }
                )
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row) + "\n")


def _make_args(corpus_path: Path, *, dry_run: bool = False) -> argparse.Namespace:
    return argparse.Namespace(
        seeds=[0],
        corpus=corpus_path,
        out_dir=corpus_path.parent,
        epochs=2,
        batch_size=8,
        lr=5e-4,
        weight_decay=1e-5,
        num_codecs=14,  # CODEC_BLOCK_DIM = 12 encoder one-hot + preset_norm + crf_norm
        dry_run=dry_run,
    )


def test_train_one_seed_returns_gate_compatible_schema(tmp_path: Path) -> None:
    corpus_path = tmp_path / "synth.jsonl"
    _write_synthetic_corpus(corpus_path)
    corpus = _load_corpus(corpus_path)
    args = _make_args(corpus_path)

    summary = _train_one_seed(seed=0, corpus=corpus, args=args)

    # Required by scripts/ci/ensemble_prod_gate.py.
    assert "mean_plcc" in summary
    assert isinstance(summary["mean_plcc"], float)

    # Per-fold trace per Research-0075 §JSON schema.
    assert "folds" in summary
    assert isinstance(summary["folds"], list)
    assert len(summary["folds"]) == 3  # 3 unique sources

    for fold in summary["folds"]:
        assert "held_out" in fold
        assert "plcc" in fold
        assert "srocc" in fold
        assert "rmse" in fold
        assert "n_train" in fold
        assert "n_val" in fold

    assert summary["seed"] == 0
    assert summary["n_folds"] == 3
    assert summary["epochs"] == 2
    assert "wall_time_s" in summary
    assert summary["wall_time_s"] >= 0.0

    # Mean across the per-fold list matches the reported aggregate
    # (within float tolerance; nan-folds are dropped before averaging).
    plcc_vals = [f["plcc"] for f in summary["folds"] if not np.isnan(f["plcc"])]
    if plcc_vals:
        assert summary["mean_plcc"] == pytest.approx(sum(plcc_vals) / len(plcc_vals))


def test_train_one_seed_dry_run_marks_note(tmp_path: Path) -> None:
    corpus_path = tmp_path / "synth.jsonl"
    _write_synthetic_corpus(corpus_path)
    corpus = _load_corpus(corpus_path)
    args = _make_args(corpus_path, dry_run=True)

    summary = _train_one_seed(seed=0, corpus=corpus, args=args)
    assert summary.get("note") == "dry-run"
    assert summary["epochs"] == 1
    assert "mean_plcc" in summary  # schema-shaped


def test_train_one_seed_min_max_consistent(tmp_path: Path) -> None:
    corpus_path = tmp_path / "synth.jsonl"
    _write_synthetic_corpus(corpus_path)
    corpus = _load_corpus(corpus_path)
    args = _make_args(corpus_path)

    summary = _train_one_seed(seed=1, corpus=corpus, args=args)
    plcc_vals = [f["plcc"] for f in summary["folds"] if not np.isnan(f["plcc"])]
    if plcc_vals:
        assert summary["min_plcc"] == pytest.approx(min(plcc_vals))
        assert summary["max_plcc"] == pytest.approx(max(plcc_vals))


def test_main_loso_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    corpus_path = tmp_path / "synth.jsonl"
    out_dir = tmp_path / "loso"
    _write_synthetic_corpus(corpus_path)

    def fake_train_one_seed(seed, corpus, args):  # type: ignore[no-untyped-def]
        assert seed == 7
        assert corpus["n_rows"] == 24
        assert args.epochs == 3
        return {
            "seed": seed,
            "corpus": str(args.corpus),
            "n_folds": 3,
            "folds": [],
            "mean_plcc": 0.99,
            "std_plcc": 0.0,
            "min_plcc": 0.98,
            "max_plcc": 0.99,
            "mean_srocc": 0.98,
            "mean_rmse": 0.1,
            "epochs": args.epochs,
            "batch_size": args.batch_size,
            "lr": args.lr,
            "weight_decay": args.weight_decay,
            "num_codecs": args.num_codecs,
            "encoder_vocab_version": 2,
            "wall_time_s": 0.0,
        }

    monkeypatch.setattr(loso, "_train_one_seed", fake_train_one_seed)

    rc = loso.main(
        [
            "--seeds",
            "7",
            "--corpus",
            str(corpus_path),
            "--out-dir",
            str(out_dir),
            "--epochs",
            "3",
        ]
    )

    assert rc == 0
    report = json.loads((out_dir / "loso_seed7.json").read_text(encoding="utf-8"))
    provenance = report["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/train_fr_regressor_v2_ensemble_loso.py"
    assert provenance["argv"] == [
        "--seeds",
        "7",
        "--corpus",
        str(corpus_path),
        "--out-dir",
        str(out_dir),
        "--epochs",
        "3",
    ]
    assert provenance["args"]["seeds"] == [7]
    assert provenance["inputs"]["corpus"]["kind"] == "file"
    assert provenance["outputs"]["report_target"] == str(out_dir / "loso_seed7.json")
