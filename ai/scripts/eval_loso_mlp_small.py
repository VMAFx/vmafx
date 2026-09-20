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

import numpy as np
import onnx
import onnxruntime as ort
from onnx.external_data_helper import load_external_data_for_model
from scipy.stats import pearsonr, spearmanr

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

from ai.train.dataset import NetflixFrameDataset

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

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


def _validate_inputs(args: argparse.Namespace) -> dict[str, Path] | None:
    if not args.data_root.is_dir():
        print(f"error: data-root not found: {args.data_root}", file=sys.stderr)
        return None
    fold_models = {clip: args.loso_dir / f"fold_{clip}" / "mlp_small_final.onnx" for clip in CLIPS}
    missing = [str(p) for p in fold_models.values() if not p.is_file()]
    if missing:
        print("error: missing fold ONNX:\n  " + "\n  ".join(missing), file=sys.stderr)
        return None
    for tag, path in (
        ("mlp_small (baseline)", args.mlp_small_baseline),
        ("mlp_medium (baseline)", args.mlp_medium_baseline),
    ):
        if not path.is_file():
            print(f"error: missing {tag} ONNX: {path}", file=sys.stderr)
            return None
    return fold_models


def _eval_loso(
    fold_models: dict[str, Path], clip_xy: dict[str, tuple[np.ndarray, np.ndarray]]
) -> tuple[dict[str, dict[str, float]], dict[str, float]]:
    print("[eval] === LOSO per-fold (each fold's mlp_small on its held-out clip) ===", flush=True)
    per_fold: dict[str, dict[str, float]] = {}
    for clip in CLIPS:
        metrics = _eval(_load_session(fold_models[clip]), *clip_xy[clip])
        per_fold[clip] = metrics
        print(
            f"[eval]   fold={clip:14s} n={metrics['n']:4d} "
            f"PLCC={metrics['plcc']:.4f} SROCC={metrics['srocc']:.4f} "
            f"RMSE={metrics['rmse']:.3f}",
            flush=True,
        )
    aggregate = {
        f"{stat}_{metric}": (
            float(getattr(np, stat)([row[metric] for row in per_fold.values()], ddof=1))
            if stat == "std"
            else float(np.mean([row[metric] for row in per_fold.values()]))
        )
        for stat in ("mean", "std")
        for metric in ("plcc", "srocc", "rmse")
    }
    print(
        f"[eval]   LOSO mean    PLCC={aggregate['mean_plcc']:.4f} "
        f"SROCC={aggregate['mean_srocc']:.4f} RMSE={aggregate['mean_rmse']:.3f}\n"
        f"[eval]   LOSO std     PLCC={aggregate['std_plcc']:.4f} "
        f"SROCC={aggregate['std_srocc']:.4f} RMSE={aggregate['std_rmse']:.3f}",
        flush=True,
    )
    return per_fold, aggregate


def _eval_baselines(
    args: argparse.Namespace, clip_xy: dict[str, tuple[np.ndarray, np.ndarray]]
) -> dict[str, object]:
    x_all = np.concatenate([clip_xy[clip][0] for clip in CLIPS], axis=0)
    y_all = np.concatenate([clip_xy[clip][1] for clip in CLIPS], axis=0)
    baselines: dict[str, object] = {}
    print("[eval] === Baselines (per-clip + all-clips concat) ===", flush=True)
    for tag, path in (
        ("mlp_small_v1", args.mlp_small_baseline),
        ("mlp_medium_v1", args.mlp_medium_baseline),
    ):
        session = _load_session(path)
        per_clip = {clip: _eval(session, *clip_xy[clip]) for clip in CLIPS}
        all_metrics = _eval(session, x_all, y_all)
        baselines[tag] = {
            "model_path": str(path),
            "per_clip": per_clip,
            "all_clips_concat": all_metrics,
        }
        print(
            f"[eval]   baseline={tag:14s} all-concat n={all_metrics['n']:5d} "
            f"PLCC={all_metrics['plcc']:.4f} SROCC={all_metrics['srocc']:.4f} "
            f"RMSE={all_metrics['rmse']:.3f}",
            flush=True,
        )
    return baselines


def _new_report(args: argparse.Namespace, json_out: Path, md_out: Path) -> dict[str, object]:
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


def _write_markdown(report: dict[str, object], md_out: Path) -> None:
    with md_out.open("w", encoding="utf-8") as f:
        f.write("# LOSO evaluation — `mlp_small` on Netflix corpus\n\n")
        f.write(f"Generated: {report['generated']}\n\n")
        f.write("## Per-fold (each fold's `mlp_small_final.onnx` on its held-out clip)\n\n")
        f.write("| fold | n | PLCC | SROCC | RMSE |\n|---|---:|---:|---:|---:|\n")
        for clip in CLIPS:
            m = report["loso_per_fold"][clip]  # type: ignore[index]
            f.write(
                f"| {clip} | {m['n']} | {m['plcc']:.4f} | {m['srocc']:.4f} | {m['rmse']:.3f} |\n"
            )
        a = report["loso_aggregate"]
        f.write(
            f"| **LOSO mean ± std** | — | "
            f"{a['mean_plcc']:.4f} ± {a['std_plcc']:.4f} | "  # type: ignore[index]
            f"{a['mean_srocc']:.4f} ± {a['std_srocc']:.4f} | "  # type: ignore[index]
            f"{a['mean_rmse']:.3f} ± {a['std_rmse']:.3f} |\n\n"  # type: ignore[index]
        )
        f.write("## Baselines (single-split `val=Tennis` models, evaluated on every clip)\n\n")
        for tag in ("mlp_small_v1", "mlp_medium_v1"):
            f.write(f"### `{tag}` ({report['baselines'][tag]['model_path']})\n\n")  # type: ignore[index]
            f.write("| split | n | PLCC | SROCC | RMSE |\n|---|---:|---:|---:|---:|\n")
            for clip in CLIPS:
                m = report["baselines"][tag]["per_clip"][clip]  # type: ignore[index]
                f.write(
                    f"| {clip} | {m['n']} | {m['plcc']:.4f} | {m['srocc']:.4f} | {m['rmse']:.3f} |\n"
                )
            ac = report["baselines"][tag]["all_clips_concat"]  # type: ignore[index]
            f.write(
                f"| **all-clips concat** | {ac['n']} | {ac['plcc']:.4f} | {ac['srocc']:.4f} | {ac['rmse']:.3f} |\n\n"
            )


def main() -> int:
    args = _parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    fold_models = _validate_inputs(args)
    if fold_models is None:
        return 2
    clip_xy = {clip: _load_clip(args.data_root, clip) for clip in CLIPS}
    json_out = args.out / "loso_mlp_small_eval.json"
    md_out = args.out / "loso_mlp_small_eval.md"
    report = _new_report(args, json_out, md_out)
    report["loso_per_fold"], report["loso_aggregate"] = _eval_loso(fold_models, clip_xy)
    report["baselines"] = _eval_baselines(args, clip_xy)
    from aiutils.run_manifest import write_manifest_json

    write_manifest_json(json_out, report)
    print(f"[eval] wrote {json_out}", flush=True)
    _write_markdown(report, md_out)
    print(f"[eval] wrote {md_out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
