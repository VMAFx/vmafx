#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Validate the exported ``vmaf_tiny_v3.onnx`` against ground truth.

Mirrors ``validate_vmaf_tiny_v2.py``. Loads the exported ONNX, runs
inference on a slice of the Netflix full-feature parquet, and reports
PLCC vs the ``vmaf`` ground-truth column. Refuses to exit with 0 if
PLCC < ``--min-plcc`` (default 0.97).

Optionally diffs the v3 prediction against the shipped v2 ONNX on the
same fixture to confirm both models live on the same VMAF scale.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)


def _ort_predict(onnx_path: Path, x: np.ndarray, input_name: str) -> np.ndarray:
    import onnxruntime as ort

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    feed = {input_name: x.astype(np.float32)}
    out = sess.run(None, feed)[0]
    return np.asarray(out).reshape(-1)


def _sample(values: np.ndarray) -> list[float]:
    return [float(value) for value in values[:5]]


def _write_report(
    args: argparse.Namespace,
    raw_argv: list[str],
    *,
    rows: int,
    plcc: float,
    rmse: float,
    predictions: np.ndarray,
    truth: np.ndarray,
    diff: dict[str, float | str] | None,
) -> None:
    payload: dict[str, object] = {
        "model": "vmaf_tiny_v3",
        "rows": rows,
        "min_plcc": args.min_plcc,
        "plcc": plcc,
        "rmse": rmse,
        "gate_pass": plcc >= args.min_plcc,
        "sample_predictions": _sample(predictions),
        "sample_truth": _sample(truth),
    }
    if diff is not None:
        payload["v2_diff"] = diff
    payload["run_provenance"] = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"onnx": args.onnx, "parquet": args.parquet, "v2_onnx": args.v2_onnx},
        outputs={"report": args.out_json},
    )
    write_manifest_json(args.out_json, payload)


def main(argv: list[str] | None = None) -> int:
    ap = make_argument_parser(description=__doc__)
    ap.add_argument("--onnx", type=Path, required=True)
    ap.add_argument(
        "--parquet",
        type=Path,
        required=True,
        help="Netflix full-feature parquet (or any parquet with canonical-6 + vmaf cols).",
    )
    ap.add_argument("--rows", type=int, default=100)
    ap.add_argument("--min-plcc", type=float, default=0.97)
    ap.add_argument("--input-name", default="features")
    ap.add_argument(
        "--v2-onnx",
        type=Path,
        default=None,
        help="Optional v2 ONNX path; if provided, diff v3 vs v2 predictions on the same slice.",
    )
    ap.add_argument("--out-json", type=Path, help="Optional JSON validation report.")
    raw_argv = collect_cli_argv(argv)
    args = ap.parse_args(raw_argv)

    import pandas as pd

    df = pd.read_parquet(args.parquet)
    missing = [c for c in CANONICAL_6 if c not in df.columns]
    if missing:
        print(f"[validate-v3] parquet missing columns: {missing}", file=sys.stderr)
        return 2
    df = df.head(args.rows).reset_index(drop=True)

    x = df[list(CANONICAL_6)].to_numpy(dtype=np.float64)
    y = df["vmaf"].to_numpy(dtype=np.float64)

    print(f"[validate-v3] onnx={args.onnx} rows={len(df)}")
    pred = _ort_predict(args.onnx, x, args.input_name)
    plcc = float(np.corrcoef(pred.astype(np.float64), y)[0, 1])
    rmse = float(np.sqrt(np.mean((pred - y) ** 2)))
    print(f"[validate-v3] PLCC={plcc:.4f}  RMSE={rmse:.3f}")
    print(f"[validate-v3] sample preds: {pred[:5].round(3).tolist()}")
    print(f"[validate-v3] sample truth: {y[:5].round(3).tolist()}")

    if args.v2_onnx is not None and args.v2_onnx.exists():
        try:
            v2_pred = _ort_predict(args.v2_onnx, x, args.input_name)
            delta = pred.astype(np.float64) - v2_pred.astype(np.float64)
            diff = {
                "mean": float(delta.mean()),
                "max_abs": float(np.max(np.abs(delta))),
            }
            print(
                f"[validate-v3] v3-v2 delta: mean={diff['mean']:+.3f} "
                f"max_abs={diff['max_abs']:.3f}"
            )
        except Exception as exc:
            diff = {"error": str(exc)}
            print(f"[validate-v3] v2 diff skipped: {exc}")
    else:
        diff = None

    if args.out_json is not None:
        _write_report(
            args,
            raw_argv,
            rows=len(df),
            plcc=plcc,
            rmse=rmse,
            predictions=pred,
            truth=y,
            diff=diff,
        )

    if plcc < args.min_plcc:
        print(
            f"[validate-v3] FAIL — PLCC {plcc:.4f} < gate {args.min_plcc:.4f}",
            file=sys.stderr,
        )
        return 1
    print(f"[validate-v3] PASS — PLCC {plcc:.4f} >= gate {args.min_plcc:.4f}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
