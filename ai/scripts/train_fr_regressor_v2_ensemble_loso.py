#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""LOSO trainer for the ``fr_regressor_v2`` deep ensemble (ADR-0303 / ADR-0319).

Companion to:

* ``ai/scripts/train_fr_regressor_v2_ensemble.py`` (PR #372 scaffold —
  trains + exports the per-seed ONNX members against a synthetic smoke
  corpus) and
* ``ai/scripts/eval_probabilistic_proxy.py`` (PR #372 scaffold — a
  smoke evaluator over the ensemble's predictive variance).

This script implements the **production-flip** LOSO protocol per
ADR-0303 / Research-0075: 9-fold leave-one-source-out training over
the Netflix Public Dataset, repeated under five distinct seeds
``{0, 1, 2, 3, 4}``, emitting one ``loso_seed{N}.json`` per seed with
per-fold PLCC / SROCC / RMSE so the production-flip CI gate
(``scripts/ci/ensemble_prod_gate.py``) can decide which seeds are
clear to flip from ``smoke: true`` to ``smoke: false`` in
``model/tiny/registry.json``.

The real loader + per-fold training body land in ADR-0319 (this PR).
The Phase A canonical-6 corpus
(``runs/phase_a/full_grid/per_frame_canonical6.jsonl``) is generated
locally via ``scripts/dev/hw_encoder_corpus.py`` over the 9 Netflix
ref YUVs. Schema per row::

    {"src": "BigBuckBunny_25fps", "encoder": "h264_nvenc", "cq": 19,
     "frame_index": 0, "vmaf": 95.86, "adm2": 0.99,
     "vif_scale0": 0.88, "vif_scale1": 0.99, "vif_scale2": 0.996,
     "vif_scale3": 0.998, "motion2": 0.0, ...}

Usage::

    python ai/scripts/train_fr_regressor_v2_ensemble_loso.py --help

    # Real-corpus invocation:
    python ai/scripts/train_fr_regressor_v2_ensemble_loso.py \\
        --seeds 0,1,2,3,4 \\
        --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl \\
        --out-dir runs/ensemble_loso/

The emitted JSON schema is documented in Research-0075
(``docs/research/0075-fr-regressor-v2-ensemble-prod-flip.md``).
"""

from __future__ import annotations

import argparse
import random
import sys
import time
from pathlib import Path
from typing import Any

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

from aiutils.run_manifest import build_run_provenance, write_manifest_json

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

# 9 Netflix Public Dataset sources — the LOSO folds. Mirrors the order
# baked into ai/scripts/eval_loso_vmaf_tiny_v3.py /
# eval_loso_vmaf_tiny_v5.py / Research-0067 (prod-loso) so the per-seed
# fold ordering is comparable across deterministic vs ensemble runs.
NETFLIX_SOURCES: tuple[str, ...] = (
    "BigBuckBunny_25fps",
    "BirdsInCage_30fps",
    "CrowdRun_25fps",
    "ElFuente1_30fps",
    "ElFuente2_30fps",
    "FoxBird_25fps",
    "OldTownCross_25fps",
    "Seeking_25fps",
    "Tennis_24fps",
)

# Canonical-6 libvmaf feature columns consumed by FRRegressor (ADR-0291).
CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)

# 12-slot ENCODER_VOCAB v2 (ADR-0291). The order is load-bearing — it
# matches the one-hot column index baked into the trained
# ``fr_regressor_v2_ensemble_v1_seed{N}.onnx`` graphs. Mirrors
# ``ai/scripts/train_fr_regressor_v2.py::ENCODER_VOCAB`` (5 SW + 6 HW
# + ``unknown``); duplicated here as a constant rather than imported so
# the trainer stays self-contained for argparse-only smoke runs that
# never touch torch.
ENCODER_VOCAB: tuple[str, ...] = (
    "libx264",
    "libx265",
    "libsvtav1",
    "libvvenc",
    "libvpx-vp9",
    "h264_nvenc",
    "hevc_nvenc",
    "av1_nvenc",
    "h264_qsv",
    "hevc_qsv",
    "av1_qsv",
    "unknown",
)
ENCODER_VOCAB_VERSION = 2
N_ENCODERS = len(ENCODER_VOCAB)
UNKNOWN_ENCODER_INDEX = ENCODER_VOCAB.index("unknown")
assert N_ENCODERS == 12, "ADR-0291 / ADR-0303 invariant: 12-slot vocab v2"

# Codec block layout: [encoder_onehot[0..11], preset_norm, crf_norm].
# Mirrors ``train_fr_regressor_v2.py``'s 14-D codec block. The Phase A
# canonical-6 corpus (``hw_encoder_corpus.py``) does NOT record preset
# explicitly — every NVENC/QSV row was encoded against the encoder
# default preset. We materialise ``preset_norm = 0.5`` (median of the
# 0..9 ordinal range) so the column carries a deterministic value;
# downstream consumers that care about preset-conditioning would need
# a corpus regen with explicit preset metadata.
CODEC_BLOCK_DIM = N_ENCODERS + 2  # 12 + preset_norm + crf_norm

# Production ship gate per ADR-0303. Recorded here as constants so the
# trainer's emitted JSON carries the gate values it was trained
# against — the CI gate consumes the JSON, not these constants
# directly, but co-locating the values makes drift auditable.
SHIP_GATE_MEAN_PLCC: float = 0.95
SHIP_GATE_PLCC_SPREAD_MAX: float = 0.005


def _parse_seed_list(raw: str) -> list[int]:
    """Parse a comma-separated seed list (e.g. ``"0,1,2,3,4"``) into ints."""
    out: list[int] = []
    for raw_token in raw.split(","):
        token = raw_token.strip()
        if not token:
            continue
        out.append(int(token))
    if not out:
        raise argparse.ArgumentTypeError(
            "--seeds must be a non-empty comma-separated list of ints (e.g. --seeds 0,1,2,3,4)"
        )
    return out


def _add_data_arguments(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--seeds",
        type=_parse_seed_list,
        default=[0, 1, 2, 3, 4],
        help=(
            "Comma-separated seeds to train (default: 0,1,2,3,4 — the "
            "five ensemble members in model/tiny/registry.json)."
        ),
    )
    p.add_argument(
        "--corpus",
        type=Path,
        default=Path("runs/phase_a/full_grid/per_frame_canonical6.jsonl"),
        help=(
            "Path to the Phase A canonical-6 per-frame JSONL corpus "
            "(generated locally via scripts/dev/hw_encoder_corpus.py). "
            "Defaults to the canonical location."
        ),
    )
    p.add_argument(
        "--out-dir",
        type=Path,
        default=Path("runs/ensemble_loso"),
        help="Output directory for loso_seed{N}.json artefacts.",
    )


def _add_training_arguments(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--epochs",
        type=int,
        default=200,
        help="Per-fold training epochs (default 200; matches ADR-0291).",
    )
    p.add_argument(
        "--batch-size",
        type=int,
        default=32,
        help="Mini-batch size (default 32; matches ADR-0291).",
    )
    p.add_argument(
        "--lr",
        type=float,
        default=5e-4,
        help="Adam learning rate (default 5e-4; matches ADR-0291).",
    )
    p.add_argument(
        "--weight-decay",
        type=float,
        default=1e-5,
        help="Adam weight decay (default 1e-5; matches ADR-0291).",
    )
    p.add_argument(
        "--num-codecs",
        type=int,
        default=CODEC_BLOCK_DIM,
        help=(
            "Width of the codec block fed alongside canonical-6 features "
            f"(default {CODEC_BLOCK_DIM} = {N_ENCODERS}-slot ENCODER_VOCAB v2 "
            "one-hot + preset_norm + crf_norm; matches train_fr_regressor_v2.py)."
        ),
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help=(
            "Run a 1-epoch sanity training per seed and emit a "
            "schema-shaped loso_seed{N}.json marked note='dry-run'. "
            "Useful for CI smoke."
        ),
    )


def build_argparser() -> argparse.ArgumentParser:
    """Build the CLI argparser. Exposed as a function so tests can import it."""
    parser = argparse.ArgumentParser(
        prog="train_fr_regressor_v2_ensemble_loso",
        description=(
            "9-fold LOSO trainer for fr_regressor_v2 deep ensemble seeds "
            "(ADR-0303 / ADR-0319). Emits loso_seed{N}.json per seed; the "
            "CI gate scripts/ci/ensemble_prod_gate.py consumes the JSONs."
        ),
    )
    _add_data_arguments(parser)
    _add_training_arguments(parser)
    return parser


def _validate_corpus_frame(df, corpus_path: Path) -> None:
    required = [*CANONICAL_6, "vmaf", "src", "encoder", "cq", "frame_index"]
    missing = [column for column in required if column not in df.columns]
    if missing:
        raise ValueError(
            f"Corpus {corpus_path} is missing required columns: {missing}. "
            f"Expected canonical-6 ({list(CANONICAL_6)}) + vmaf + src + "
            f"encoder + cq + frame_index."
        )
    if len(df) == 0:
        raise ValueError(f"Corpus {corpus_path} has zero rows.")


def _codec_block(df):
    import numpy as np

    def _enc_idx(enc: str) -> int:
        try:
            return ENCODER_VOCAB.index(enc)
        except ValueError:
            return UNKNOWN_ENCODER_INDEX

    codec_idx = np.array([_enc_idx(str(value)) for value in df["encoder"]], dtype=np.int64)
    codec_onehot = np.eye(N_ENCODERS, dtype=np.float32)[codec_idx]
    preset_norm = np.full((len(df),), 0.5, dtype=np.float32)
    cqs = df["cq"].to_numpy(dtype=np.float32)
    cq_min, cq_max = float(cqs.min()), float(cqs.max())
    crf_norm = (
        np.full_like(cqs, 0.5) if cq_max - cq_min < 1e-6 else (cqs - cq_min) / (cq_max - cq_min)
    )
    block = np.concatenate(
        [codec_onehot, preset_norm[:, None], crf_norm[:, None]],
        axis=1,
    ).astype(np.float32)
    columns = [f"encoder_onehot[{encoder}]" for encoder in ENCODER_VOCAB] + [
        "preset_norm",
        "crf_norm",
    ]
    return block, columns, cq_min, cq_max


def _load_corpus(corpus_path: Path) -> dict[str, Any]:
    """Load and validate the Phase A canonical-6 JSONL corpus."""
    import numpy as np
    import pandas as pd

    if not corpus_path.is_file():
        raise FileNotFoundError(
            f"Corpus JSONL not found at {corpus_path}. Generate it via "
            "scripts/dev/hw_encoder_corpus.py over the 9 Netflix ref YUVs."
        )
    df = pd.read_json(corpus_path, lines=True)
    _validate_corpus_frame(df, corpus_path)
    features = df[list(CANONICAL_6)].to_numpy(dtype=np.float64)
    feature_mean = features.mean(axis=0)
    feature_std = np.where(features.std(axis=0, ddof=0) < 1e-8, 1.0, features.std(axis=0, ddof=0))
    codec_block, codec_columns, cq_min, cq_max = _codec_block(df)

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
        "codec_block": codec_block,  # (N, CODEC_BLOCK_DIM)
        "feature_mean": feature_mean,
        "feature_std": feature_std,
        "cq_min": cq_min,
        "cq_max": cq_max,
        "n_rows": len(df),
    }


def _set_seed_all(seed: int) -> None:
    """Set torch + numpy + python.random seeds for fold-level determinism."""
    import numpy as np
    import torch

    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    if torch.cuda.is_available():  # pragma: no cover (CI runs CPU)
        torch.cuda.manual_seed_all(seed)


def _plcc(pred, target) -> float:
    """Pearson PLCC. Returns NaN for n<2 or constant inputs."""
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


def _train_one_fold(
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


def _predict_fold(model, x_feat, x_codec):  # type: ignore[no-untyped-def]
    import numpy as np
    import torch

    with torch.no_grad():
        out = model(
            torch.from_numpy(np.asarray(x_feat, dtype=np.float32)),
            torch.from_numpy(np.asarray(x_codec, dtype=np.float32)),
        )
    return out.cpu().numpy().reshape(-1)


def _run_fold(
    held_out: str,
    corpus: dict[str, Any],
    args: argparse.Namespace,
    seed: int,
    epochs: int,
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
    y_train = df.loc[train_mask, target_col].to_numpy(dtype=np.float64)
    x_val = df.loc[val_mask, feat_cols].to_numpy(dtype=np.float64)
    y_val = df.loc[val_mask, target_col].to_numpy(dtype=np.float64)
    mean = x_train.mean(axis=0)
    std = np.where(x_train.std(axis=0, ddof=0) < 1e-8, 1.0, x_train.std(axis=0, ddof=0))
    model = _train_one_fold(
        (x_train - mean) / std,
        codec_block[train_mask],
        y_train,
        epochs=epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        weight_decay=args.weight_decay,
        seed=seed,
        num_codecs=args.num_codecs,
    )
    prediction = _predict_fold(model, (x_val - mean) / std, codec_block[val_mask])
    return {
        "held_out": held_out,
        "n_train": int(train_mask.sum()),
        "n_val": int(val_mask.sum()),
        "plcc": _plcc(prediction, y_val),
        "srocc": _srocc(prediction, y_val),
        "rmse": float(np.sqrt(np.mean((prediction - y_val) ** 2))) if len(y_val) else float("nan"),
    }


def _seed_summary(
    seed: int,
    folds: list[dict[str, Any]],
    args: argparse.Namespace,
    epochs: int,
    wall_time: float,
) -> dict[str, Any]:
    import numpy as np

    plccs = [fold["plcc"] for fold in folds]
    valid_plccs = np.asarray([value for value in plccs if not np.isnan(value)], dtype=np.float64)
    summary = {
        "seed": seed,
        "corpus": str(args.corpus),
        "n_folds": len(folds),
        "folds": folds,
        "mean_plcc": float(valid_plccs.mean()) if valid_plccs.size else float("nan"),
        "std_plcc": float(valid_plccs.std(ddof=1)) if valid_plccs.size >= 2 else float("nan"),
        "min_plcc": float(min(plccs)) if plccs else float("nan"),
        "max_plcc": float(max(plccs)) if plccs else float("nan"),
        "mean_srocc": float(np.nanmean([fold["srocc"] for fold in folds])),
        "mean_rmse": float(np.nanmean([fold["rmse"] for fold in folds])),
        "epochs": epochs,
        "batch_size": args.batch_size,
        "lr": args.lr,
        "weight_decay": args.weight_decay,
        "num_codecs": args.num_codecs,
        "encoder_vocab_version": ENCODER_VOCAB_VERSION,
        "wall_time_s": wall_time,
    }
    if args.dry_run:
        summary["note"] = "dry-run"
    return summary


def _train_one_seed(
    seed: int,
    corpus: dict[str, Any],
    args: argparse.Namespace,
) -> dict[str, Any]:
    """Run source-held-out folds for one ensemble seed."""
    _set_seed_all(seed)
    source_col = corpus["source_col"]
    sources = sorted(corpus["df"][source_col].unique().tolist())
    epochs = 1 if args.dry_run else args.epochs
    started = time.monotonic()
    folds = [_run_fold(source, corpus, args, seed, epochs) for source in sources]
    return _seed_summary(seed, folds, args, epochs, time.monotonic() - started)


def main(argv: list[str] | None = None) -> int:
    parser = build_argparser()
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    args = parser.parse_args(raw_argv)

    print(
        f"[ensemble-loso] seeds={args.seeds} corpus={args.corpus} "
        f"out_dir={args.out_dir} epochs={args.epochs} "
        f"dry_run={args.dry_run}",
        flush=True,
    )

    if not args.corpus.exists():
        print(
            f"[ensemble-loso] corpus not present at {args.corpus} — "
            f"generate it via scripts/dev/hw_encoder_corpus.py first "
            f"(see docs/ai/ensemble-v2-real-corpus-retrain-runbook.md "
            f"§Step 0). Refusing to run.",
            file=sys.stderr,
            flush=True,
        )
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)

    corpus = _load_corpus(args.corpus)
    print(
        f"[ensemble-loso] loaded {corpus['n_rows']} rows; "
        f"sources={sorted(corpus['df'][corpus['source_col']].unique().tolist())} "
        f"encoders={sorted(corpus['df']['encoder'].unique().tolist())}",
        flush=True,
    )

    for seed in args.seeds:
        t0 = time.monotonic()
        summary = _train_one_seed(seed, corpus, args)
        out_path = args.out_dir / f"loso_seed{seed}.json"
        summary["run_provenance"] = build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"corpus": args.corpus},
            outputs={"report_target": str(out_path)},
        )
        write_manifest_json(out_path, summary)
        print(
            f"[ensemble-loso] wrote {out_path} "
            f"mean_plcc={summary['mean_plcc']:.4f} "
            f"spread={summary['max_plcc'] - summary['min_plcc']:.4f} "
            f"({time.monotonic() - t0:.1f}s)",
            flush=True,
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
