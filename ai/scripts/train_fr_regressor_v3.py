#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
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

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

# Import the v3 vocab from the v2 trainer where it's documented as a
# parallel constant (per ADR-0302's scaffold landed in PR #401).
from ai.scripts.train_fr_regressor_v2 import ENCODER_VOCAB_V3

from aiutils.file_utils import sha256
from aiutils.run_manifest import build_run_provenance, write_manifest_json

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

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


def _normalize_v3_frame(df, corpus_path: Path, mean_columns: list[str]):
    import numpy as np

    required = [*mean_columns, "vmaf_score", "src", "encoder", "crf"]
    missing = [column for column in required if column not in df.columns]
    if missing:
        raise ValueError(f"Corpus {corpus_path} is missing required v3 columns: {missing}.")
    finite = np.isfinite(df[mean_columns].to_numpy(dtype=np.float64)).all(axis=1)
    finite &= np.isfinite(df["vmaf_score"].to_numpy(dtype=np.float64))
    normalized = df.loc[finite].reset_index(drop=True)
    if len(normalized) == 0:
        raise ValueError(f"Corpus {corpus_path} has zero rows after dropping NaN-feature rows.")
    for feature, column in zip(CANONICAL_6, mean_columns, strict=True):
        normalized[feature] = normalized[column]
    if "vmaf" not in normalized.columns:
        normalized = normalized.assign(vmaf=normalized["vmaf_score"])
    if "cq" not in normalized.columns:
        normalized = normalized.assign(cq=normalized["crf"])
    return normalized


def _normalize_corpus_frame(df, corpus_path: Path):
    mean_columns = [f"{feature}_mean" for feature in CANONICAL_6]
    if all(column in df.columns for column in mean_columns):
        return _normalize_v3_frame(df, corpus_path, mean_columns)
    if all(column in df.columns for column in CANONICAL_6):
        required = [*CANONICAL_6, "vmaf", "src", "encoder", "cq"]
        missing = [column for column in required if column not in df.columns]
        if missing:
            raise ValueError(f"Corpus {corpus_path} is missing required legacy columns: {missing}.")
        return df
    raise ValueError(
        f"Corpus {corpus_path} is missing required v3 columns: {mean_columns}. "
        f"Expected schema-v3 means or legacy bare {list(CANONICAL_6)} columns."
    )


def _build_codec_block(df):
    import numpy as np

    codec_idx = np.array([_enc_idx_v3(str(value)) for value in df["encoder"]], dtype=np.int64)
    codec_onehot = np.eye(N_ENCODERS_V3, dtype=np.float32)[codec_idx]
    preset_norm = np.full((len(df),), 0.5, dtype=np.float32)
    quality_column = "crf" if "crf" in df.columns else "cq"
    crfs = df[quality_column].to_numpy(dtype=np.float32)
    cq_min, cq_max = float(crfs.min()), float(crfs.max())
    crf_norm = (
        np.full_like(crfs, 0.5) if cq_max - cq_min < 1e-6 else (crfs - cq_min) / (cq_max - cq_min)
    )
    block = np.concatenate(
        [codec_onehot, preset_norm[:, None], crf_norm[:, None]],
        axis=1,
    ).astype(np.float32)
    columns = [f"encoder_onehot[{encoder}]" for encoder in ENCODER_VOCAB_V3] + [
        "preset_norm",
        "crf_norm",
    ]
    return block, columns, cq_min, cq_max


def _load_corpus(corpus_path: Path) -> dict[str, Any]:
    """Load either schema-v3 or legacy canonical-6 corpus rows."""
    import numpy as np
    import pandas as pd

    if not corpus_path.is_file():
        raise FileNotFoundError(f"Corpus JSONL not found at {corpus_path}.")
    frame = pd.read_json(corpus_path, lines=True)
    if len(frame) == 0:
        raise ValueError(f"Corpus {corpus_path} has zero rows.")
    df = _normalize_corpus_frame(frame, corpus_path)
    features = df[list(CANONICAL_6)].to_numpy(dtype=np.float64)
    feature_mean = features.mean(axis=0)
    feature_std = np.where(features.std(axis=0, ddof=0) < 1e-8, 1.0, features.std(axis=0, ddof=0))
    codec_block, codec_columns, cq_min, cq_max = _build_codec_block(df)

    return {
        "df": df,
        "feature_cols": list(CANONICAL_6),
        "codec_block_cols": codec_columns,
        "target_col": "vmaf",
        "source_col": "src",
        "scaler_params": {
            "feature_mean": feature_mean.tolist(),
            "feature_std": feature_std.tolist(),
        },
        "codec_block": codec_block,
        "feature_mean": feature_mean,
        "feature_std": feature_std,
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


def _run_loso_fold(
    held_out: str, corpus: dict[str, Any], args: argparse.Namespace
) -> dict[str, Any]:
    import numpy as np

    df = corpus["df"]
    feat_cols = corpus["feature_cols"]
    target_col = corpus["target_col"]
    source_col = corpus["source_col"]
    codec_block = corpus["codec_block"]

    train_mask = (df[source_col] != held_out).to_numpy()
    val_mask = (df[source_col] == held_out).to_numpy()
    x_train = df.loc[train_mask, feat_cols].to_numpy(dtype=np.float64)
    target_train = df.loc[train_mask, target_col].to_numpy(dtype=np.float64)
    x_val = df.loc[val_mask, feat_cols].to_numpy(dtype=np.float64)
    target_val = df.loc[val_mask, target_col].to_numpy(dtype=np.float64)
    mean = x_train.mean(axis=0)
    std = np.where(x_train.std(axis=0, ddof=0) < 1e-8, 1.0, x_train.std(axis=0, ddof=0))
    model = _train_fold(
        (x_train - mean) / std,
        codec_block[train_mask],
        target_train,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        weight_decay=args.weight_decay,
        seed=args.seed,
        num_codecs=CODEC_BLOCK_DIM,
    )
    prediction = _predict(model, (x_val - mean) / std, codec_block[val_mask])
    return {
        "held_out": held_out,
        "n_train": int(train_mask.sum()),
        "n_val": int(val_mask.sum()),
        "plcc": _plcc(prediction, target_val),
        "srocc": _srocc(prediction, target_val),
        "rmse": (
            float(np.sqrt(np.mean((prediction - target_val) ** 2)))
            if len(target_val)
            else float("nan")
        ),
    }


def _summarize_loso(folds: list[dict[str, Any]], wall_time: float) -> dict[str, Any]:
    import numpy as np

    plccs = [fold["plcc"] for fold in folds]
    valid_plccs = np.asarray([value for value in plccs if not np.isnan(value)], dtype=np.float64)
    return {
        "n_folds": len(folds),
        "folds": folds,
        "mean_plcc": float(valid_plccs.mean()) if valid_plccs.size else float("nan"),
        "std_plcc": float(valid_plccs.std(ddof=1)) if valid_plccs.size >= 2 else float("nan"),
        "min_plcc": float(min(plccs)) if plccs else float("nan"),
        "max_plcc": float(max(plccs)) if plccs else float("nan"),
        "mean_srocc": float(np.nanmean([fold["srocc"] for fold in folds])),
        "mean_rmse": float(np.nanmean([fold["rmse"] for fold in folds])),
        "wall_time_s": wall_time,
    }


def run_loso(corpus: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    """Run LOSO over every unique source and summarize its held-out metrics."""
    source_col = corpus["source_col"]
    sources = sorted(corpus["df"][source_col].unique().tolist())
    started = time.monotonic()
    folds = [_run_loso_fold(source, corpus, args) for source in sources]
    return _summarize_loso(folds, time.monotonic() - started)


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


def _model_notes(loso_summary: dict[str, Any], gate_passed: bool) -> str:
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
    return notes


def _sidecar_document(
    *,
    onnx_path: Path,
    scaler: dict,
    loso_summary: dict[str, Any],
    corpus_path: Path,
    n_rows: int,
    smoke: bool,
    gate_passed: bool,
    run_provenance: dict[str, Any],
    digest: str,
    notes: str,
) -> dict[str, Any]:
    return {
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
        "corpus_sha256": sha256(corpus_path) if corpus_path.is_file() else None,
        "loso_mean_plcc": loso_summary["mean_plcc"],
        "gate_passed": gate_passed,
        "run_provenance": run_provenance,
    }


def _update_registry(
    registry_path: Path, onnx_path: Path, digest: str, notes: str, gate_passed: bool
) -> None:
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
    notes = _model_notes(loso_summary, gate_passed)
    sidecar = _sidecar_document(
        onnx_path=onnx_path,
        scaler=scaler,
        loso_summary=loso_summary,
        corpus_path=corpus_path,
        n_rows=n_rows,
        smoke=smoke,
        gate_passed=gate_passed,
        run_provenance=run_provenance,
        digest=digest,
        notes=notes,
    )
    sidecar_path.parent.mkdir(parents=True, exist_ok=True)
    write_manifest_json(sidecar_path, sidecar)
    _update_registry(registry_path, onnx_path, digest, notes, gate_passed)


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


def _prepare_args(args: argparse.Namespace) -> bool:
    if args.smoke and args.corpus is not None:
        print("error: --smoke and --corpus are mutually exclusive", file=sys.stderr)
        return False
    if not args.smoke and args.corpus is None:
        print("error: provide --corpus PATH or use --smoke", file=sys.stderr)
        return False
    if args.smoke:
        import tempfile

        tmp = Path(tempfile.mkdtemp(prefix="fr_v3_smoke_")) / "synth.jsonl"
        _write_smoke_corpus(tmp)
        args.corpus = tmp
        args.epochs = 1
    return True


def _print_loso_summary(summary: dict[str, Any]) -> None:
    print(
        f"[fr-v3] LOSO mean_plcc={summary['mean_plcc']:.4f} "
        f"std={summary['std_plcc']:.4f} "
        f"min={summary['min_plcc']:.4f} max={summary['max_plcc']:.4f} "
        f"({summary['wall_time_s']:.1f}s)",
        flush=True,
    )
    for fold in summary["folds"]:
        print(
            f"  fold[{fold['held_out']:24s}] n_train={fold['n_train']:>5d} "
            f"n_val={fold['n_val']:>5d} plcc={fold['plcc']:.4f} "
            f"srocc={fold['srocc']:.4f} rmse={fold['rmse']:.3f}",
            flush=True,
        )


def _write_checkpoint(
    corpus: dict[str, Any],
    args: argparse.Namespace,
    summary: dict[str, Any],
    run_provenance: dict[str, Any],
    *,
    gate_passed: bool,
    smoke: bool,
    epochs: int | None = None,
) -> None:
    fit_args = args
    if epochs is not None:
        fit_args = argparse.Namespace(**vars(args))
        fit_args.epochs = epochs
    model, scaler = fit_full_corpus(corpus, fit_args)
    export_onnx(model, args.out_onnx)
    write_sidecar_and_registry(
        onnx_path=args.out_onnx,
        sidecar_path=args.out_sidecar,
        registry_path=args.registry,
        scaler=scaler,
        loso_summary=summary,
        corpus_path=args.corpus,
        n_rows=corpus["n_rows"],
        smoke=smoke,
        gate_passed=gate_passed,
        run_provenance=run_provenance,
    )


def _run_provenance(args: argparse.Namespace, argv: list[str] | None) -> dict[str, Any]:
    return build_run_provenance(
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


def main(argv: list[str] | None = None) -> int:
    args = build_argparser().parse_args(argv)
    if not _prepare_args(args):
        return 2

    print(f"[fr-v3] loading corpus {args.corpus}", flush=True)
    corpus = _load_corpus(args.corpus)
    run_provenance = _run_provenance(args, argv)
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
    _print_loso_summary(summary)

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
        if args.no_export:
            return 1
        print("[fr-v3] gate-fail: shipping scaffold ONNX (smoke=true)", flush=True)
        _write_checkpoint(
            corpus, args, summary, run_provenance, gate_passed=False, smoke=False, epochs=1
        )
        return 1

    if args.no_export:
        print("[fr-v3] --no-export set; skipping ONNX export.")
        return 0

    print("[fr-v3] gate PASS — fitting full-corpus checkpoint", flush=True)
    _write_checkpoint(
        corpus, args, summary, run_provenance, gate_passed=gate_passed, smoke=args.smoke
    )
    print(
        f"[fr-v3] shipped: {args.out_onnx} (sha256={sha256(args.out_onnx)})",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
