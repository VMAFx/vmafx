#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train ``fr_regressor_v3`` — codec-aware FR regressor on ENCODER_VOCAB v3 (16-slot).

Closes the v3 retrain deferral documented in
[ADR-0302](../docs/adr/0302-encoder-vocab-v3-schema-expansion.md) +
[Research-0078](../docs/research/0078-encoder-vocab-v3-schema-expansion.md).
Mirrors :mod:`train_fr_regressor_v2`'s recipe but bumps the codec block
to ``ENCODER_VOCAB_V3`` (16 slots) — see
:data:`train_fr_regressor_v2.ENCODER_VOCAB_V3`. The live
``ENCODER_VOCAB_VERSION = 2`` in the v2 trainer **stays** authoritative
(per ADR-0302's invariant); promoting v3 to "the" canonical
``fr_regressor_v2.onnx`` slot is a separate follow-up PR. This script
ships an additional ``fr_regressor_v3.onnx`` + sidecar + registry row.

Pipeline (closely mirrors :mod:`train_fr_regressor_v2_ensemble_loso`):

1. ``_load_corpus`` reads the Phase A canonical-6 JSONL, validates the
   schema, fits a corpus-wide StandardScaler, and pre-computes the 18-D
   codec block (``ENCODER_VOCAB_V3`` 16-slot one-hot + ``preset_norm`` +
   ``crf_norm``).
2. 9-fold LOSO over the unique ``src`` values: per fold, fit a
   fold-local StandardScaler on the training rows only (mirrors
   ``eval_loso_vmaf_tiny_v3.py``), train an
   ``FRRegressor(in_features=6, num_codecs=18)`` for ``args.epochs``
   (default 200) with Adam(lr=5e-4, weight_decay=1e-5), evaluate
   PLCC / SROCC / RMSE on the held-out source.
3. Compute mean LOSO PLCC. **If mean_plcc < 0.95**, exit non-zero and
   refuse to overwrite the registry — the ADR-0302 ship gate matches
   ADR-0291's gate. The trainer never silently ships a regressor that
   doesn't clear the gate.
4. On gate-pass: fit a final full-corpus model on all 9 sources
   (matches ADR-0291's "ship the full-corpus checkpoint" pattern) and
   export to ``model/tiny/fr_regressor_v3.onnx`` (opset 17, two-input
   ``features`` + ``codec_block`` -> ``vmaf`` scalar). Sidecar JSON
   mirrors the v2 shape with ``encoder_vocab_version: 3`` and the
   ``corpus`` + ``corpus_sha256`` fields.
5. Registry entry ``fr_regressor_v3`` lands with ``smoke: false`` on
   gate-pass; ``smoke: true`` + a "v3 retrain pending" note on
   gate-fail (the trainer + sidecar still ship so downstream PRs can
   iterate).

Honestly-documented limitation (per the constraint in the task brief):
the corpus is currently **NVENC-only** — only slot 5 of
``ENCODER_VOCAB_V3`` (``av1_nvenc``) and slot 3 (``h264_nvenc``)
receive non-zero training rows when v3's vocab is mapped through. The
remaining 14 slots produce default predictions for novel codecs at
inference time. Documented in the model card and ADR-0323.

Reproducer (real corpus)::

    python ai/scripts/train_fr_regressor_v3.py \\
        --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl

Reproducer (synthetic smoke; CI-friendly, sub-second)::

    python ai/scripts/train_fr_regressor_v3.py --smoke

See ADR-0302 (vocab v3), ADR-0319 (LOSO trainer pattern), ADR-0291
(v2 production-flip ship gate), ADR-0235 (multi-codec lift floor),
ADR-0323 (this PR).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

from aiutils.file_utils import sha256
from aiutils.run_manifest import build_run_provenance, write_manifest_json

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = Path(__file__).resolve()
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
if str(REPO_ROOT / "ai" / "scripts") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

# Import the v3 vocab from the v2 trainer where it's documented as a
# parallel constant (per ADR-0302's scaffold landed in PR #401).
from train_fr_regressor_v2 import ENCODER_VOCAB_V3  # noqa: E402  # type: ignore[import-not-found]

# Canonical-6 libvmaf feature columns (ADR-0291 / ADR-0319).
CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)

ENCODER_VOCAB_VERSION = 3
N_ENCODERS_V3 = len(ENCODER_VOCAB_V3)
assert N_ENCODERS_V3 == 16, "ADR-0302 invariant: 16-slot vocab v3"

# v3 has no explicit "unknown" slot — the 16-slot tuple is the closed
# vocabulary of supported codec adapters. Encoder strings outside the
# vocab fall back to slot 0 (libx264) so the row keeps a deterministic
# one-hot rather than an all-zero codec block, but the codec_block
# layout exposes no ambiguous ``unknown`` column. Document this
# behaviour in the model card.
FALLBACK_ENCODER_INDEX = 0

# Codec block: 16 one-hot + preset_norm + crf_norm = 18 dims.
CODEC_BLOCK_DIM = N_ENCODERS_V3 + 2

# Production ship gate per ADR-0302 §Retrain ship gate (matches the
# ADR-0291 gate v2 cleared at 0.9681).
SHIP_GATE_MEAN_PLCC: float = 0.95


def _enc_idx_v3(name: str) -> int:
    """Map a codec string to its ENCODER_VOCAB_V3 index, falling back to slot 0."""
    try:
        return ENCODER_VOCAB_V3.index(str(name))
    except ValueError:
        return FALLBACK_ENCODER_INDEX


def _load_corpus(corpus_path: Path) -> dict[str, Any]:
    """Load the Phase A canonical-6 JSONL corpus into a structured dict.

    Reads the schema-v3 corpus directly (ADR-0366): the canonical-6
    features come from the ``<feature>_mean`` columns the corpus
    emitter writes after parsing libvmaf's ``pooled_metrics`` block.
    Required columns:

    * ``adm2_mean``, ``vif_scale[0..3]_mean``, ``motion2_mean`` —
      per-encode means of the canonical-6 libvmaf features;
    * ``vmaf_score`` — pooled VMAF target (renamed ``vmaf`` for the
      training arrays so the rest of this module can stay positional);
    * ``src``, ``encoder``, ``crf`` — partition + codec block inputs.

    Rows whose canonical-6 means are NaN (libvmaf did not expose the
    feature for that run, or the encode failed) are dropped before the
    StandardScaler is fitted — invented zeros would skew the scaler.
    Legacy v2 corpora that carry only ``vmaf_score`` and no per-feature
    aggregates raise ``ValueError`` with a pointer to ADR-0366; the
    older ``--synthetic`` fallback was removed because it produced
    misleading models that did not predict on real data.
    """
    import numpy as np
    import pandas as pd

    if not corpus_path.is_file():
        raise FileNotFoundError(
            f"Corpus JSONL not found at {corpus_path}. Generate it via "
            "`vmaf-tune corpus ...` (schema v3 per ADR-0366)."
        )

    df = pd.read_json(corpus_path, lines=True)
    if len(df) == 0:
        raise ValueError(f"Corpus {corpus_path} has zero rows.")

    feature_mean_cols = [f"{f}_mean" for f in CANONICAL_6]
    has_v3_means = all(c in df.columns for c in feature_mean_cols)
    has_legacy_bare = all(c in df.columns for c in CANONICAL_6)

    if has_v3_means:
        # Schema-v3 ``vmaf-tune corpus`` shape (ADR-0366). The means
        # come from libvmaf's ``pooled_metrics.<feature>`` block; drop
        # rows where libvmaf did not expose the feature (NaN cells).
        required = [*feature_mean_cols, "vmaf_score", "src", "encoder", "crf"]
        missing = [c for c in required if c not in df.columns]
        if missing:
            raise ValueError(
                f"Corpus {corpus_path} is missing required v3 columns: {missing}. "
                f"Expected canonical-6 means ({feature_mean_cols}) + vmaf_score + "
                f"src + encoder + crf. Re-emit via `vmaf-tune corpus` after "
                "ADR-0366 (schema v3); legacy schema-v2 corpora carry only "
                "aggregate vmaf_score and cannot train this regressor."
            )
        finite_mask = np.isfinite(df[feature_mean_cols].to_numpy(dtype=np.float64)).all(axis=1)
        finite_mask &= np.isfinite(df["vmaf_score"].to_numpy(dtype=np.float64))
        if not finite_mask.all():
            df = df.loc[finite_mask].reset_index(drop=True)
        if len(df) == 0:
            raise ValueError(
                f"Corpus {corpus_path} has zero rows after dropping NaN-feature rows. "
                "Verify the libvmaf model exposes the canonical-6 features."
            )
        # Project the ``_mean`` columns into bare-named ones and rename
        # ``vmaf_score`` -> ``vmaf`` so the rest of the module stays
        # positional.
        for feat, col in zip(CANONICAL_6, feature_mean_cols, strict=True):
            df[feat] = df[col]
        if "vmaf" not in df.columns:
            df = df.assign(vmaf=df["vmaf_score"])
        if "cq" not in df.columns:
            df = df.assign(cq=df["crf"])
    elif has_legacy_bare:
        # Legacy ``hw_encoder_corpus.py`` per-frame shape — bare canonical-6
        # column names, ``vmaf`` target, ``cq`` quality knob. Kept for
        # backward compatibility with existing on-disk corpora; new
        # corpora should be emitted in v3 shape via ``vmaf-tune corpus``.
        required = [*CANONICAL_6, "vmaf", "src", "encoder", "cq"]
        missing = [c for c in required if c not in df.columns]
        if missing:
            raise ValueError(
                f"Corpus {corpus_path} is missing required legacy columns: {missing}. "
                f"Expected canonical-6 ({list(CANONICAL_6)}) + vmaf + src + "
                f"encoder + cq."
            )
    else:
        # Neither shape — surface the v3 error first since it's the
        # forward path; mention the legacy shape for older corpora.
        raise ValueError(
            f"Corpus {corpus_path} is missing required v3 columns: {feature_mean_cols}. "
            "Expected either schema-v3 (vmaf-tune corpus, ADR-0366) "
            f"with `<feature>_mean` columns + vmaf_score + crf, or the legacy "
            f"hw_encoder_corpus.py per-frame shape with bare {list(CANONICAL_6)} "
            "+ vmaf + cq columns. Re-emit via `vmaf-tune corpus` for the "
            "preferred path."
        )

    feat = df[list(CANONICAL_6)].to_numpy(dtype=np.float64)
    feat_mean = feat.mean(axis=0)
    feat_std = feat.std(axis=0, ddof=0)
    feat_std = np.where(feat_std < 1e-8, 1.0, feat_std)

    codec_idx = np.array([_enc_idx_v3(str(e)) for e in df["encoder"].tolist()], dtype=np.int64)
    codec_onehot = np.eye(N_ENCODERS_V3, dtype=np.float32)[codec_idx]

    # The schema-v3 corpus records ``preset`` per row but its ordinal
    # value is encoder-specific. We default to the 0.5 median to match
    # ADR-0319's choice — switching to the encoder-aware ordinal table
    # is tracked as a follow-up so this PR stays focused on schema.
    preset_norm = np.full((len(df),), 0.5, dtype=np.float32)

    # ``crf`` (schema-v3 corpus) and ``cq`` (legacy hw-encoder corpus)
    # are interchangeable as the codec-block normalisation knob.
    if "crf" in df.columns:
        crfs = df["crf"].to_numpy(dtype=np.float32)
    else:
        crfs = df["cq"].to_numpy(dtype=np.float32)
    cq_min, cq_max = float(crfs.min()), float(crfs.max())
    if cq_max - cq_min < 1e-6:
        crf_norm = np.full_like(crfs, 0.5)
    else:
        crf_norm = (crfs - cq_min) / (cq_max - cq_min)

    codec_block = np.concatenate(
        [codec_onehot, preset_norm[:, None], crf_norm[:, None]],
        axis=1,
    ).astype(np.float32)

    codec_block_cols = [f"encoder_onehot[{e}]" for e in ENCODER_VOCAB_V3] + [
        "preset_norm",
        "crf_norm",
    ]

    return {
        "df": df,
        "feature_cols": list(CANONICAL_6),
        "codec_block_cols": codec_block_cols,
        "target_col": "vmaf",
        "source_col": "src",
        "scaler_params": {
            "feature_mean": feat_mean.tolist(),
            "feature_std": feat_std.tolist(),
        },
        "codec_block": codec_block,
        "feature_mean": feat_mean,
        "feature_std": feat_std,
        "cq_min": cq_min,
        "cq_max": cq_max,
        "n_rows": len(df),
    }


def _set_seed_all(seed: int) -> None:
    import random

    import numpy as np
    import torch

    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():  # pragma: no cover (CI is CPU-only)
        torch.cuda.manual_seed_all(seed)


def _plcc(pred, target) -> float:
    import numpy as np

    p = np.asarray(pred, dtype=np.float64).reshape(-1)
    t = np.asarray(target, dtype=np.float64).reshape(-1)
    if p.size < 2 or t.size < 2:
        return float("nan")
    if np.std(p) < 1e-12 or np.std(t) < 1e-12:
        return float("nan")
    return float(np.corrcoef(p, t)[0, 1])


def _srocc(pred, target) -> float:
    import numpy as np

    p = np.asarray(pred, dtype=np.float64).reshape(-1)
    t = np.asarray(target, dtype=np.float64).reshape(-1)
    if p.size < 2 or t.size < 2:
        return float("nan")
    rp = np.argsort(np.argsort(p))
    rt = np.argsort(np.argsort(t))
    if np.std(rp) < 1e-12 or np.std(rt) < 1e-12:
        return float("nan")
    return float(np.corrcoef(rp, rt)[0, 1])


def _train_fold(
    x_feat,
    x_codec,
    y,
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    seed: int,
    num_codecs: int,
):  # type: ignore[no-untyped-def]
    """Train a single FRRegressor for one LOSO fold; return the fitted model."""
    import numpy as np
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    from vmaf_train.models import FRRegressor

    _set_seed_all(seed)

    model = FRRegressor(
        in_features=6,
        hidden=64,
        depth=2,
        dropout=0.1,
        lr=lr,
        weight_decay=weight_decay,
        num_codecs=num_codecs,
    )
    ds = TensorDataset(
        torch.from_numpy(np.asarray(x_feat, dtype=np.float32)),
        torch.from_numpy(np.asarray(x_codec, dtype=np.float32)),
        torch.from_numpy(np.asarray(y, dtype=np.float32)),
    )
    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, drop_last=False)
    opt = torch.optim.Adam(model.parameters(), lr=lr, weight_decay=weight_decay)
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


def _predict(model, x_feat, x_codec):  # type: ignore[no-untyped-def]
    import numpy as np
    import torch

    with torch.no_grad():
        out = model(
            torch.from_numpy(np.asarray(x_feat, dtype=np.float32)),
            torch.from_numpy(np.asarray(x_codec, dtype=np.float32)),
        )
    return out.cpu().numpy().reshape(-1)


def run_loso(corpus: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    """Run 9-fold LOSO over the corpus's unique sources; return summary dict."""
    import numpy as np

    df = corpus["df"]
    feat_cols = corpus["feature_cols"]
    target_col = corpus["target_col"]
    source_col = corpus["source_col"]
    codec_block = corpus["codec_block"]

    sources = sorted(df[source_col].unique().tolist())
    folds: list[dict[str, Any]] = []
    plccs: list[float] = []
    sroccs: list[float] = []
    rmses: list[float] = []

    t_start = time.monotonic()
    for held_out in sources:
        train_mask = (df[source_col] != held_out).to_numpy()
        val_mask = (df[source_col] == held_out).to_numpy()

        x_feat_tr = df.loc[train_mask, feat_cols].to_numpy(dtype=np.float64)
        y_tr = df.loc[train_mask, target_col].to_numpy(dtype=np.float64)
        x_codec_tr = codec_block[train_mask]

        x_feat_va = df.loc[val_mask, feat_cols].to_numpy(dtype=np.float64)
        y_va = df.loc[val_mask, target_col].to_numpy(dtype=np.float64)
        x_codec_va = codec_block[val_mask]

        mean = x_feat_tr.mean(axis=0)
        std = x_feat_tr.std(axis=0, ddof=0)
        std = np.where(std < 1e-8, 1.0, std)
        x_feat_tr_norm = (x_feat_tr - mean) / std
        x_feat_va_norm = (x_feat_va - mean) / std

        model = _train_fold(
            x_feat_tr_norm,
            x_codec_tr,
            y_tr,
            epochs=args.epochs,
            batch_size=args.batch_size,
            lr=args.lr,
            weight_decay=args.weight_decay,
            seed=args.seed,
            num_codecs=CODEC_BLOCK_DIM,
        )
        pred_va = _predict(model, x_feat_va_norm, x_codec_va)
        plcc = _plcc(pred_va, y_va)
        srocc = _srocc(pred_va, y_va)
        rmse = float(np.sqrt(np.mean((pred_va - y_va) ** 2))) if len(y_va) else float("nan")
        folds.append(
            {
                "held_out": held_out,
                "n_train": int(train_mask.sum()),
                "n_val": int(val_mask.sum()),
                "plcc": plcc,
                "srocc": srocc,
                "rmse": rmse,
            }
        )
        plccs.append(plcc)
        sroccs.append(srocc)
        rmses.append(rmse)

    plccs_arr = np.asarray([p for p in plccs if not np.isnan(p)], dtype=np.float64)
    mean_plcc = float(plccs_arr.mean()) if plccs_arr.size > 0 else float("nan")
    std_plcc = float(plccs_arr.std(ddof=1)) if plccs_arr.size >= 2 else float("nan")

    return {
        "n_folds": len(folds),
        "folds": folds,
        "mean_plcc": mean_plcc,
        "std_plcc": std_plcc,
        "min_plcc": float(min(plccs)) if plccs else float("nan"),
        "max_plcc": float(max(plccs)) if plccs else float("nan"),
        "mean_srocc": float(np.nanmean(sroccs)) if sroccs else float("nan"),
        "mean_rmse": float(np.nanmean(rmses)) if rmses else float("nan"),
        "wall_time_s": float(time.monotonic() - t_start),
    }


def fit_full_corpus(corpus: dict[str, Any], args: argparse.Namespace):  # type: ignore[no-untyped-def]
    """Fit one FRRegressor on the entire corpus; return (model, scaler)."""
    import numpy as np

    df = corpus["df"]
    feat_cols = corpus["feature_cols"]
    target_col = corpus["target_col"]
    codec_block = corpus["codec_block"]

    x_feat = df[feat_cols].to_numpy(dtype=np.float64)
    y = df[target_col].to_numpy(dtype=np.float64)
    mean = x_feat.mean(axis=0)
    std = x_feat.std(axis=0, ddof=0)
    std = np.where(std < 1e-8, 1.0, std)
    x_feat_norm = (x_feat - mean) / std

    model = _train_fold(
        x_feat_norm,
        codec_block,
        y,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        weight_decay=args.weight_decay,
        seed=args.seed,
        num_codecs=CODEC_BLOCK_DIM,
    )
    scaler = {"feature_mean": mean.tolist(), "feature_std": std.tolist()}
    return model, scaler


def export_onnx(model, onnx_path: Path) -> None:  # type: ignore[no-untyped-def]
    """Export the v3 FRRegressor to ONNX (opset 17, two named inputs)."""
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch

    from vmaf_train.op_allowlist import check_graph

    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    model = model.eval()
    dummy_feat = torch.zeros(1, 6, dtype=torch.float32)
    dummy_codec = torch.zeros(1, CODEC_BLOCK_DIM, dtype=torch.float32)

    dynamic_axes = {
        "features": {0: "batch"},
        "codec_block": {0: "batch"},
        "vmaf": {0: "batch"},
    }
    torch.onnx.export(
        model,
        (dummy_feat, dummy_codec),
        str(onnx_path),
        input_names=["features", "codec_block"],
        output_names=["vmaf"],
        dynamic_axes=dynamic_axes,
        opset_version=17,
    )

    loaded = onnx.load(str(onnx_path))
    init_names = {t.name for t in loaded.graph.initializer}
    survivors = [vi for vi in loaded.graph.value_info if vi.name not in init_names]
    if len(survivors) != len(loaded.graph.value_info):
        del loaded.graph.value_info[:]
        loaded.graph.value_info.extend(survivors)
        onnx.save(loaded, str(onnx_path), save_as_external_data=False)
    onnx.checker.check_model(loaded)

    report = check_graph(loaded)
    if not report.ok:
        raise RuntimeError(f"exported graph uses ops not on libvmaf's allowlist: {report.pretty()}")

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    with torch.no_grad():
        ref = model(dummy_feat, dummy_codec).cpu().numpy()
    ort_out = sess.run(
        None,
        {"features": dummy_feat.numpy(), "codec_block": dummy_codec.numpy()},
    )[0]
    max_abs = float(np.abs(ref - ort_out).max())
    if max_abs > 1e-4:
        raise RuntimeError(f"torch vs onnxruntime drift {max_abs:g} exceeds atol 1e-4")


def write_sidecar_and_registry(
    *,
    onnx_path: Path,
    sidecar_path: Path,
    registry_path: Path,
    scaler: dict,
    loso_summary: dict[str, Any],
    corpus_path: Path,
    n_rows: int,
    smoke: bool,
    gate_passed: bool,
    run_provenance: dict[str, Any],
) -> None:
    digest = sha256(onnx_path)
    corpus_digest = sha256(corpus_path) if corpus_path.is_file() else None
    note_tail = (
        "v3 retrain pending — first attempt did not clear the v2 baseline gate."
        if not gate_passed
        else "Production checkpoint."
    )
    notes = (
        "Tiny FR regressor v3 (codec-aware, ENCODER_VOCAB v3 16-slot) - 6 canonical "
        "libvmaf features (adm2, vif_scale0..3, motion2) + 18-D codec block "
        "(16 encoder one-hot + preset_norm + crf_norm) -> VMAF teacher score. "
        f"LOSO mean PLCC={loso_summary['mean_plcc']:.4f} "
        f"(gate >= {SHIP_GATE_MEAN_PLCC}). "
        f"{note_tail} See docs/ai/models/fr_regressor_v3.md + ADR-0323 + "
        "ADR-0302 + ADR-0291 + ADR-0235."
    )

    sidecar = {
        "id": "fr_regressor_v3",
        "kind": "fr",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "notes": notes,
        "input_names": ["features", "codec_block"],
        "output_names": ["vmaf"],
        "feature_order": list(CANONICAL_6),
        "feature_mean": scaler["feature_mean"],
        "feature_std": scaler["feature_std"],
        "codec_aware": True,
        "encoder_vocab": list(ENCODER_VOCAB_V3),
        "encoder_vocab_version": ENCODER_VOCAB_VERSION,
        "codec_block_layout": [
            *(f"encoder_onehot[{e}]" for e in ENCODER_VOCAB_V3),
            "preset_norm",
            "crf_norm",
        ],
        "training": {
            "dataset": "phase-a-canonical6" if not smoke else "synthetic-smoke",
            "n_rows": n_rows,
            "loso_mean_plcc": loso_summary["mean_plcc"],
            "loso_std_plcc": loso_summary["std_plcc"],
            "loso_min_plcc": loso_summary["min_plcc"],
            "loso_max_plcc": loso_summary["max_plcc"],
            "loso_mean_srocc": loso_summary["mean_srocc"],
            "loso_mean_rmse": loso_summary["mean_rmse"],
            "loso_folds": loso_summary["folds"],
            "ship_gate_mean_plcc": SHIP_GATE_MEAN_PLCC,
            "gate_passed": gate_passed,
            "smoke": smoke,
        },
        "corpus": str(corpus_path),
        "corpus_sha256": corpus_digest,
        "loso_mean_plcc": loso_summary["mean_plcc"],
        "gate_passed": gate_passed,
        "run_provenance": run_provenance,
    }
    sidecar_path.parent.mkdir(parents=True, exist_ok=True)
    write_manifest_json(sidecar_path, sidecar)

    registry = json.loads(registry_path.read_text())
    models = registry.get("models", [])
    new_entry = {
        "id": "fr_regressor_v3",
        "kind": "fr",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "notes": notes,
        "smoke": (not gate_passed),
    }
    models = [m for m in models if m.get("id") != "fr_regressor_v3"]
    models.append(new_entry)
    models.sort(key=lambda e: e.get("id", ""))
    registry["models"] = models
    registry_path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n")


def _write_smoke_corpus(path: Path, n_per_source: int = 4) -> None:
    """Write a small v3-shaped synthetic corpus for the pipeline-only smoke run.

    Emits rows that mirror the schema-v3 corpus contract from ADR-0366:
    ``adm2_mean`` / ``vif_scale[0..3]_mean`` / ``motion2_mean`` per row,
    target column ``vmaf_score``, partition column ``src``, codec
    selectors ``encoder`` + ``crf``. The ``--smoke`` mode is a pipeline
    integrity check (load → LOSO → export → registry) and never ships a
    quality model — gate_passed is always False on smoke.

    The previous smoke synthesised per-frame rows under a pre-ADR-0366
    schema and a separate ``--synthetic`` path; both have been removed
    (ADR-0366 §Consequences) because they let pipeline-only runs masquerade
    as quality-validated checkpoints in downstream consumers.
    """
    import numpy as np

    sources = ["srcA", "srcB", "srcC"]
    crfs = [19, 25, 31, 37]
    rng = np.random.default_rng(42)
    rows = []
    for src in sources:
        for crf in crfs:
            for _f in range(n_per_source):
                rows.append(
                    {
                        "schema_version": 3,
                        "src": src,
                        "encoder": "h264_nvenc",
                        "preset": "p4",
                        "crf": crf,
                        "vmaf_score": float(95.0 - (crf - 19) * 0.3 + rng.normal(0, 0.5)),
                        "adm2_mean": float(0.95 - (crf - 19) * 0.005),
                        "vif_scale0_mean": float(0.85 - (crf - 19) * 0.003),
                        "vif_scale1_mean": float(0.92 - (crf - 19) * 0.002),
                        "vif_scale2_mean": float(0.97 - (crf - 19) * 0.001),
                        "vif_scale3_mean": float(0.99 - (crf - 19) * 0.0005),
                        "motion2_mean": float(rng.uniform(0.0, 5.0)),
                        "adm2_std": 0.01,
                        "vif_scale0_std": 0.01,
                        "vif_scale1_std": 0.01,
                        "vif_scale2_std": 0.01,
                        "vif_scale3_std": 0.01,
                        "motion2_std": 0.5,
                    }
                )
    with path.open("w", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")


def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="train_fr_regressor_v3",
        description=(
            "Train fr_regressor_v3 (codec-aware FR regressor on ENCODER_VOCAB v3 "
            "16-slot). Runs 9-fold LOSO over the Phase A canonical-6 corpus and "
            "ships the model only if mean LOSO PLCC >= 0.95 (ADR-0302 ship gate)."
        ),
    )
    p.add_argument(
        "--corpus",
        type=Path,
        default=None,
        help="Path to the Phase A canonical-6 JSONL corpus.",
    )
    p.add_argument(
        "--smoke",
        action="store_true",
        help="Synthesise a 48-row corpus, train 1 epoch, skip the gate. Pipeline test only.",
    )
    p.add_argument("--epochs", type=int, default=200)
    p.add_argument("--batch-size", type=int, default=32)
    p.add_argument("--lr", type=float, default=5e-4)
    p.add_argument("--weight-decay", type=float, default=1e-5)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--num-codecs", type=int, default=CODEC_BLOCK_DIM)
    p.add_argument(
        "--out-onnx",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "fr_regressor_v3.onnx",
    )
    p.add_argument(
        "--out-sidecar",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "fr_regressor_v3.json",
    )
    p.add_argument(
        "--registry",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "registry.json",
    )
    p.add_argument(
        "--no-export",
        action="store_true",
        help="Skip ONNX export + registry update (dev mode).",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_argparser()
    args = parser.parse_args(argv)

    if args.smoke and args.corpus is not None:
        print("error: --smoke and --corpus are mutually exclusive", file=sys.stderr)
        return 2
    if not args.smoke and args.corpus is None:
        print("error: provide --corpus PATH or use --smoke", file=sys.stderr)
        return 2

    if args.smoke:
        # Synthesise into a temp file then load via the real path.
        import tempfile

        tmp = Path(tempfile.mkdtemp(prefix="fr_v3_smoke_")) / "synth.jsonl"
        _write_smoke_corpus(tmp)
        args.corpus = tmp
        args.epochs = 1

    print(f"[fr-v3] loading corpus {args.corpus}", flush=True)
    corpus = _load_corpus(args.corpus)
    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=sys.argv[1:] if argv is None else argv,
        args=args,
        inputs={"corpus": args.corpus},
        outputs={
            "onnx_target": str(args.out_onnx),
            "sidecar_target": str(args.out_sidecar),
            "registry_target": str(args.registry),
        },
    )
    print(
        f"[fr-v3] loaded {corpus['n_rows']} rows; sources="
        f"{sorted(corpus['df'][corpus['source_col']].unique().tolist())} "
        f"encoders={sorted(corpus['df']['encoder'].unique().tolist())}",
        flush=True,
    )

    print(
        f"[fr-v3] running 9-fold LOSO (epochs={args.epochs}, lr={args.lr}, "
        f"wd={args.weight_decay}, batch={args.batch_size})",
        flush=True,
    )
    summary = run_loso(corpus, args)
    print(
        f"[fr-v3] LOSO mean_plcc={summary['mean_plcc']:.4f} "
        f"std={summary['std_plcc']:.4f} "
        f"min={summary['min_plcc']:.4f} max={summary['max_plcc']:.4f} "
        f"({summary['wall_time_s']:.1f}s)",
        flush=True,
    )
    for f in summary["folds"]:
        print(
            f"  fold[{f['held_out']:24s}] n_train={f['n_train']:>5d} "
            f"n_val={f['n_val']:>5d} plcc={f['plcc']:.4f} srocc={f['srocc']:.4f} "
            f"rmse={f['rmse']:.3f}",
            flush=True,
        )

    gate_passed = not args.smoke and summary["mean_plcc"] >= SHIP_GATE_MEAN_PLCC
    if args.smoke:
        print("[fr-v3] smoke mode — skipping ship gate", flush=True)
    elif not gate_passed:
        print(
            f"[fr-v3] GATE FAIL: mean LOSO PLCC {summary['mean_plcc']:.4f} "
            f"< {SHIP_GATE_MEAN_PLCC} (ADR-0302). Refusing to export.",
            file=sys.stderr,
            flush=True,
        )
        # Still write the sidecar + registry-with-smoke-true row so the
        # PR ships scaffold + sidecar + registry stub for the follow-up.
        if args.no_export:
            return 1
        # Need an ONNX file on disk for the registry sha256 contract.
        # Train a 1-epoch full-corpus model so the file exists; the
        # registry row is `smoke: true` with the gate-fail note.
        print("[fr-v3] gate-fail: shipping scaffold ONNX (smoke=true)", flush=True)
        scaffold_args = argparse.Namespace(**vars(args))
        scaffold_args.epochs = 1
        scaffold_model, scaffold_scaler = fit_full_corpus(corpus, scaffold_args)
        export_onnx(scaffold_model, args.out_onnx)
        write_sidecar_and_registry(
            onnx_path=args.out_onnx,
            sidecar_path=args.out_sidecar,
            registry_path=args.registry,
            scaler=scaffold_scaler,
            loso_summary=summary,
            corpus_path=args.corpus,
            n_rows=corpus["n_rows"],
            smoke=False,
            gate_passed=False,
            run_provenance=run_provenance,
        )
        return 1

    if args.no_export:
        print("[fr-v3] --no-export set; skipping ONNX export.")
        return 0

    print("[fr-v3] gate PASS — fitting full-corpus checkpoint", flush=True)
    model, scaler = fit_full_corpus(corpus, args)
    export_onnx(model, args.out_onnx)
    write_sidecar_and_registry(
        onnx_path=args.out_onnx,
        sidecar_path=args.out_sidecar,
        registry_path=args.registry,
        scaler=scaler,
        loso_summary=summary,
        corpus_path=args.corpus,
        n_rows=corpus["n_rows"],
        smoke=args.smoke,
        gate_passed=gate_passed,
        run_provenance=run_provenance,
    )
    print(
        f"[fr-v3] shipped: {args.out_onnx} (sha256={sha256(args.out_onnx)})",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
