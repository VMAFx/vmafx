#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train a deep ensemble of ``fr_regressor_v2`` for probabilistic VMAF.

PR-#354 audit Bucket #18 (top-3 ranked) calls for a *probabilistic* head
on top of the codec-aware ``fr_regressor_v2`` so producers can ask risk-
tolerant questions of the form ``P(VMAF >= 92) >= 0.95`` instead of
"predicted VMAF = 92". This script trains an ensemble of N=5 copies of
the v2 architecture (``FRRegressor(num_codecs=NUM_CODECS)``) under
distinct random seeds and exports each copy as
``model/tiny/fr_regressor_v2_seed<N>.onnx`` plus a manifest sidecar
``fr_regressor_v2_ensemble_v1.json`` that wires them into a single
ensemble identifier.

At inference time the C / Python loader runs all 5 ONNX sessions on
the same ``(features, codec_onehot)`` input and aggregates the 5
scalar outputs into a Gaussian summary ``(mu, sigma)``. The 95 %
prediction interval is ``mu +/- 1.96 * sigma`` (deep-ensemble baseline,
Lakshminarayanan et al. 2017). Optional **split-conformal** calibration
(Vovk / Romano / Lei) replaces the 1.96 multiplier with an empirical
quantile of held-out residuals, giving a guaranteed marginal coverage
``>= 1 - alpha`` regardless of the underlying noise model.

ONNX I/O contract (per copy, mirrors v2 scaffold from PR #347):

    Inputs:
      features:     float32 [N, 6]   canonical-6 libvmaf features
      codec_onehot: float32 [N, NUM_CODECS]  one-hot codec id

    Output:
      score:        float32 [N]      VMAF MOS scalar

The ensemble manifest pins:

  - ``members``: list of N ONNX paths (relative to ``model/tiny/``) +
    sha256s and per-member training seed.
  - ``confidence``: ``{ method: "ensemble" | "ensemble+conformal",
    nominal_coverage: 0.95, conformal_q_residual: <float?>,
    feature_mean / feature_std: list[6] }``.
  - ``ensemble_size``: int.
  - ``codec_vocab`` + ``codec_vocab_version``: pinned alongside.

A query at the API layer (``ai.src.vmaf_train.confidence``) returns
``(mu, sigma, lower_95, upper_95)`` per row.

Smoke reproducer (no real corpus required):

    python ai/scripts/train_fr_regressor_v2_ensemble.py --smoke

Production reproducer (gated on a real Phase A corpus):

    python ai/scripts/train_fr_regressor_v2_ensemble.py \
        --corpus runs/full_features_phase_a.parquet \
        --ensemble-size 5 \
        --conformal-calibration-frac 0.2

The shipped artifact in this PR is the **smoke** output (synthetic
corpus, 1 epoch per member); it is a load-path probe, not a quality
model. Production training is gated on a multi-codec Phase A corpus +
clearing the v2 ship floor (deferred, tracked as backlog item
T7-FR-REGRESSOR-V2-PROBABILISTIC).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

from aiutils.file_utils import sha256  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)


def _set_seed(seed: int) -> None:
    import torch

    np.random.seed(seed)
    torch.manual_seed(seed)


def _synthesize_smoke_corpus(
    n_rows: int = 100,
    n_features: int = 6,
    num_codecs: int = 6,
    seed: int = 1234,
):  # type: ignore[no-untyped-def]
    """Synthesise a tiny smoke corpus that mimics the v2 input shape.

    Returns (features, codec_onehot, vmaf, codec_idx). Values are
    plausible canonical-6 ranges so per-feature standardisation does
    not blow up; VMAF target is a noisy linear combination plus a
    codec-bias term (so ensemble members actually disagree under
    different seeds — flat targets would collapse the variance).
    """
    rng = np.random.default_rng(seed)
    features = np.column_stack(
        [
            rng.uniform(0.4, 1.0, size=n_rows),  # adm2
            rng.uniform(0.2, 0.95, size=n_rows),  # vif_scale0
            rng.uniform(0.3, 0.95, size=n_rows),  # vif_scale1
            rng.uniform(0.4, 0.97, size=n_rows),  # vif_scale2
            rng.uniform(0.5, 0.98, size=n_rows),  # vif_scale3
            rng.uniform(0.0, 30.0, size=n_rows),  # motion2
        ]
    ).astype(np.float32)
    codec_idx = rng.integers(0, num_codecs, size=n_rows)
    codec_onehot = np.eye(num_codecs, dtype=np.float32)[codec_idx]
    # Plausible vmaf targets in [10, 100] with codec-conditional bias.
    codec_bias = np.linspace(-3.0, 3.0, num=num_codecs, dtype=np.float32)
    base = 50.0 + 30.0 * features[:, 0] + 10.0 * features[:, 4]
    target = base + codec_bias[codec_idx] + rng.normal(0.0, 1.5, size=n_rows)
    target = np.clip(target, 0.0, 100.0).astype(np.float32)
    return features, codec_onehot, target, codec_idx


def _train_one_member(
    x_features: np.ndarray,
    x_codec: np.ndarray,
    y: np.ndarray,
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    seed: int,
    num_codecs: int,
):  # type: ignore[no-untyped-def]
    """Train one ``FRRegressor`` ensemble member; return the model."""
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    from vmaf_train.models import FRRegressor

    _set_seed(seed)
    model = FRRegressor(
        in_features=x_features.shape[1],
        hidden=64,
        depth=2,
        dropout=0.1,
        lr=lr,
        weight_decay=weight_decay,
        num_codecs=num_codecs,
    )
    ds = TensorDataset(
        torch.from_numpy(x_features.astype(np.float32)),
        torch.from_numpy(x_codec.astype(np.float32)),
        torch.from_numpy(y.astype(np.float32)),
    )
    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, drop_last=False)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=weight_decay)
    loss_fn = torch.nn.MSELoss()
    model.train()
    for _ep in range(epochs):
        for xb, cb, yb in loader:
            opt.zero_grad()
            pred = model(xb, cb)
            loss = loss_fn(pred, yb)
            loss.backward()
            opt.step()
    model.eval()
    return model


def _predict_member(
    model,  # type: ignore[no-untyped-def]
    x_features: np.ndarray,
    x_codec: np.ndarray,
) -> np.ndarray:
    import torch

    with torch.no_grad():
        out = model(
            torch.from_numpy(x_features.astype(np.float32)),
            torch.from_numpy(x_codec.astype(np.float32)),
        )
    return out.cpu().numpy().reshape(-1)


def _ensemble_stats(member_preds: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """member_preds shape: (M, N) -> mu (N,), sigma (N,) (ddof=1)."""
    mu = member_preds.mean(axis=0)
    sigma = member_preds.std(axis=0, ddof=1) if member_preds.shape[0] >= 2 else np.zeros_like(mu)
    return mu.astype(np.float32), sigma.astype(np.float32)


def _split_conformal_q(
    cal_preds_mu: np.ndarray,
    cal_preds_sigma: np.ndarray,
    cal_targets: np.ndarray,
    alpha: float,
) -> float:
    """Compute the (1 - alpha) split-conformal residual quantile.

    Uses the *standardised* residual ``|y - mu| / max(sigma, eps)``
    (Romano-style normalised conformal); the returned quantile ``q``
    multiplies sigma at inference time so the prediction interval is
    ``mu +/- q * sigma``. Falls back to absolute residuals when the
    ensemble is degenerate (sigma ~ 0). Marginal coverage on exchangeable
    data is ``>= 1 - alpha`` by construction.
    """
    eps = 1e-6
    sigma_safe = np.maximum(cal_preds_sigma, eps)
    if np.all(cal_preds_sigma < eps):
        residuals = np.abs(cal_targets - cal_preds_mu)
    else:
        residuals = np.abs(cal_targets - cal_preds_mu) / sigma_safe
    n = len(residuals)
    # Conformal correction: ceil((n+1)(1-alpha))/n quantile, clipped.
    if n == 0:
        return float("nan")
    rank = int(np.ceil((n + 1) * (1.0 - alpha))) - 1
    rank = max(0, min(n - 1, rank))
    q = float(np.sort(residuals)[rank])
    return q


def _export_member(
    model,  # type: ignore[no-untyped-def]
    *,
    onnx_path: Path,
    num_codecs: int,
) -> str:
    """Export one ensemble member as a two-input ONNX (features + codec).

    Mirrors the LPIPS-Sq two-input export precedent (ADR-0040 /
    ADR-0041). Returns the sha256 digest of the written file.
    """
    import torch

    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    model = model.eval()
    dummy_features = torch.zeros(1, len(CANONICAL_6), dtype=torch.float32)
    dummy_codec = torch.zeros(1, num_codecs, dtype=torch.float32)
    torch.onnx.export(
        model,
        (dummy_features, dummy_codec),
        str(onnx_path),
        input_names=["features", "codec_onehot"],
        output_names=["score"],
        dynamic_axes={
            "features": {0: "batch"},
            "codec_onehot": {0: "batch"},
            "score": {0: "batch"},
        },
        opset_version=17,
    )
    return sha256(onnx_path)


def _build_manifest(
    ensemble_id: str,
    members: list[dict[str, Any]],
    *,
    standardisation: dict[str, list[float]],
    codec_vocab: tuple[str, ...],
    codec_vocab_version: int,
    encoder_vocab: tuple[str, ...],
    nominal_coverage: float,
    conformal_q: float | None,
    smoke: bool,
    eval_metrics: dict[str, Any] | None,
    run_provenance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    method = "ensemble+conformal" if conformal_q is not None else "ensemble"
    # codec_block_dim records the exact width of the codec_onehot ONNX input.
    # Storing it alongside the vocab lets the eval script and the libvmaf C
    # loader cross-check against the live ONNX input shape without having to
    # re-derive it from the vocabulary list, preventing silent dim-mismatch
    # crashes when the vocab and the trained weights drift.
    codec_block_dim = len(encoder_vocab)
    codec_block_layout = [f"encoder_onehot[{e}]" for e in encoder_vocab]
    manifest = {
        "id": ensemble_id,
        "kind": "fr_ensemble",
        "ensemble_size": len(members),
        "members": members,
        "feature_order": list(CANONICAL_6),
        "feature_mean": standardisation["feature_mean"],
        "feature_std": standardisation["feature_std"],
        "codec_vocab": list(codec_vocab),
        "codec_vocab_version": codec_vocab_version,
        "codec_block_dim": codec_block_dim,
        "codec_block_layout": codec_block_layout,
        "confidence": {
            "method": method,
            "nominal_coverage": nominal_coverage,
            "gaussian_z": 1.959963984540054,
            "conformal_q_residual": conformal_q,
        },
        "smoke": smoke,
        "eval": eval_metrics,
    }
    if run_provenance is not None:
        manifest["run_provenance"] = run_provenance
    return manifest


def _update_registry(
    registry_path: Path,
    *,
    ensemble_id: str,
    members: list[dict[str, Any]],
    smoke: bool,
) -> None:
    """Add / replace the ensemble registry row.

    The registry schema only knows scoring kinds (fr / nr / filter), so
    each ensemble *member* is registered as kind=``fr`` with a stable
    id ``<ensemble_id>_seed<N>`` and the manifest sidecar
    (``<ensemble_id>.json``) is the higher-level entry point. This
    keeps `validate_model_registry.py` green without a schema bump.
    The ensemble manifest itself is referenced via the first member's
    ``notes`` field so downstream tooling can discover it.
    """
    registry = json.loads(registry_path.read_text())
    models = registry.get("models", [])
    keep = [m for m in models if not m.get("id", "").startswith(f"{ensemble_id}_seed")]
    keep = [m for m in keep if m.get("id") != ensemble_id]
    for member in members:
        keep.append(
            {
                "id": member["id"],
                "kind": "fr",
                "onnx": member["onnx"],
                "opset": 17,
                "sha256": member["sha256"],
                "smoke": smoke,
                "notes": (
                    f"Ensemble member of {ensemble_id} "
                    f"(seed={member['seed']}). See "
                    f"docs/ai/models/fr_regressor_v2_probabilistic.md "
                    f"and {ensemble_id}.json manifest."
                ),
            }
        )
    keep.sort(key=lambda e: e.get("id", ""))
    registry["models"] = keep
    registry_path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="train_fr_regressor_v2_ensemble.py")
    ap.add_argument(
        "--corpus",
        type=Path,
        default=None,
        help="Path to a Phase A multi-codec parquet (canonical-6 + codec + vmaf). "
        "Required unless --smoke is set.",
    )
    ap.add_argument(
        "--smoke",
        action="store_true",
        help="Synthesize a 100-row corpus + train 1 epoch per member. "
        "Validates the pipeline end-to-end without a real corpus.",
    )
    ap.add_argument("--ensemble-size", type=int, default=5)
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--weight-decay", type=float, default=1e-5)
    ap.add_argument("--base-seed", type=int, default=0)
    ap.add_argument(
        "--conformal-calibration-frac",
        type=float,
        default=0.0,
        help="Fraction of training rows held out for split-conformal "
        "calibration. 0.0 disables conformal (interval = mu +/- 1.96 * sigma).",
    )
    ap.add_argument("--nominal-coverage", type=float, default=0.95)
    ap.add_argument(
        "--ensemble-id",
        type=str,
        default="fr_regressor_v2_ensemble_v1",
    )
    ap.add_argument(
        "--out-dir",
        type=Path,
        default=REPO_ROOT / "model" / "tiny",
    )
    ap.add_argument(
        "--registry",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "registry.json",
    )
    ap.add_argument(
        "--no-export",
        action="store_true",
        help="Skip ONNX export + registry update (dev mode).",
    )
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    args = ap.parse_args(raw_argv)

    if args.ensemble_size < 1:
        print("error: --ensemble-size must be >= 1", file=sys.stderr)
        return 2
    if args.smoke and args.corpus is not None:
        print("warning: --smoke overrides --corpus; using synthetic data", file=sys.stderr)

    from vmaf_train.codec import CODEC_VOCAB, CODEC_VOCAB_VERSION, NUM_CODECS

    epochs = 1 if args.smoke else args.epochs
    print(
        f"[fr-v2-ens] mode={'smoke' if args.smoke else 'production'} "
        f"ensemble_size={args.ensemble_size} epochs/member={epochs} "
        f"num_codecs={NUM_CODECS}",
        flush=True,
    )

    if args.smoke:
        features, codec_onehot, target, _codec_idx = _synthesize_smoke_corpus(
            num_codecs=NUM_CODECS,
            seed=args.base_seed + 1234,
        )
    else:
        if args.corpus is None or not args.corpus.is_file():
            print(
                f"error: --corpus required (got {args.corpus}); "
                "use --smoke for a pipeline-only run.",
                file=sys.stderr,
            )
            return 2
        import pandas as pd

        df = pd.read_parquet(args.corpus)
        missing = [c for c in CANONICAL_6 if c not in df.columns]
        if missing:
            print(f"error: parquet missing canonical-6 columns: {missing}", file=sys.stderr)
            return 2
        if "vmaf" not in df.columns:
            print("error: parquet missing 'vmaf' column", file=sys.stderr)
            return 2
        if "codec" not in df.columns:
            print("error: parquet missing 'codec' column", file=sys.stderr)
            return 2
        from vmaf_train.codec import codec_index

        features = df[list(CANONICAL_6)].to_numpy(dtype=np.float32)
        target = df["vmaf"].to_numpy(dtype=np.float32)
        codec_idx = np.array([codec_index(c) for c in df["codec"].astype(str)], dtype=np.int64)
        codec_onehot = np.eye(NUM_CODECS, dtype=np.float32)[codec_idx]

    # Standardise features (fit on the FULL training set; baked into manifest,
    # so the runtime applies the same (x - mean) / std before inference).
    mean = features.mean(axis=0)
    std = features.std(axis=0, ddof=0)
    std = np.where(std < 1e-8, 1.0, std)
    features_norm = ((features - mean) / std).astype(np.float32)

    # Optional split-conformal calibration split.
    n = features_norm.shape[0]
    cal_frac = args.conformal_calibration_frac
    cal_idx = np.array([], dtype=np.int64)
    train_idx = np.arange(n, dtype=np.int64)
    if cal_frac > 0.0:
        if n < 10:
            print(
                "[fr-v2-ens] warning: corpus too small for conformal split; " "disabling.",
                file=sys.stderr,
            )
            cal_frac = 0.0
        else:
            rng = np.random.default_rng(args.base_seed + 9999)
            perm = rng.permutation(n)
            n_cal = max(1, round(n * cal_frac))
            cal_idx = perm[:n_cal]
            train_idx = perm[n_cal:]

    feat_train = features_norm[train_idx]
    cod_train = codec_onehot[train_idx]
    y_train = target[train_idx]
    feat_cal = features_norm[cal_idx] if len(cal_idx) > 0 else None
    cod_cal = codec_onehot[cal_idx] if len(cal_idx) > 0 else None
    y_cal = target[cal_idx] if len(cal_idx) > 0 else None

    # Train ensemble.
    member_preds_train: list[np.ndarray] = []
    member_preds_cal: list[np.ndarray] = []
    member_records: list[dict[str, Any]] = []
    args.out_dir.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    for k in range(args.ensemble_size):
        seed = args.base_seed + k
        print(
            f"[fr-v2-ens] training member {k + 1}/{args.ensemble_size} (seed={seed}) ...",
            flush=True,
        )
        model = _train_one_member(
            feat_train,
            cod_train,
            y_train,
            epochs=epochs,
            batch_size=args.batch_size,
            lr=args.lr,
            weight_decay=args.weight_decay,
            seed=seed,
            num_codecs=NUM_CODECS,
        )
        member_preds_train.append(_predict_member(model, feat_train, cod_train))
        if feat_cal is not None:
            member_preds_cal.append(_predict_member(model, feat_cal, cod_cal))

        if not args.no_export:
            onnx_name = f"{args.ensemble_id}_seed{k}.onnx"
            onnx_path = args.out_dir / onnx_name
            sha = _export_member(model, onnx_path=onnx_path, num_codecs=NUM_CODECS)
            member_records.append(
                {
                    "id": f"{args.ensemble_id}_seed{k}",
                    "onnx": onnx_name,
                    "seed": seed,
                    "sha256": sha,
                }
            )

    elapsed = time.time() - t0
    member_preds_train_arr = np.vstack(member_preds_train)
    mu_train, sigma_train = _ensemble_stats(member_preds_train_arr)
    train_rmse = float(np.sqrt(np.mean((mu_train - y_train) ** 2)))
    train_plcc = float(np.corrcoef(mu_train, y_train)[0, 1]) if len(mu_train) >= 2 else float("nan")
    mean_sigma = float(sigma_train.mean())

    conformal_q: float | None = None
    cal_summary: dict[str, Any] | None = None
    if feat_cal is not None and len(member_preds_cal) > 0:
        member_preds_cal_arr = np.vstack(member_preds_cal)
        mu_cal, sigma_cal = _ensemble_stats(member_preds_cal_arr)
        alpha = max(1e-6, 1.0 - args.nominal_coverage)
        conformal_q = _split_conformal_q(mu_cal, sigma_cal, y_cal, alpha)
        cal_summary = {
            "n_calibration": len(y_cal),
            "alpha": alpha,
            "nominal_coverage": args.nominal_coverage,
            "conformal_q_residual": conformal_q,
            "mean_sigma_cal": float(sigma_cal.mean()),
        }

    print(
        f"[fr-v2-ens] trained {args.ensemble_size} members in {elapsed:.1f}s; "
        f"in-sample mu PLCC={train_plcc:.4f} RMSE={train_rmse:.3f} "
        f"mean_sigma={mean_sigma:.3f} "
        f"conformal_q={conformal_q if conformal_q is not None else 'n/a'}",
        flush=True,
    )

    if args.no_export:
        print("[fr-v2-ens] --no-export set; skipping ONNX export + registry update.")
        return 0

    eval_metrics: dict[str, Any] = {
        "in_sample_plcc": train_plcc,
        "in_sample_rmse": train_rmse,
        "mean_sigma_train": mean_sigma,
    }
    if cal_summary is not None:
        eval_metrics["calibration"] = cal_summary

    standardisation = {
        "feature_mean": mean.astype(float).tolist(),
        "feature_std": std.astype(float).tolist(),
    }
    manifest_path = args.out_dir / f"{args.ensemble_id}.json"
    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={
            "corpus": args.corpus,
        },
        outputs={
            "manifest": manifest_path,
            "member_onnx": [args.out_dir / member["onnx"] for member in member_records],
            "registry": args.registry,
        },
    )
    manifest = _build_manifest(
        args.ensemble_id,
        member_records,
        standardisation=standardisation,
        codec_vocab=CODEC_VOCAB,
        codec_vocab_version=CODEC_VOCAB_VERSION,
        encoder_vocab=CODEC_VOCAB,
        nominal_coverage=args.nominal_coverage,
        conformal_q=conformal_q,
        smoke=args.smoke,
        eval_metrics=eval_metrics,
        run_provenance=run_provenance,
    )
    write_manifest_json(manifest_path, manifest)
    print(f"[fr-v2-ens] wrote manifest to {manifest_path}")

    _update_registry(
        args.registry,
        ensemble_id=args.ensemble_id,
        members=member_records,
        smoke=args.smoke,
    )
    print(f"[fr-v2-ens] updated registry {args.registry.name} ({len(member_records)} members)")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
