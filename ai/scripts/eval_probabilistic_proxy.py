#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Evaluate the empirical coverage of a ``fr_regressor_v2`` ensemble.

Companion to ``ai/scripts/train_fr_regressor_v2_ensemble.py``. Loads the
ensemble manifest (``model/tiny/<ensemble_id>.json``) + each member's
ONNX, runs them over a held-out parquet (or a synthesised smoke
corpus), and reports the metrics that matter for a probabilistic
regressor:

  * **Coverage at nominal levels** (50 / 80 / 95 %): the fraction of
    rows whose true VMAF falls inside the predicted interval. A
    well-calibrated probabilistic head matches its nominal coverage
    within sampling error (Lakshminarayanan et al. 2017,
    Romano et al. 2019).
  * **Mean interval width** at each nominal level. Tighter is better,
    *given* coverage is at-or-above nominal — a 100-point-wide interval
    that always covers is useless.
  * **Mean PLCC** of the ensemble's mean prediction vs ground truth
    (matches the v1 / v2 deterministic gate).

Two interval-construction modes are supported and both are reported
side-by-side:

  1. ``ensemble`` — Gaussian assumption, ``mu +/- z(alpha) * sigma``.
  2. ``ensemble+conformal`` — uses the per-manifest
     ``confidence.conformal_q_residual`` (computed by the trainer's
     calibration split). When the manifest has no conformal scalar,
     this row is omitted.

Smoke reproducer (after the trainer's ``--smoke`` run):

    python ai/scripts/eval_probabilistic_proxy.py --smoke

Production reproducer (held-out Phase A parquet):

    python ai/scripts/eval_probabilistic_proxy.py \
        --manifest model/tiny/fr_regressor_v2_ensemble_v1.json \
        --parquet runs/full_features_phase_a_holdout.parquet
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path
from typing import Any

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


# Standard-normal inverse CDF table for the most common nominal coverages.
# Pre-tabulated rather than pulled from scipy so this script stays
# pip-light (matches the rest of ai/scripts).
_Z_TABLE: dict[float, float] = {
    0.50: 0.6744897501960817,
    0.80: 1.2815515655446004,
    0.90: 1.6448536269514722,
    0.95: 1.959963984540054,
    0.99: 2.5758293035489004,
}


def _z_for_coverage(coverage: float) -> float:
    if coverage in _Z_TABLE:
        return _Z_TABLE[coverage]
    # Beasley-Springer-Moro approximation for non-tabulated levels.
    p = 0.5 + 0.5 * coverage
    # Acklam-style rational approximation; good to ~1e-9 in the body.
    a = [
        -3.969683028665376e01,
        2.209460984245205e02,
        -2.759285104469687e02,
        1.383577518672690e02,
        -3.066479806614716e01,
        2.506628277459239e00,
    ]
    b = [
        -5.447609879822406e01,
        1.615858368580409e02,
        -1.556989798598866e02,
        6.680131188771972e01,
        -1.328068155288572e01,
    ]
    c = [
        -7.784894002430293e-03,
        -3.223964580411365e-01,
        -2.400758277161838e00,
        -2.549732539343734e00,
        4.374664141464968e00,
        2.938163982698783e00,
    ]
    d = [
        7.784695709041462e-03,
        3.224671290700398e-01,
        2.445134137142996e00,
        3.754408661907416e00,
    ]
    plow = 0.02425
    phigh = 1 - plow
    if p < plow:
        q = math.sqrt(-2 * math.log(p))
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / (
            (((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1
        )
    if p <= phigh:
        q = p - 0.5
        r = q * q
        return ((((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q) / (
            ((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1
        )
    q = math.sqrt(-2 * math.log(1 - p))
    return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / (
        (((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1
    )


def _load_ensemble(manifest_path: Path):  # type: ignore[no-untyped-def]
    """Load an ensemble manifest + open one ORT session per member.

    Returns ``(manifest, sessions)``.
    """
    import onnxruntime as ort

    manifest = json.loads(manifest_path.read_text())
    members = manifest["members"]
    sessions = []
    for m in members:
        onnx_path = manifest_path.parent / m["onnx"]
        if not onnx_path.is_file():
            raise FileNotFoundError(f"ensemble member ONNX missing: {onnx_path}")
        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        sessions.append(sess)
    return manifest, sessions


def _predict_ensemble(
    sessions: list,  # type: ignore[type-arg]
    features_norm: np.ndarray,
    codec_onehot: np.ndarray,
) -> np.ndarray:
    """Run all members; return shape (M, N) score matrix."""
    rows: list[np.ndarray] = []
    for sess in sessions:
        out = sess.run(
            None,
            {
                "features": features_norm.astype(np.float32),
                "codec_onehot": codec_onehot.astype(np.float32),
            },
        )[0]
        rows.append(np.asarray(out).reshape(-1))
    return np.vstack(rows)


def _coverage_metrics(
    mu: np.ndarray,
    sigma: np.ndarray,
    target: np.ndarray,
    *,
    nominal_levels: tuple[float, ...] = (0.50, 0.80, 0.95),
    conformal_q: float | None = None,
) -> dict[str, Any]:
    """Compute empirical coverage + mean width at each nominal level.

    When ``conformal_q`` is provided, also reports a conformal row that
    uses ``mu +/- conformal_q * sigma`` (Romano-style normalised
    conformal). The conformal row is a fixed-q construction — it does
    not vary by nominal level (the q was calibrated for one alpha at
    train time).
    """
    out: dict[str, Any] = {"gaussian": {}}
    for cov in nominal_levels:
        z = _z_for_coverage(cov)
        lower = mu - z * sigma
        upper = mu + z * sigma
        inside = np.logical_and(target >= lower, target <= upper).astype(np.float64)
        out["gaussian"][f"{int(cov * 100)}"] = {
            "z": z,
            "empirical_coverage": float(inside.mean()),
            "mean_width": float((upper - lower).mean()),
        }
    if conformal_q is not None:
        lower = mu - conformal_q * sigma
        upper = mu + conformal_q * sigma
        inside = np.logical_and(target >= lower, target <= upper).astype(np.float64)
        out["conformal"] = {
            "q": conformal_q,
            "empirical_coverage": float(inside.mean()),
            "mean_width": float((upper - lower).mean()),
        }
    return out


def _synthesize_smoke_corpus(
    n_rows: int = 100,
    num_codecs: int = 6,
    seed: int = 4321,
):  # type: ignore[no-untyped-def]
    """Match the trainer smoke distribution but with a different seed
    so we evaluate on out-of-training rows."""
    rng = np.random.default_rng(seed)
    features = np.column_stack(
        [
            rng.uniform(0.4, 1.0, size=n_rows),
            rng.uniform(0.2, 0.95, size=n_rows),
            rng.uniform(0.3, 0.95, size=n_rows),
            rng.uniform(0.4, 0.97, size=n_rows),
            rng.uniform(0.5, 0.98, size=n_rows),
            rng.uniform(0.0, 30.0, size=n_rows),
        ]
    ).astype(np.float32)
    codec_idx = rng.integers(0, num_codecs, size=n_rows)
    codec_onehot = np.eye(num_codecs, dtype=np.float32)[codec_idx]
    codec_bias = np.linspace(-3.0, 3.0, num=num_codecs, dtype=np.float32)
    base = 50.0 + 30.0 * features[:, 0] + 10.0 * features[:, 4]
    target = base + codec_bias[codec_idx] + rng.normal(0.0, 1.5, size=n_rows)
    target = np.clip(target, 0.0, 100.0).astype(np.float32)
    return features, codec_onehot, target


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(
        prog="eval_probabilistic_proxy.py",
        description=__doc__,
    )
    ap.add_argument(
        "--manifest",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "fr_regressor_v2_ensemble_v1.json",
    )
    ap.add_argument("--parquet", type=Path, default=None)
    ap.add_argument("--smoke", action="store_true")
    ap.add_argument("--metrics-out", type=Path, default=None)
    args = ap.parse_args(raw_argv)

    if not args.manifest.is_file():
        print(f"error: manifest missing at {args.manifest}", file=sys.stderr)
        print(
            "hint: run train_fr_regressor_v2_ensemble.py --smoke first.",
            file=sys.stderr,
        )
        return 2

    manifest, sessions = _load_ensemble(args.manifest)
    feature_mean = np.asarray(manifest["feature_mean"], dtype=np.float32)
    feature_std = np.asarray(manifest["feature_std"], dtype=np.float32)
    conformal_q = manifest.get("confidence", {}).get("conformal_q_residual")

    # Ground truth: read codec dim from the first member's ONNX model.
    # Using the manifest's codec_vocab length would silently mismatch if the
    # vocab list and the trained model weight dimensions drift (e.g. the
    # manifest records 6 codecs but the ONNX was trained with 14). Deriving
    # the dimension from the live ONNX input shape is authoritative.
    codec_input = next((i for i in sessions[0].get_inputs() if i.name == "codec_onehot"), None)
    if codec_input is None:
        print("error: ONNX missing codec_onehot input", file=sys.stderr)
        return 2
    num_codecs = codec_input.shape[1]

    if args.smoke or args.parquet is None:
        features, codec_onehot, target = _synthesize_smoke_corpus(num_codecs=num_codecs)
    else:
        if not args.parquet.is_file():
            print(f"error: parquet missing at {args.parquet}", file=sys.stderr)
            return 2
        import pandas as pd

        df = pd.read_parquet(args.parquet)
        missing = [c for c in CANONICAL_6 if c not in df.columns]
        if missing:
            print(f"error: parquet missing canonical-6 columns: {missing}", file=sys.stderr)
            return 2
        if "vmaf" not in df.columns or "codec" not in df.columns:
            print("error: parquet missing 'vmaf' or 'codec' column", file=sys.stderr)
            return 2

        from vmaf_train.codec import codec_index

        features = df[list(CANONICAL_6)].to_numpy(dtype=np.float32)
        target = df["vmaf"].to_numpy(dtype=np.float32)
        codec_idx = np.array([codec_index(c) for c in df["codec"].astype(str)], dtype=np.int64)
        codec_onehot = np.eye(num_codecs, dtype=np.float32)[codec_idx]

    features_norm = (
        (features - feature_mean) / np.where(feature_std < 1e-8, 1.0, feature_std)
    ).astype(np.float32)

    member_preds = _predict_ensemble(sessions, features_norm, codec_onehot)
    mu = member_preds.mean(axis=0)
    sigma = member_preds.std(axis=0, ddof=1) if member_preds.shape[0] >= 2 else np.zeros_like(mu)

    plcc = float(np.corrcoef(mu, target)[0, 1]) if len(mu) >= 2 else float("nan")
    rmse = float(np.sqrt(np.mean((mu - target) ** 2)))
    coverage = _coverage_metrics(mu, sigma, target, conformal_q=conformal_q)

    report: dict[str, Any] = {
        "manifest": str(args.manifest),
        "ensemble_size": int(member_preds.shape[0]),
        "n_rows": len(target),
        "plcc": plcc,
        "rmse": rmse,
        "mean_sigma": float(sigma.mean()),
        "coverage": coverage,
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"manifest": args.manifest, "parquet": args.parquet},
            outputs={"metrics_out": args.metrics_out},
        ),
    }

    print(json.dumps(report, indent=2))

    if args.metrics_out is not None:
        write_manifest_json(args.metrics_out, report)

    # Sanity: empirical 95% coverage should be in [0, 1].
    g95 = coverage["gaussian"]["95"]["empirical_coverage"]
    if not (0.0 <= g95 <= 1.0):
        print(f"error: implausible coverage {g95}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
