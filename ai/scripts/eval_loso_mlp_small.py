#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""LOSO evaluation harness for the mlp_small Netflix-corpus run.

Mirrors the per-fold accounting of MCP `compare_models` while
respecting the leave-one-source-out structure: each fold model is
scored on its own held-out clip; the two single-split baselines
(``vmaf_tiny_v1.onnx`` = mlp_small @val=Tennis, ``vmaf_tiny_v1_medium.onnx``
= mlp_medium @val=Tennis) are scored on every clip plus the
all-clips concat for a same-axes comparison.

Outputs:
  runs/loso_eval/loso_mlp_small_eval.json   --- machine-readable
  runs/loso_eval/loso_mlp_small_eval.md     --- markdown summary
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np
import onnx
import onnxruntime as ort
from onnx.external_data_helper import load_external_data_for_model
from scipy.stats import pearsonr, spearmanr

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT))

from ai.train.dataset import NetflixFrameDataset  # noqa: E402

CLIPS = [
    "BigBuckBunny",
    "BirdsInCage",
    "CrowdRun",
    "ElFuente1",
    "ElFuente2",
    "FoxBird",
    "OldTownCross",
    "Seeking",
    "Tennis",
]


def _metrics(pred: np.ndarray, y: np.ndarray) -> dict[str, float]:
    return {
        "n": len(y),
        "plcc": float(pearsonr(pred, y).statistic),
        "srocc": float(spearmanr(pred, y).statistic),
        "rmse": float(np.sqrt(((pred - y) ** 2).mean())),
    }


def _eval(session: ort.InferenceSession, x: np.ndarray, y: np.ndarray) -> dict[str, float]:
    input_name = session.get_inputs()[0].name
    pred = np.asarray(session.run(None, {input_name: x.astype(np.float32)})[0]).reshape(-1)
    if pred.shape != y.shape:
        raise ValueError(f"pred {pred.shape} != target {y.shape}")
    return _metrics(pred, y)


def _load_session(model_path: Path) -> ort.InferenceSession:
    """Load an ONNX model with external_data, tolerating sibling-rename mismatches.

    Some shipped baseline ONNX (``vmaf_tiny_v1.onnx`` /
    ``vmaf_tiny_v1_medium.onnx``) were renamed from
    ``mlp_small_final.onnx`` / ``mlp_medium_final.onnx`` after export
    without updating their embedded ``external_data.location``. To
    survive that we load the ONNX without external data, manually
    attach the actual sibling ``<stem>.onnx.data`` (which always
    exists next to the ONNX), then hand the materialized in-memory
    proto to ORT as bytes.
    """
    proto = onnx.load(str(model_path), load_external_data=False)
    sibling = model_path.with_suffix(".onnx.data")
    if sibling.is_file():
        for tensor in proto.graph.initializer:
            if tensor.data_location == onnx.TensorProto.EXTERNAL:
                for entry in tensor.external_data:
                    if entry.key == "location":
                        entry.value = sibling.name
        load_external_data_for_model(proto, str(sibling.parent))
    return ort.InferenceSession(proto.SerializeToString())


def _load_clip(data_root: Path, clip: str) -> tuple[np.ndarray, np.ndarray]:
    """Load val_ds for held-out clip; uses on-disk JSON cache when present."""
    print(f"[eval] loading clip={clip}", flush=True)
    t0 = time.monotonic()
    ds = NetflixFrameDataset(
        data_root,
        split="val",
        val_source=clip,
        use_cache=True,
    )
    x, y = ds.numpy_arrays()
    print(f"[eval]   clip={clip} samples={len(y)} in {time.monotonic() - t0:.1f}s", flush=True)
    return x, y


def _parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--data-root",
        type=Path,
        default=Path(
            os.environ.get(
                "VMAF_NETFLIX_CORPUS_DIR",
                str(REPO_ROOT / ".corpus" / "netflix"),
            )
        ),
        help=(
            "Netflix corpus root (ref/ + dis/). Override with the "
            "``VMAF_NETFLIX_CORPUS_DIR`` env var for container or "
            "non-maintainer layouts."
        ),
    )
    ap.add_argument(
        "--loso-dir",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "training_runs" / "loso_mlp_small",
    )
    ap.add_argument(
        "--mlp-small-baseline",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "vmaf_tiny_v1.onnx",
    )
    ap.add_argument(
        "--mlp-medium-baseline",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "vmaf_tiny_v1_medium.onnx",
    )
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "runs" / "loso_eval")
    return ap.parse_args()


def _validate_models(args: argparse.Namespace, fold_models: dict[str, Path]) -> bool:
    missing = [str(p) for p in fold_models.values() if not p.is_file()]
    if missing:
        print("error: missing fold ONNX:\n  " + "\n  ".join(missing), file=sys.stderr)
        return False
    for tag, p in (
        ("mlp_small (baseline)", args.mlp_small_baseline),
        ("mlp_medium (baseline)", args.mlp_medium_baseline),
    ):
        if not p.is_file():
            print(f"error: missing {tag} ONNX: {p}", file=sys.stderr)
            return False
    return True


def _load_clips(data_root: Path) -> dict[str, tuple[np.ndarray, np.ndarray]]:
    return {clip: _load_clip(data_root, clip) for clip in CLIPS}


def _build_report(args: argparse.Namespace, json_out: Path, md_out: Path) -> dict[str, Any]:
    from aiutils.run_manifest import build_run_provenance

    return {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "corpus": str(args.data_root),
        "loso_per_fold": {},
        "loso_aggregate": {},
        "baselines": {},
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=sys.argv,
            args=args,
            inputs={
                "data_root": args.data_root,
                "loso_dir": args.loso_dir,
                "mlp_small_baseline": args.mlp_small_baseline,
                "mlp_medium_baseline": args.mlp_medium_baseline,
            },
            outputs={"json_report": json_out, "markdown_report": md_out},
        ),
    }


def _evaluate_loso(
    report: dict[str, Any],
    fold_models: dict[str, Path],
    clip_xy: dict[str, tuple[np.ndarray, np.ndarray]],
) -> None:
    print("[eval] === LOSO per-fold (each fold's mlp_small on its held-out clip) ===", flush=True)
    plccs: list[float] = []
    sroccs: list[float] = []
    rmses: list[float] = []
    for clip in CLIPS:
        metrics = _eval(_load_session(fold_models[clip]), *clip_xy[clip])
        report["loso_per_fold"][clip] = metrics
        plccs.append(metrics["plcc"])
        sroccs.append(metrics["srocc"])
        rmses.append(metrics["rmse"])
        print(
            f"[eval]   fold={clip:14s} n={metrics['n']:4.0f} "
            f"PLCC={metrics['plcc']:.4f} SROCC={metrics['srocc']:.4f} "
            f"RMSE={metrics['rmse']:.3f}",
            flush=True,
        )
    report["loso_aggregate"] = {
        "mean_plcc": float(np.mean(plccs)),
        "mean_srocc": float(np.mean(sroccs)),
        "mean_rmse": float(np.mean(rmses)),
        "std_plcc": float(np.std(plccs, ddof=1)),
        "std_srocc": float(np.std(sroccs, ddof=1)),
        "std_rmse": float(np.std(rmses, ddof=1)),
    }
    agg = report["loso_aggregate"]
    print(
        f"[eval]   LOSO mean    PLCC={agg['mean_plcc']:.4f} "
        f"SROCC={agg['mean_srocc']:.4f} RMSE={agg['mean_rmse']:.3f}\n"
        f"[eval]   LOSO std     PLCC={agg['std_plcc']:.4f} "
        f"SROCC={agg['std_srocc']:.4f} RMSE={agg['std_rmse']:.3f}",
        flush=True,
    )


def _evaluate_baselines(
    report: dict[str, Any],
    args: argparse.Namespace,
    clip_xy: dict[str, tuple[np.ndarray, np.ndarray]],
) -> None:
    x_all = np.concatenate([clip_xy[clip][0] for clip in CLIPS], axis=0)
    y_all = np.concatenate([clip_xy[clip][1] for clip in CLIPS], axis=0)
    print("[eval] === Baselines (per-clip + all-clips concat) ===", flush=True)
    for tag, path in (
        ("mlp_small_v1", args.mlp_small_baseline),
        ("mlp_medium_v1", args.mlp_medium_baseline),
    ):
        session = _load_session(path)
        per_clip = {clip: _eval(session, *clip_xy[clip]) for clip in CLIPS}
        all_metrics = _eval(session, x_all, y_all)
        report["baselines"][tag] = {
            "model_path": str(path),
            "per_clip": per_clip,
            "all_clips_concat": all_metrics,
        }
        print(
            f"[eval]   baseline={tag:14s} all-concat n={all_metrics['n']:5.0f} "
            f"PLCC={all_metrics['plcc']:.4f} SROCC={all_metrics['srocc']:.4f} "
            f"RMSE={all_metrics['rmse']:.3f}",
            flush=True,
        )


def _markdown(report: dict[str, Any]) -> str:
    lines = [
        "# LOSO evaluation — `mlp_small` on Netflix corpus\n",
        f"Generated: {report['generated']}\n",
        "## Per-fold (each fold's `mlp_small_final.onnx` on its held-out clip)\n",
        "| fold | n | PLCC | SROCC | RMSE |",
        "|---|---:|---:|---:|---:|",
    ]
    for clip in CLIPS:
        metrics = report["loso_per_fold"][clip]
        lines.append(
            f"| {clip} | {metrics['n']} | {metrics['plcc']:.4f} | "
            f"{metrics['srocc']:.4f} | {metrics['rmse']:.3f} |"
        )
    agg = report["loso_aggregate"]
    lines.append(
        f"| **LOSO mean ± std** | — | {agg['mean_plcc']:.4f} ± {agg['std_plcc']:.4f} | "
        f"{agg['mean_srocc']:.4f} ± {agg['std_srocc']:.4f} | "
        f"{agg['mean_rmse']:.3f} ± {agg['std_rmse']:.3f} |\n"
    )
    lines.append("## Baselines (single-split `val=Tennis` models, evaluated on every clip)\n")
    for tag in ("mlp_small_v1", "mlp_medium_v1"):
        baseline = report["baselines"][tag]
        lines.extend(
            [
                f"### `{tag}` ({baseline['model_path']})\n",
                "| split | n | PLCC | SROCC | RMSE |",
                "|---|---:|---:|---:|---:|",
            ]
        )
        for clip in CLIPS:
            metrics = baseline["per_clip"][clip]
            lines.append(
                f"| {clip} | {metrics['n']} | {metrics['plcc']:.4f} | "
                f"{metrics['srocc']:.4f} | {metrics['rmse']:.3f} |"
            )
        metrics = baseline["all_clips_concat"]
        lines.append(
            f"| **all-clips concat** | {metrics['n']} | {metrics['plcc']:.4f} | "
            f"{metrics['srocc']:.4f} | {metrics['rmse']:.3f} |\n"
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    args = _parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    if not args.data_root.is_dir():
        print(f"error: data-root not found: {args.data_root}", file=sys.stderr)
        return 2
    fold_models = {clip: args.loso_dir / f"fold_{clip}" / "mlp_small_final.onnx" for clip in CLIPS}
    if not _validate_models(args, fold_models):
        return 2

    clip_xy = _load_clips(args.data_root)
    json_out = args.out / "loso_mlp_small_eval.json"
    md_out = args.out / "loso_mlp_small_eval.md"
    report = _build_report(args, json_out, md_out)
    _evaluate_loso(report, fold_models, clip_xy)
    _evaluate_baselines(report, args, clip_xy)
    from aiutils.run_manifest import write_manifest_json

    write_manifest_json(json_out, report)
    print(f"[eval] wrote {json_out}", flush=True)
    md_out.write_text(_markdown(report), encoding="utf-8")
    print(f"[eval] wrote {md_out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
