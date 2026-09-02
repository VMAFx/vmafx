#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train ``fr_regressor_v2`` — codec-aware FR regressor (Phase B prereq).

Scaffold-only. Consumes the JSONL corpus emitted by the
``vmaf-tune corpus`` command (Phase A, ADR-0237) and trains a 9-D
input MLP that conditions on encoder / preset / CRF in addition to the
canonical-6 libvmaf feature vector v1 already uses.

Prerequisite: a real corpus run (Phase A), which is currently expensive
(hours per encoder × preset × CRF grid). Until that lands, the
``--smoke`` mode synthesises 100 fake rows so the training pipeline is
end-to-end exercisable without a real corpus. ``--smoke`` is a
documented diagnostic flag — it never ships a quality model and the
exported registry row is tagged ``smoke: true``.

Pipeline:

  1. Load the JSONL corpus (one row per (source, encoder, preset, crf)).
  2. Materialise the 9-D feature vector per row:
       * 6 canonical-6 features (adm2, vif_scale0..3, motion2) — read
         directly from the corpus row's ``<feature>_mean`` columns
         introduced under schema v3 (ADR-0331). Pre-v3 rows fall back
         to the legacy ``per_frame_features`` payload (used by the
         smoke-only synthetic corpus); rows missing both shapes
         materialise to all-zero canonical features and downstream
         consumers should filter on ``schema_version >= 3``.
       * 1 encoder one-hot index (over a closed vocabulary).
       * 1 preset ordinal scaled to [0, 1].
       * 1 CRF normalised to [0, 1] (divide by 63 — the union upper
         bound across encoders supported by ``vmaf-tune``).
  3. Train ``FRRegressor(in_features=6, num_codecs=N_ENCODERS+2)`` —
     the existing class already supports the codec-aware contract via
     ``num_codecs``. We re-use it rather than minting a new class so
     v1 / v2 share the export + ONNX-allowlist plumbing.
  4. Bake the StandardScaler (per-feature mean/std on the 6 canonical
     dims; the 3 codec dims pass through unscaled) into the sidecar
     JSON, mirroring v1's pattern.
  5. Export ONNX (opset 17, dynamic batch) and update
     ``model/tiny/registry.json`` with a ``smoke: true`` row until a
     real corpus run flips the entry.

The smoke mode builds a deterministic synthetic corpus and trains for 1
epoch on 100 fake rows so the pipeline can be validated in CI without
hours of encode time. The output ONNX is a load-path probe, not a
quality model.

See ADR-0272 (codec-aware FR regressor v2 scaffold), ADR-0235
(codec-aware decision), Research-0054 (v2 feasibility).

Reproducer (smoke):

  python ai/scripts/train_fr_regressor_v2.py --smoke

Reproducer (real corpus, once Phase A has produced one):

  python ai/scripts/train_fr_regressor_v2.py \\
         --corpus runs/vmaf_tune_corpus.jsonl \\
         --epochs 30
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np

from aiutils.file_utils import sha256
from aiutils.run_manifest import build_run_provenance, write_manifest_json

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = Path(__file__).resolve()
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))


# Canonical-6 feature subset, identical to v1 so the libvmaf input
# contract stays stable between the two regressor checkpoints.
CANONICAL6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)

# Closed encoder vocabulary. Order is load-bearing — index baked into
# the trained ONNX. Append-only; bump SCHEMA_VERSION to retrain.
ENCODER_VOCAB: tuple[str, ...] = (
    "libx264",
    "libx265",
    "libsvtav1",
    "libvvenc",
    "libvpx-vp9",
    # Hardware encoders (T-VMAF-TUNE Phase A real-corpus runner via
    # `scripts/dev/hw_encoder_corpus.py` — see ADR-0237 + Research-0061).
    # NVIDIA NVENC + Intel Arc QSV. Vocab extension bumps
    # ENCODER_VOCAB_VERSION to 2 — the previous vocab v1 model graphs
    # are forward-compatible (extra one-hot bits are zero-padded for
    # legacy callers) but new training runs target v2.
    "h264_nvenc",
    "hevc_nvenc",
    "av1_nvenc",
    "h264_qsv",
    "hevc_qsv",
    "av1_qsv",
    "unknown",
)
ENCODER_VOCAB_VERSION = 2
# NOTE (ADR-0302 — Proposed): the v3 16-slot schema is documented as
# `ENCODER_VOCAB_V3` below. The live `ENCODER_VOCAB_VERSION` stays at
# 2 until the follow-up retrain PR clears the LOSO PLCC ship gate
# (mean LOSO PLCC >= 0.95 + multi-codec lift >= +0.005 over v1 per
# ADR-0235). When that PR lands, bump this to 3, replace
# `ENCODER_VOCAB` in place with the 16-slot tuple, and remove the
# `ENCODER_VOCAB_V3` parallel constant. See
# docs/adr/0302-encoder-vocab-v3-schema-expansion.md and
# docs/research/0078-encoder-vocab-v3-schema-expansion.md.
N_ENCODERS = len(ENCODER_VOCAB)
UNKNOWN_ENCODER_INDEX = ENCODER_VOCAB.index("unknown")

# Target schema for the v3 retrain (ADR-0302, scaffold only).
#
# This tuple is **information-only** in this PR — no inference path
# consumes it yet. It documents the 16-slot vocab the follow-up
# retrain PR will train against:
#
#   - slots 0..12 mirror the user-facing v2 layout documented in
#     ADR-0291 (libx264, libaom-av1, libx265, h264_nvenc, hevc_nvenc,
#     av1_nvenc, h264_amf, hevc_amf, av1_amf, h264_qsv, hevc_qsv,
#     av1_qsv, libvvenc).
#   - slots 13/14/15 append `libsvtav1`, `h264_videotoolbox`,
#     `hevc_videotoolbox` — three codec adapters that landed on
#     master after `fr_regressor_v2` shipped to production
#     (ADR-0283 for VT, ADR-0294 for SVT-AV1).
#
# Append-only ordering is load-bearing per ADR-0235 — the 13 v2
# indices stay verbatim; never reorder.
ENCODER_VOCAB_V3: tuple[str, ...] = (
    "libx264",
    "libaom-av1",
    "libx265",
    "h264_nvenc",
    "hevc_nvenc",
    "av1_nvenc",
    "h264_amf",
    "hevc_amf",
    "av1_amf",
    "h264_qsv",
    "hevc_qsv",
    "av1_qsv",
    "libvvenc",
    "libsvtav1",
    "h264_videotoolbox",
    "hevc_videotoolbox",
)
assert len(ENCODER_VOCAB_V3) == 16, "ADR-0302 schema invariant: 16 slots"

# Preset ordinal table per encoder. The ordinal is scaled to [0, 1] in
# the feature vector so the MLP sees a continuous "speed knob" rather
# than per-encoder one-hots — the ordinal carries the speed/quality
# trade-off direction consistently across encoders.
PRESET_ORDINAL: dict[str, dict[str, int]] = {
    "libx264": {
        "ultrafast": 0,
        "superfast": 1,
        "veryfast": 2,
        "faster": 3,
        "fast": 4,
        "medium": 5,
        "slow": 6,
        "slower": 7,
        "veryslow": 8,
        "placebo": 9,
    },
    "libx265": {
        "ultrafast": 0,
        "superfast": 1,
        "veryfast": 2,
        "faster": 3,
        "fast": 4,
        "medium": 5,
        "slow": 6,
        "slower": 7,
        "veryslow": 8,
        "placebo": 9,
    },
    # libsvtav1 uses numeric "preset" (0..13). We squash to 0..9.
    "libsvtav1": {str(i): min(i, 9) for i in range(14)},
    # libvvenc presets: faster/fast/medium/slow/slower
    "libvvenc": {"faster": 1, "fast": 3, "medium": 5, "slow": 7, "slower": 8},
    # libvpx-vp9 "deadline" + cpu-used; treat as ordinal best-effort.
    "libvpx-vp9": {"realtime": 0, "good": 5, "best": 9},
    # NVIDIA NVENC presets are p1..p7 (p1=fastest, p7=slowest).
    # Squash 1..7 -> 0..9 ordinal scale to keep PRESET_MAX_ORDINAL=9
    # invariant. p4 (the default for NVENC) maps to 5 (median).
    "h264_nvenc": {"p1": 0, "p2": 2, "p3": 3, "p4": 5, "p5": 6, "p6": 7, "p7": 9},
    "hevc_nvenc": {"p1": 0, "p2": 2, "p3": 3, "p4": 5, "p5": 6, "p6": 7, "p7": 9},
    "av1_nvenc": {"p1": 0, "p2": 2, "p3": 3, "p4": 5, "p5": 6, "p6": 7, "p7": 9},
    # Intel QSV presets share the libx264 vocabulary but only ship
    # veryfast/faster/fast/medium/slow/slower/veryslow on Arc.
    "h264_qsv": {
        "veryfast": 2,
        "faster": 3,
        "fast": 4,
        "medium": 5,
        "slow": 6,
        "slower": 7,
        "veryslow": 8,
    },
    "hevc_qsv": {
        "veryfast": 2,
        "faster": 3,
        "fast": 4,
        "medium": 5,
        "slow": 6,
        "slower": 7,
        "veryslow": 8,
    },
    "av1_qsv": {
        "veryfast": 2,
        "faster": 3,
        "fast": 4,
        "medium": 5,
        "slow": 6,
        "slower": 7,
        "veryslow": 8,
    },
    "unknown": {},
}
PRESET_MAX_ORDINAL = 9.0  # all tables are normalised by this

# Encoder-specific CRF/QP upper bounds. The UNION upper bound is 63 —
# libsvtav1 / libvpx-vp9 max CRF — so /63 is the safe normaliser.
CRF_MAX = 63.0


def _set_seed(seed: int) -> None:
    import torch

    np.random.seed(seed)
    torch.manual_seed(seed)


def _encoder_index(name: str | None) -> int:
    """Map an encoder string to its ENCODER_VOCAB index, falling back to 'unknown'."""
    if name is None:
        return UNKNOWN_ENCODER_INDEX
    name = name.strip().lower()
    if not name:
        return UNKNOWN_ENCODER_INDEX
    aliases = {
        "h264": "libx264",
        "x264": "libx264",
        "hevc": "libx265",
        "x265": "libx265",
        "av1": "libsvtav1",
        "vvc": "libvvenc",
        "vp9": "libvpx-vp9",
    }
    canon = aliases.get(name, name)
    try:
        return ENCODER_VOCAB.index(canon)
    except ValueError:
        return UNKNOWN_ENCODER_INDEX


def _preset_ordinal(encoder: str, preset: str | int) -> float:
    """Return the [0, 1]-scaled preset ordinal for ``(encoder, preset)``."""
    enc = encoder.strip().lower() if encoder else "unknown"
    table = PRESET_ORDINAL.get(enc, {})
    raw = table.get(str(preset), 5)  # default 'medium'-ish
    return float(raw) / PRESET_MAX_ORDINAL


def _encoder_onehot(idx: int) -> np.ndarray:
    v = np.zeros(N_ENCODERS, dtype=np.float32)
    v[idx] = 1.0
    return v


def _row_to_features(
    row: dict, *, warn_missing: bool = True
) -> tuple[np.ndarray, np.ndarray, float]:
    """Materialise one (canonical6, codec_block, target) tuple from a corpus row.

    Returns ``(canonical6 ndarray shape (6,), codec_block ndarray shape (N_ENCODERS+2,),
    target float)``. ``codec_block`` packs ``[onehot(N_ENCODERS), preset_norm, crf_norm]``.

    Schema v3 (ADR-0331) lifts the canonical-6 features into top-level
    ``<feature>_mean`` columns aggregated over the scored frames. v2
    rows pre-date the bump and synthetic smoke rows under the old
    ``per_frame_features`` payload still flow through this path; both
    are honoured. Cells that are NaN (libvmaf didn't expose the
    feature, or the encode failed) collapse to 0.0 in the materialised
    vector — the caller is expected to drop NaN rows upstream when it
    matters; the legacy smoke path relies on the per-frame fallback.
    """
    canon = np.zeros(6, dtype=np.float32)
    have_features = False

    # v3 path: top-level ``<feature>_mean`` columns from the corpus row.
    for i, name in enumerate(CANONICAL6):
        v = row.get(f"{name}_mean")
        if v is None:
            continue
        try:
            fv = float(v)
        except (TypeError, ValueError):
            continue
        if math.isnan(fv):
            continue
        canon[i] = fv
        have_features = True

    # Legacy / synthetic-smoke path: ``per_frame_features`` payload.
    if not have_features:
        pf = row.get("per_frame_features") or {}
        for i, name in enumerate(CANONICAL6):
            if name in pf:
                try:
                    canon[i] = float(pf[name])
                    have_features = True
                except (TypeError, ValueError):
                    pass

    if not have_features and warn_missing:
        # Pre-ADR-0331 corpora carry aggregate ``vmaf_score`` only; the
        # row materialises to all-zero canonical features. Trainers
        # consuming legacy data should either filter on
        # ``schema_version >= 3`` or fall back to ``--synthetic``.
        pass

    enc_idx = _encoder_index(row.get("encoder"))
    preset_norm = _preset_ordinal(str(row.get("encoder", "unknown")), row.get("preset", "medium"))
    crf = row.get("crf", 23)
    crf_norm = float(crf) / CRF_MAX

    codec_block = np.concatenate(
        [
            _encoder_onehot(enc_idx),
            np.asarray([preset_norm, crf_norm], dtype=np.float32),
        ]
    )
    target = float(row.get("vmaf_score", 0.0))
    return canon, codec_block, target


def _load_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rows.append(json.loads(line))
    return rows


def _synth_smoke_corpus(n: int = 100, seed: int = 0) -> list[dict]:
    """Synthesise ``n`` fake corpus rows.

    Generates rows that look like Phase A's schema and embed plausible
    canonical-6 features in ``per_frame_features``. The synthetic VMAF
    target is a deterministic function of CRF so the MLP has a
    learnable signal — the goal is to validate the pipeline, not the
    accuracy.
    """
    rng = np.random.default_rng(seed)
    rows = []
    encoders = ["libx264", "libx265", "libsvtav1", "libvvenc", "libvpx-vp9"]
    presets_for = {
        "libx264": ["fast", "medium", "slow"],
        "libx265": ["fast", "medium", "slow"],
        "libsvtav1": ["4", "8", "10"],
        "libvvenc": ["fast", "medium", "slow"],
        "libvpx-vp9": ["good"],
    }
    for i in range(n):
        enc = encoders[i % len(encoders)]
        preset = presets_for[enc][i % len(presets_for[enc])]
        crf = int(rng.integers(18, 45))
        # Synthetic VMAF: higher CRF -> lower VMAF, with codec offset.
        codec_offset = encoders.index(enc) * 1.5
        target = float(
            np.clip(95.0 - (crf - 18) * 1.7 - codec_offset + rng.normal(0, 1.5), 0.0, 100.0)
        )
        per_frame = {
            "adm2": float(0.9 - (crf - 18) * 0.01 + rng.normal(0, 0.01)),
            "vif_scale0": float(0.6 - (crf - 18) * 0.012 + rng.normal(0, 0.01)),
            "vif_scale1": float(0.7 - (crf - 18) * 0.011 + rng.normal(0, 0.01)),
            "vif_scale2": float(0.8 - (crf - 18) * 0.010 + rng.normal(0, 0.01)),
            "vif_scale3": float(0.85 - (crf - 18) * 0.009 + rng.normal(0, 0.01)),
            "motion2": float(2.0 + rng.normal(0, 0.3)),
        }
        # Emit both the legacy ``per_frame_features`` payload and the
        # schema-v3 ``<feature>_mean`` columns so the synthetic corpus
        # exercises both code paths in ``_row_to_features`` (ADR-0331).
        rows.append(
            {
                "schema_version": 3,
                "encoder": enc,
                "preset": preset,
                "crf": crf,
                "vmaf_score": target,
                "per_frame_features": per_frame,
                "adm2_mean": per_frame["adm2"],
                "vif_scale0_mean": per_frame["vif_scale0"],
                "vif_scale1_mean": per_frame["vif_scale1"],
                "vif_scale2_mean": per_frame["vif_scale2"],
                "vif_scale3_mean": per_frame["vif_scale3"],
                "motion2_mean": per_frame["motion2"],
                "src": f"synth_{i:03d}.yuv",
            }
        )
    return rows


def _materialise(rows: list[dict]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Stack rows into ``(X_canon (N, 6), X_codec (N, N_ENCODERS+2), y (N,))``.

    Rows whose materialised canonical features, codec block, or target are
    not all finite are dropped (with a logged count) rather than collapsed
    to 0.0. A single NaN feature/target cell otherwise silently poisons the
    StandardScaler mean/std and the MSE loss, collapsing the whole fit.
    """
    canons, codecs, ys = [], [], []
    dropped = 0
    for r in rows:
        c, k, y = _row_to_features(r)
        if not (np.isfinite(c).all() and np.isfinite(k).all() and math.isfinite(y)):
            dropped += 1
            continue
        canons.append(c)
        codecs.append(k)
        ys.append(y)
    if dropped:
        print(
            f"[fr-v2] WARNING: dropped {dropped}/{len(rows)} row(s) with "
            f"non-finite features/target before training",
            file=sys.stderr,
            flush=True,
        )
    return (
        np.stack(canons).astype(np.float32),
        np.stack(codecs).astype(np.float32),
        np.asarray(ys, dtype=np.float32),
    )


def _train(
    x_canon: np.ndarray,
    x_codec: np.ndarray,
    y: np.ndarray,
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    seed: int,
    hidden: int,
    depth: int,
):  # type: ignore[no-untyped-def]
    """Train an FRRegressor with codec conditioning. Returns (model, scaler)."""
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    from vmaf_train.models import FRRegressor

    _set_seed(seed)

    # Standardise canonical-6 only; codec block already in [0, 1].
    mean = x_canon.mean(axis=0)
    std = x_canon.std(axis=0, ddof=0)
    std = np.where(std < 1e-8, 1.0, std)
    x_canon_norm = (x_canon - mean) / std

    num_codec_dims = x_codec.shape[1]
    model = FRRegressor(
        in_features=6,
        hidden=hidden,
        depth=depth,
        dropout=0.1,
        lr=lr,
        weight_decay=weight_decay,
        num_codecs=num_codec_dims,
    )

    ds = TensorDataset(
        torch.from_numpy(x_canon_norm.astype(np.float32)),
        torch.from_numpy(x_codec.astype(np.float32)),
        torch.from_numpy(y.astype(np.float32)),
    )
    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, drop_last=False)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=weight_decay)
    loss_fn = torch.nn.MSELoss()

    model.train()
    for ep in range(epochs):
        ep_loss = 0.0
        for xb, kb, yb in loader:
            opt.zero_grad()
            pred = model(xb, kb)
            loss = loss_fn(pred, yb)
            loss.backward()
            opt.step()
            ep_loss += float(loss.item())
        if ep == 0 or (ep + 1) % max(1, epochs // 5) == 0:
            print(f"  epoch {ep + 1}/{epochs} loss={ep_loss / max(1, len(loader)):.4f}", flush=True)

    scaler = {"feature_mean": mean.tolist(), "feature_std": std.tolist()}
    return model, scaler


def _export_onnx_combined(
    model,  # type: ignore[no-untyped-def]
    *,
    num_codec_dims: int,
    onnx_path: Path,
    atol: float = 1e-4,
) -> None:
    """Export the codec-aware FRRegressor to ONNX with two named inputs.

    Mirrors the LPIPS-Sq two-input precedent (ADR-0040 / ADR-0041) so
    libvmaf's ``vmaf_dnn_session_run`` can wire both tensors at inference.
    """
    import onnx
    import onnxruntime as ort
    import torch

    from vmaf_train.op_allowlist import check_graph

    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    model = model.eval()
    dummy_feat = torch.zeros(1, 6, dtype=torch.float32)
    dummy_codec = torch.zeros(1, num_codec_dims, dtype=torch.float32)

    dynamic_axes = {
        "features": {0: "batch"},
        "codec": {0: "batch"},
        "score": {0: "batch"},
    }
    torch.onnx.export(
        model,
        (dummy_feat, dummy_codec),
        str(onnx_path),
        input_names=["features", "codec"],
        output_names=["score"],
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
    ort_out = sess.run(None, {"features": dummy_feat.numpy(), "codec": dummy_codec.numpy()})[0]
    max_abs = float(np.abs(ref - ort_out).max())
    if max_abs > atol:
        raise RuntimeError(f"torch vs onnxruntime drift {max_abs:g} exceeds atol {atol:g}")


def _metrics(pred: np.ndarray, target: np.ndarray) -> dict[str, float]:
    pred = pred.astype(np.float64)
    target = target.astype(np.float64)
    if pred.size < 2 or target.size < 2:
        return {"plcc": float("nan"), "srocc": float("nan"), "rmse": float("nan")}
    plcc = float(np.corrcoef(pred, target)[0, 1])
    rmse = float(np.sqrt(np.mean((pred - target) ** 2)))
    rank_p = np.argsort(np.argsort(pred))
    rank_t = np.argsort(np.argsort(target))
    srocc = float(np.corrcoef(rank_p, rank_t)[0, 1])
    return {"plcc": plcc, "srocc": srocc, "rmse": rmse}


def _write_sidecar_and_registry(
    *,
    onnx_path: Path,
    sidecar_path: Path,
    registry_path: Path,
    scaler: dict,
    in_sample: dict[str, float],
    n_rows: int,
    smoke: bool,
    run_provenance: dict[str, Any],
    notes_extra: str = "",
) -> dict[str, Any]:
    digest = sha256(onnx_path)
    notes = (
        "Tiny FR regressor v2 (codec-aware) — 6 canonical libvmaf features "
        "(adm2, vif_scale0..3, motion2) + 8-D codec block "
        "(6 encoder one-hot + preset_norm + crf_norm) -> VMAF teacher score. "
        "Trained on the vmaf-tune Phase A JSONL corpus (ADR-0237). "
        f"{'SMOKE / placeholder build (synthetic corpus).' if smoke else 'Production checkpoint.'} "
        f"In-sample PLCC={in_sample['plcc']:.4f}. "
        "Exported via ai/scripts/train_fr_regressor_v2.py. See "
        "docs/ai/models/fr_regressor_v2.md + ADR-0272 + ADR-0235."
    )
    if notes_extra:
        notes = notes + " " + notes_extra

    sidecar = {
        "id": "fr_regressor_v2",
        "kind": "fr",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "notes": notes,
        "input_names": ["features", "codec"],
        "output_names": ["score"],
        "feature_order": list(CANONICAL6),
        "feature_mean": scaler["feature_mean"],
        "feature_std": scaler["feature_std"],
        "codec_aware": True,
        "encoder_vocab": list(ENCODER_VOCAB),
        "encoder_vocab_version": ENCODER_VOCAB_VERSION,
        "codec_block_layout": [
            *(f"encoder_onehot[{e}]" for e in ENCODER_VOCAB),
            "preset_norm",
            "crf_norm",
        ],
        "training": {
            "dataset": "vmaf-tune-corpus" if not smoke else "synthetic-smoke",
            "n_rows": n_rows,
            "in_sample_plcc": in_sample["plcc"],
            "in_sample_srocc": in_sample["srocc"],
            "in_sample_rmse": in_sample["rmse"],
            "smoke": smoke,
        },
        "run_provenance": run_provenance,
    }
    write_manifest_json(sidecar_path, sidecar)

    # Update registry — idempotent.
    registry = json.loads(registry_path.read_text())
    models = registry.get("models", [])
    new_entry = {
        "id": "fr_regressor_v2",
        "kind": "fr",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "notes": notes,
        "smoke": smoke,
    }
    models = [m for m in models if m.get("id") != "fr_regressor_v2"]
    models.append(new_entry)
    models.sort(key=lambda e: e.get("id", ""))
    registry["models"] = models
    registry_path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n")
    return sidecar


def main() -> int:
    ap = argparse.ArgumentParser(prog="train_fr_regressor_v2.py")
    ap.add_argument(
        "--corpus",
        type=Path,
        default=None,
        help="Path to a vmaf-tune Phase A JSONL corpus. Mutually exclusive with --smoke.",
    )
    ap.add_argument(
        "--smoke",
        action="store_true",
        help="Synthesise 100 fake corpus rows and train 1 epoch. Pipeline validation only.",
    )
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--weight-decay", type=float, default=1e-5)
    ap.add_argument(
        "--hidden",
        type=int,
        default=16,
        help="MLP hidden width. v2 default 16 (matches the user's 6->16->8->1 spec).",
    )
    ap.add_argument("--depth", type=int, default=2)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument(
        "--out-onnx",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "fr_regressor_v2.onnx",
    )
    ap.add_argument(
        "--out-sidecar",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "fr_regressor_v2.json",
    )
    ap.add_argument(
        "--registry",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "registry.json",
    )
    ap.add_argument(
        "--metrics-out",
        type=Path,
        default=None,
        help="Path for the metrics JSON output. "
        "Defaults to $VMAFX_RUNS_DIR/fr_regressor_v2_metrics.json "
        "(or <repo>/runs/fr_regressor_v2_metrics.json if $VMAFX_RUNS_DIR is unset). "
        "In --smoke mode defaults to a temp file unless explicitly overridden.",
    )
    ap.add_argument(
        "--no-export", action="store_true", help="Skip ONNX export + registry update (dev mode)."
    )
    args = ap.parse_args()

    if args.smoke and args.corpus is not None:
        print("error: --smoke and --corpus are mutually exclusive", file=sys.stderr)
        return 2
    if not args.smoke and args.corpus is None:
        print("error: provide --corpus PATH or use --smoke", file=sys.stderr)
        return 2

    # Resolve --metrics-out: smoke mode writes to a temp file so read-only
    # workspaces (e.g. the container /workspace mount) are not written to.
    # Production mode respects $VMAFX_RUNS_DIR or falls back to <repo>/runs/.
    if args.metrics_out is None:
        if args.smoke:
            args.metrics_out = Path(tempfile.gettempdir()) / "fr_regressor_v2_smoke_metrics.json"
        else:
            _runs_root = Path(os.environ.get("VMAFX_RUNS_DIR", str(REPO_ROOT / "runs")))
            args.metrics_out = _runs_root / "fr_regressor_v2_metrics.json"

    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=sys.argv[1:],
        args=args,
        inputs={
            "corpus": args.corpus if args.corpus is not None else "synthetic-smoke",
        },
        outputs={
            "onnx_target": str(args.out_onnx),
            "sidecar_target": str(args.out_sidecar),
            "registry_target": str(args.registry),
            "metrics_target": str(args.metrics_out),
        },
    )

    if args.smoke:
        print("[fr-v2] SMOKE mode — synthesising 100 fake corpus rows", flush=True)
        rows = _synth_smoke_corpus(n=100, seed=args.seed)
        epochs = 1
    else:
        if not args.corpus.is_file():
            print(f"error: corpus not found at {args.corpus}", file=sys.stderr)
            return 2
        print(f"[fr-v2] loading corpus {args.corpus}", flush=True)
        rows = _load_jsonl(args.corpus)
        epochs = args.epochs

    if not rows:
        print("error: corpus has zero rows", file=sys.stderr)
        return 2

    print(f"[fr-v2] materialising {len(rows)} rows -> 9-D feature space", flush=True)
    x_canon, x_codec, y = _materialise(rows)
    print(
        f"[fr-v2] shapes: canon={x_canon.shape} codec={x_codec.shape} y={y.shape} "
        f"(canonical6={x_canon.shape[1]}, codec_block={x_codec.shape[1]})",
        flush=True,
    )

    t0 = time.time()
    model, scaler = _train(
        x_canon,
        x_codec,
        y,
        epochs=epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        weight_decay=args.weight_decay,
        seed=args.seed,
        hidden=args.hidden,
        depth=args.depth,
    )
    print(f"[fr-v2] training done in {time.time() - t0:.1f}s", flush=True)

    # In-sample sanity prediction.
    import torch

    model.eval()
    with torch.no_grad():
        x_canon_norm = (
            x_canon - np.asarray(scaler["feature_mean"], dtype=np.float32)
        ) / np.asarray(scaler["feature_std"], dtype=np.float32)
        preds = (
            model(
                torch.from_numpy(x_canon_norm.astype(np.float32)),
                torch.from_numpy(x_codec.astype(np.float32)),
            )
            .cpu()
            .numpy()
        )
    in_sample = _metrics(preds, y)
    print(
        f"[fr-v2] in-sample: PLCC={in_sample['plcc']:.4f} "
        f"SROCC={in_sample['srocc']:.4f} RMSE={in_sample['rmse']:.3f}",
        flush=True,
    )

    metrics_out = {
        "feature_subset": "canonical6+codec",
        "canonical6": list(CANONICAL6),
        "encoder_vocab": list(ENCODER_VOCAB),
        "n_rows": len(rows),
        "in_sample": in_sample,
        "epochs": epochs,
        "batch_size": args.batch_size,
        "lr": args.lr,
        "weight_decay": args.weight_decay,
        "seed": args.seed,
        "smoke": args.smoke,
        "run_provenance": run_provenance,
    }
    args.metrics_out.parent.mkdir(parents=True, exist_ok=True)
    write_manifest_json(args.metrics_out, metrics_out)
    print(f"[fr-v2] wrote metrics to {args.metrics_out}")

    if args.no_export:
        print("[fr-v2] --no-export set; skipping ONNX export.")
        return 0

    print(f"[fr-v2] exporting ONNX -> {args.out_onnx}", flush=True)
    _export_onnx_combined(
        model,
        num_codec_dims=x_codec.shape[1],
        onnx_path=args.out_onnx,
    )
    _write_sidecar_and_registry(
        onnx_path=args.out_onnx,
        sidecar_path=args.out_sidecar,
        registry_path=args.registry,
        scaler=scaler,
        in_sample=in_sample,
        n_rows=len(rows),
        smoke=args.smoke,
        run_provenance=run_provenance,
    )
    print(f"[fr-v2] shipped: {args.out_onnx} (sha256={sha256(args.out_onnx)})")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
