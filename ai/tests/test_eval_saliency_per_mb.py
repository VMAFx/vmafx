# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the saliency per-block IoU evaluator."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

np = pytest.importorskip("numpy")

_HERE = Path(__file__).resolve().parent
_REPO = _HERE.parent.parent
sys.path.insert(0, str(_REPO))

from ai.scripts.eval_saliency_per_mb import (  # noqa: E402
    block_iou,
    evaluate_dirs,
    evaluate_pair,
    load_mask,
    main,
    mask_to_blocks,
)


def test_mask_to_blocks_thresholds_block_means() -> None:
    mask = np.zeros((4, 4), dtype=np.float32)
    mask[:2, :2] = 1.0
    mask[2:, 2:] = 0.49

    blocks = mask_to_blocks(mask, block_size=2, threshold=0.5)

    assert blocks.tolist() == [[True, False], [False, False]]


def test_block_iou_counts_intersection_and_union() -> None:
    pred = np.asarray([[True, False], [True, False]])
    gt = np.asarray([[True, True], [False, False]])

    intersection, union, iou = block_iou(pred, gt)

    assert intersection == 1
    assert union == 3
    assert iou == pytest.approx(1.0 / 3.0)


def test_evaluate_pair_reports_per_mask_iou(tmp_path: Path) -> None:
    pred = np.zeros((4, 4), dtype=np.float32)
    gt = np.zeros((4, 4), dtype=np.float32)
    pred[:2, :2] = 1.0
    pred[:2, 2:] = 1.0
    gt[:2, :2] = 1.0
    pred_path = tmp_path / "pred.npy"
    gt_path = tmp_path / "gt.npy"
    np.save(pred_path, pred)
    np.save(gt_path, gt)

    row = evaluate_pair(pred_path, gt_path, block_size=2, threshold=0.5)

    assert row.pred_blocks == 2
    assert row.gt_blocks == 1
    assert row.intersection_blocks == 1
    assert row.union_blocks == 2
    assert row.iou == pytest.approx(0.5)


def test_evaluate_dirs_pairs_by_stem_and_aggregates(tmp_path: Path) -> None:
    pred_dir = tmp_path / "pred"
    gt_dir = tmp_path / "gt"
    pred_dir.mkdir()
    gt_dir.mkdir()
    np.save(pred_dir / "a.npy", np.ones((4, 4), dtype=np.float32))
    np.save(gt_dir / "a.npy", np.ones((4, 4), dtype=np.float32))
    np.save(pred_dir / "ignored.npy", np.zeros((4, 4), dtype=np.float32))

    payload = evaluate_dirs(pred_dir, gt_dir, block_size=2, threshold=0.5)

    assert payload["n_pairs"] == 1
    assert payload["macro_iou"] == pytest.approx(1.0)
    assert payload["micro_iou"] == pytest.approx(1.0)
    assert payload["rows"][0]["stem"] == "a"


def test_load_mask_reads_ascii_pgm(tmp_path: Path) -> None:
    pgm = tmp_path / "mask.pgm"
    pgm.write_text("P2\n# comment\n2 2\n255\n0 255 128 64\n", encoding="ascii")

    arr = load_mask(pgm)

    assert arr.shape == (2, 2)
    assert arr[0, 1] == pytest.approx(1.0)
    assert arr[1, 0] == pytest.approx(128.0 / 255.0)


def test_main_writes_json(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    pred_dir = tmp_path / "pred"
    gt_dir = tmp_path / "gt"
    pred_dir.mkdir()
    gt_dir.mkdir()
    np.save(pred_dir / "a.npy", np.ones((4, 4), dtype=np.float32))
    np.save(gt_dir / "a.npy", np.ones((4, 4), dtype=np.float32))
    out_json = tmp_path / "out.json"
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "eval_saliency_per_mb.py",
            "--pred-dir",
            str(pred_dir),
            "--gt-dir",
            str(gt_dir),
            "--block-size",
            "2",
            "--out-json",
            str(out_json),
        ],
    )

    assert main() == 0
    payload = json.loads(out_json.read_text(encoding="utf-8"))
    assert payload["n_pairs"] == 1
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert payload["run_provenance"]["entrypoint"]["path"] == "ai/scripts/eval_saliency_per_mb.py"
    assert payload["run_provenance"]["inputs"]["pred_dir"]["kind"] == "directory"
