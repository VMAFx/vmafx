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

import os
import sys
import time
from argparse import Namespace
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

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from ai.train.dataset import NetflixFrameDataset  # noqa: E402

# isort: split
from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402

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


def _parse_args(raw_argv: list[str]) -> Namespace:
    ap = make_argument_parser()
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
    return ap.parse_args(raw_argv)


def _resolve_models(args: Namespace) -> tuple[dict[str, Path], str | None]:
    fold_models = {clip: args.loso_dir / f"fold_{clip}" / "mlp_small_final.onnx" for clip in CLIPS}
    missing = [str(p) for p in fold_models.values() if not p.is_file()]
    if missing:
        return fold_models, "missing fold ONNX:\n  " + "\n  ".join(missing)
    for tag, path in (
        ("mlp_small (baseline)", args.mlp_small_baseline),
        ("mlp_medium (baseline)", args.mlp_medium_baseline),
    ):
        if not path.is_file():
            return fold_models, f"missing {tag} ONNX: {path}"
    return fold_models, None


def _load_clips(data_root: Path) -> dict[str, tuple[np.ndarray, np.ndarray]]:
    return {clip: _load_clip(data_root, clip) for clip in CLIPS}


def _evaluate_loso(
    report: dict[str, object],
    fold_models: dict[str, Path],
    clip_xy: dict[str, tuple[np.ndarray, np.ndarray]],
) -> None:
    print("[eval] === LOSO per-fold (each fold's mlp_small on its held-out clip) ===", flush=True)
    plccs, sroccs, rmses = [], [], []
    for clip in CLIPS:
        sess = _load_session(fold_models[clip])
        x, y = clip_xy[clip]
        metrics = _eval(sess, x, y)
        report["loso_per_fold"][clip] = metrics  # type: ignore[index]
        plccs.append(metrics["plcc"])
        sroccs.append(metrics["srocc"])
        rmses.append(metrics["rmse"])
        print(
            f"[eval]   fold={clip:14s} n={metrics['n']:4d} PLCC={metrics['plcc']:.4f} "
            f"SROCC={metrics['srocc']:.4f} RMSE={metrics['rmse']:.3f}",
            flush=True,
        )
    agg = {
        "mean_plcc": float(np.mean(plccs)),
        "mean_srocc": float(np.mean(sroccs)),
        "mean_rmse": float(np.mean(rmses)),
        "std_plcc": float(np.std(plccs, ddof=1)),
        "std_srocc": float(np.std(sroccs, ddof=1)),
        "std_rmse": float(np.std(rmses, ddof=1)),
    }
    report["loso_aggregate"] = agg
    print(
        f"[eval]   LOSO mean    PLCC={agg['mean_plcc']:.4f} SROCC={agg['mean_srocc']:.4f} RMSE={agg['mean_rmse']:.3f}",
        flush=True,
    )
    print(
        f"[eval]   LOSO std     PLCC={agg['std_plcc']:.4f} SROCC={agg['std_srocc']:.4f} RMSE={agg['std_rmse']:.3f}",
        flush=True,
    )


def _evaluate_baselines(
    report: dict[str, object],
    args: Namespace,
    clip_xy: dict[str, tuple[np.ndarray, np.ndarray]],
    x_all: np.ndarray,
    y_all: np.ndarray,
) -> None:
    print("[eval] === Baselines (per-clip + all-clips concat) ===", flush=True)
    for tag, path in (
        ("mlp_small_v1", args.mlp_small_baseline),
        ("mlp_medium_v1", args.mlp_medium_baseline),
    ):
        sess = _load_session(path)
        per_clip = {clip: _eval(sess, *clip_xy[clip]) for clip in CLIPS}
        all_metrics = _eval(sess, x_all, y_all)
        report["baselines"][tag] = {  # type: ignore[index]
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


def _write_markdown(md_out: Path, report: dict[str, object]) -> None:
    with md_out.open("w", encoding="utf-8") as output:
        output.write("# LOSO evaluation — `mlp_small` on Netflix corpus\n\n")
        output.write(f"Generated: {report['generated']}\n\n")
        output.write("## Per-fold (each fold's `mlp_small_final.onnx` on its held-out clip)\n\n")
        output.write("| fold | n | PLCC | SROCC | RMSE |\n|---|---:|---:|---:|---:|\n")
        for clip in CLIPS:
            metrics = report["loso_per_fold"][clip]  # type: ignore[index]
            output.write(
                f"| {clip} | {metrics['n']} | {metrics['plcc']:.4f} | "
                f"{metrics['srocc']:.4f} | {metrics['rmse']:.3f} |\n"
            )
        aggregate = report["loso_aggregate"]
        output.write(
            f"| **LOSO mean ± std** | — | "
            f"{aggregate['mean_plcc']:.4f} ± {aggregate['std_plcc']:.4f} | "  # type: ignore[index]
            f"{aggregate['mean_srocc']:.4f} ± {aggregate['std_srocc']:.4f} | "  # type: ignore[index]
            f"{aggregate['mean_rmse']:.3f} ± {aggregate['std_rmse']:.3f} |\n\n"  # type: ignore[index]
        )
        output.write("## Baselines (single-split `val=Tennis` models, evaluated on every clip)\n\n")
        for tag in ("mlp_small_v1", "mlp_medium_v1"):
            output.write(f"### `{tag}` ({report['baselines'][tag]['model_path']})\n\n")  # type: ignore[index]
            output.write("| split | n | PLCC | SROCC | RMSE |\n|---|---:|---:|---:|---:|\n")
            for clip in CLIPS:
                metrics = report["baselines"][tag]["per_clip"][clip]  # type: ignore[index]
                output.write(
                    f"| {clip} | {metrics['n']} | {metrics['plcc']:.4f} | "
                    f"{metrics['srocc']:.4f} | {metrics['rmse']:.3f} |\n"
                )
            all_metrics = report["baselines"][tag]["all_clips_concat"]  # type: ignore[index]
            output.write(
                f"| **all-clips concat** | {all_metrics['n']} | {all_metrics['plcc']:.4f} | "
                f"{all_metrics['srocc']:.4f} | {all_metrics['rmse']:.3f} |\n\n"
            )


def _run(args: Namespace, raw_argv: list[str]) -> int:

    args.out.mkdir(parents=True, exist_ok=True)

    if not args.data_root.is_dir():
        print(f"error: data-root not found: {args.data_root}", file=sys.stderr)
        return 2

    fold_models, error = _resolve_models(args)
    if error is not None:
        print(f"error: {error}", file=sys.stderr)
        return 2
    clip_xy = _load_clips(args.data_root)

    x_all = np.concatenate([clip_xy[c][0] for c in CLIPS], axis=0)
    y_all = np.concatenate([clip_xy[c][1] for c in CLIPS], axis=0)

    json_out = args.out / "loso_mlp_small_eval.json"
    md_out = args.out / "loso_mlp_small_eval.md"

    from aiutils.run_manifest import build_run_provenance, write_manifest_json

    report: dict[str, object] = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "corpus": str(args.data_root),
        "loso_per_fold": {},
        "loso_aggregate": {},
        "baselines": {},
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
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

    _evaluate_loso(report, fold_models, clip_xy)
    _evaluate_baselines(report, args, clip_xy, x_all, y_all)

    write_manifest_json(json_out, report)
    print(f"[eval] wrote {json_out}", flush=True)

    _write_markdown(md_out, report)
    print(f"[eval] wrote {md_out}", flush=True)
    return 0


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    return _run(_parse_args(raw_argv), raw_argv)


if __name__ == "__main__":
    sys.exit(main())
