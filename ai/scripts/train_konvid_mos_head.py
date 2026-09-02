#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train the KonViD MOS head v1 — Phase 3 of ADR-0325.

Phases 1 + 2 of ADR-0325 land the KonViD-1k / KonViD-150k corpora as
JSONL drops under ``.workingdir2/konvid-{1k,150k}/`` (PRs #440 / #447).
Phase 3 — this script — trains a small MLP that maps the canonical-6
libvmaf features + saliency mean/var + 3 TransNet shot-metadata
columns + a UGC-mixed encoder one-hot to a scalar MOS prediction in
``[1.0, 5.0]`` (subjective MOS units).

Why a separate MOS head, when the fork already ships
``fr_regressor_v2_ensemble`` (VMAF prediction)? Because the
ChatGPT-vision audit (Research-0086) flagged that the fork's
predictors all target *VMAF*, not raw subjective MOS — so we cannot
honestly claim the fork "predicts human MOS" without a head trained
against subjective ratings. KonViD ships ≥5 crowdworker MOS ratings
per clip, which is exactly the subjective ground truth the head
needs.

ONNX I/O contract::

    Inputs:
      features: float32 [N, F]    F depends on the selected feature schema.
                                  KonViD defaults to the 11-D
                                  canonical-6 + saliency + shot layout;
                                  CHUG HDR defaults to a 34-D wide schema
                                  with temporal aggregates and HDR metadata,
                                  or a 45-D display-aware schema when a
                                  target display profile is supplied.
      encoder_onehot: float32 [N, 1]  ENCODER_VOCAB v4 single slot
                                      (always [1.0]) — placeholder for
                                      future multi-slot expansion.
    Output:
      mos: float32 [N]            predicted MOS in [1.0, 5.0]

Reproducer (smoke — no real corpus on disk; deterministic seed)::

    python ai/scripts/train_konvid_mos_head.py --smoke

Production (real KonViD JSONL drops)::

python ai/scripts/train_konvid_mos_head.py \
    --konvid-1k .workingdir2/konvid-1k/konvid_1k.jsonl \
    --konvid-150k .workingdir2/konvid-150k/konvid_150k.jsonl

Full-feature parquet runs must already carry ``mos`` or ``mos_raw_0_100``.
Use ``ai/scripts/materialize_mos_labels.py`` to join MOS labels before
training. A real run that loads zero labelled rows fails instead of falling
back to synthetic data; ``--smoke`` is the explicit synthetic path.

CHUG HDR MOS training uses ``train_chug_hdr_mos_head.py``. That wrapper
reuses this module's training loop but keeps the command name, defaults,
and emitted manifest identity CHUG-specific.

Production-flip gate (mirrors ADR-0303 / fr_regressor_v2_ensemble):

* Mean PLCC ≥ 0.85 across 5 folds
* SROCC ≥ 0.82
* RMSE ≤ 0.45 MOS units
* Max-min spread across folds ≤ 0.005

Per the user direction (memory ``feedback_no_test_weakening``) and
ADR-0325 §Production-flip gate: a real-corpus miss does **not** lower
the threshold — the head ships ``Status: Proposed`` instead with the
gate verdict recorded in the model card.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from aiutils.file_utils import sha256
from aiutils.run_manifest import build_run_provenance, describe_path, write_manifest_json

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = Path(__file__).resolve()

# ---------------------------------------------------------------------
# Constants — the KonViD schema is predictor-facing and must stay in sync with
# the model card and ``tools/vmaf-tune/src/vmaftune/predictor.py``. CHUG HDR
# schemas are local training schemas until a validated HDR model ships.
# ---------------------------------------------------------------------

CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)

# Saliency + TransNet shot-metadata columns. The 3 shot-metadata fields
# are the ones #477's TransNet ingestion bolts onto the corpus rows;
# when the corpus row does not carry them (Phase 1/2 KonViD JSONL was
# spec'd before #477), we default-fill with content-independent zeros.
EXTRA_FEATURES: tuple[str, ...] = (
    "saliency_mean",
    "saliency_var",
    "shot_count_norm",  # log10(1 + shot_count) / 3.0  -> ~[0, 1]
    "shot_mean_len_norm",  # mean shot length in seconds / 30.0 -> ~[0, 1]
    "shot_cut_density",  # cuts per frame -> ~[0, 0.1]
)

FEATURE_COLUMNS: tuple[str, ...] = CANONICAL_6 + EXTRA_FEATURES
N_FEATURES = len(FEATURE_COLUMNS)
DEFAULT_MODEL_ID = "konvid_mos_head_v1"
FEATURE_SCHEMA_KONVID_V1 = "konvid-v1"

# CHUG feature shards carry richer full-reference aggregates than the original
# KonViD MOS-head surface: per-feature p10/p90/std temporal summaries plus
# HDR ladder / geometry metadata.  Keep this as a separate schema so the
# committed KonViD head remains byte-compatible while local CHUG experiments can
# actually use the data the extractor produced.
CHUG_HDR_TEMPORAL_FEATURES: tuple[str, ...] = tuple(
    f"{name}_{stat}" for name in CANONICAL_6 for stat in ("p10", "p90", "std")
)
CHUG_HDR_METADATA_FEATURES: tuple[str, ...] = (
    "chug_bitrate_mbps",
    "chug_orientation_portrait",
    "chug_ref",
    "distorted_width_norm",
    "distorted_height_norm",
    "reference_width_norm",
    "reference_height_norm",
    "duration_s_norm",
    "n_feature_frames_norm",
    "feature_bitdepth_norm",
)
CHUG_HDR_FEATURE_COLUMNS: tuple[str, ...] = (
    CANONICAL_6 + CHUG_HDR_TEMPORAL_FEATURES + CHUG_HDR_METADATA_FEATURES
)
FEATURE_SCHEMA_CHUG_HDR_WIDE_V1 = "chug-hdr-wide-v1"

CHUG_HDR_DISPLAY_FEATURES: tuple[str, ...] = (
    "display_peak_luminance_nits_norm",
    "display_black_luminance_nits_norm",
    "display_contrast_ratio_log_norm",
    "display_ambient_lux_norm",
    "display_bt2020_coverage",
    "display_p3_coverage",
    "display_panel_oled",
    "display_panel_qled",
    "display_panel_lcd",
    "display_local_dimming",
    "display_tone_mapping_dynamic",
)
CHUG_HDR_DISPLAY_FEATURE_COLUMNS: tuple[str, ...] = (
    CHUG_HDR_FEATURE_COLUMNS + CHUG_HDR_DISPLAY_FEATURES
)
FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1 = "chug-hdr-display-v1"
FEATURE_SCHEMAS: dict[str, tuple[str, ...]] = {
    FEATURE_SCHEMA_KONVID_V1: FEATURE_COLUMNS,
    FEATURE_SCHEMA_CHUG_HDR_WIDE_V1: CHUG_HDR_FEATURE_COLUMNS,
    FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1: CHUG_HDR_DISPLAY_FEATURE_COLUMNS,
}

# ENCODER_VOCAB v4 — KonViD UGC content collapses to a single
# ``ugc-mixed`` slot per ADR-0325 §Phase 2 §Decision. The MOS-head
# input carries the slot as a length-1 one-hot so the schema is
# forward-compatible with future multi-slot expansion (e.g. when the
# fork starts ingesting LSVQ + YouTube-UGC).
ENCODER_VOCAB_V4: tuple[str, ...] = ("ugc-mixed",)
ENCODER_VOCAB_V4_VERSION = 4
N_ENCODERS = len(ENCODER_VOCAB_V4)

# MOS scale — KonViD uses the standard ITU-T 5-point MOS scale.
MOS_MIN = 1.0
MOS_MAX = 5.0

# Production-flip gate per ADR-0325 Phase 3 (mirrors ADR-0303 shape).
# The values are recorded here as constants so the trainer's emitted
# JSON carries the gate it was trained against. They are *not* lowered
# on real-corpus failures (memory ``feedback_no_test_weakening``).
GATE_MEAN_PLCC: float = 0.85
GATE_SROCC: float = 0.82
GATE_RMSE_MAX: float = 0.45
GATE_SPREAD_MAX: float = 0.005

# Synthetic-corpus gate — placeholder per the task brief; the real
# threshold comes from real-corpus runs once #447 lands.
SYNTHETIC_GATE_PLCC: float = 0.75


@dataclass(frozen=True)
class CorpusArrays:
    """Projected MOS-corpus arrays plus optional split labels."""

    features: np.ndarray
    encoder: np.ndarray
    mos: np.ndarray
    splits: np.ndarray


@dataclass(frozen=True)
class DisplayProfile:
    """Target display profile normalised onto trainer feature columns."""

    source_path: str
    sha256: str
    feature_values: dict[str, float]


# ---------------------------------------------------------------------
# Helpers — seeding, sha256, corpus loading.
# ---------------------------------------------------------------------


def _set_seed(seed: int) -> None:
    """Deterministic seed across numpy + torch (when available)."""
    import contextlib

    np.random.seed(seed)
    try:
        import torch  # type: ignore[import-not-found]

        torch.manual_seed(seed)
        if hasattr(torch, "cuda") and torch.cuda.is_available():
            torch.cuda.manual_seed_all(seed)
        if hasattr(torch, "use_deterministic_algorithms"):
            # Some PyTorch builds reject a hard True under CUDA; warn-only is best-effort.
            with contextlib.suppress(RuntimeError):
                torch.use_deterministic_algorithms(True, warn_only=True)
    except ImportError:
        pass


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    """Load a JSONL corpus drop into a list of dicts."""
    out: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                # Skip mis-formatted lines rather than aborting the
                # whole training run; a well-tested ingester should not
                # produce them but defence in depth is cheap.
                continue
    return out


def _load_parquet(path: Path) -> list[dict[str, Any]]:
    """Load a FULL_FEATURES parquet corpus drop into a list of dicts."""
    try:
        import pandas as pd  # type: ignore[import-not-found]
    except ImportError as exc:
        raise RuntimeError("pandas is required for --feature-parquet corpus loading") from exc
    frame = pd.read_parquet(path)
    return frame.to_dict(orient="records")


def _load_corpus_rows(path: Path) -> list[dict[str, Any]]:
    if path.suffix.lower() in {".parquet", ".pq"}:
        return _load_parquet(path)
    return _load_jsonl(path)


def _normalise_split(raw: Any) -> str:
    split = str(raw or "").strip().lower()
    return split if split in {"train", "val", "test"} else ""


def _feature_columns_for_schema(schema: str) -> tuple[str, ...]:
    try:
        return FEATURE_SCHEMAS[schema]
    except KeyError as exc:
        known = ", ".join(sorted(FEATURE_SCHEMAS))
        raise ValueError(f"unknown feature schema {schema!r}; expected one of: {known}") from exc


def _safe_float(value: Any) -> float:
    try:
        out = float(value)
    except (TypeError, ValueError):
        return math.nan
    return out if math.isfinite(out) else math.nan


def _normalise_positive(value: Any, denominator: float) -> float:
    raw = _safe_float(value)
    if not math.isfinite(raw) or raw < 0.0:
        return math.nan
    return raw / denominator


def _mapping_first(mapping: dict[str, Any], *keys: str) -> Any:
    for key in keys:
        if key in mapping and mapping[key] is not None:
            return mapping[key]
    return None


def _clamp01(value: float) -> float:
    return min(1.0, max(0.0, value))


def _normalise_fraction(value: Any) -> float:
    raw = _safe_float(value)
    if not math.isfinite(raw) or raw < 0.0:
        return math.nan
    if raw > 1.0 and raw <= 100.0:
        raw /= 100.0
    return _clamp01(raw) if raw <= 1.0 else math.nan


def _boolish_feature(value: Any) -> float:
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    raw = _safe_float(value)
    if math.isfinite(raw):
        return 1.0 if raw != 0.0 else 0.0
    text = str(value or "").strip().lower()
    if text in {"1", "true", "yes", "y", "on", "enabled", "enable"}:
        return 1.0
    if text in {"0", "false", "no", "n", "off", "disabled", "disable", "none"}:
        return 0.0
    return math.nan


def _display_panel_type(mapping: dict[str, Any]) -> str:
    return (
        str(
            _mapping_first(
                mapping,
                "display_panel_type",
                "panel_type",
                "display_type",
                "panel",
            )
            or ""
        )
        .strip()
        .lower()
    )


def _display_tone_mapping(mapping: dict[str, Any]) -> str:
    return (
        str(
            _mapping_first(
                mapping,
                "display_tone_mapping",
                "tone_mapping",
                "hdr_tone_mapping",
                "hdr_format",
            )
            or ""
        )
        .strip()
        .lower()
    )


def _display_feature_from_mapping(mapping: dict[str, Any], name: str) -> float:
    direct = _safe_float(mapping.get(name))
    if math.isfinite(direct):
        return direct
    if name == "display_peak_luminance_nits_norm":
        value = _mapping_first(
            mapping,
            "display_peak_luminance_nits",
            "peak_luminance_nits",
            "peak_nits",
            "max_luminance_nits",
        )
        return _normalise_positive(value, 10000.0)
    if name == "display_black_luminance_nits_norm":
        value = _mapping_first(
            mapping,
            "display_black_luminance_nits",
            "black_luminance_nits",
            "black_nits",
            "min_luminance_nits",
        )
        return _normalise_positive(value, 1.0)
    if name == "display_contrast_ratio_log_norm":
        contrast = _safe_float(_mapping_first(mapping, "display_contrast_ratio", "contrast_ratio"))
        if not math.isfinite(contrast):
            peak = _safe_float(
                _mapping_first(
                    mapping,
                    "display_peak_luminance_nits",
                    "peak_luminance_nits",
                    "peak_nits",
                    "max_luminance_nits",
                )
            )
            black = _safe_float(
                _mapping_first(
                    mapping,
                    "display_black_luminance_nits",
                    "black_luminance_nits",
                    "black_nits",
                    "min_luminance_nits",
                )
            )
            if math.isfinite(peak) and math.isfinite(black) and black > 0.0:
                contrast = peak / black
        if not math.isfinite(contrast) or contrast <= 1.0:
            return math.nan
        return _clamp01(math.log10(contrast) / 7.0)
    if name == "display_ambient_lux_norm":
        value = _mapping_first(mapping, "display_ambient_lux", "ambient_lux", "viewing_lux")
        return _normalise_positive(value, 1000.0)
    if name == "display_bt2020_coverage":
        return _normalise_fraction(
            _mapping_first(
                mapping,
                "display_bt2020_coverage",
                "bt2020_coverage",
                "rec2020_coverage",
            )
        )
    if name == "display_p3_coverage":
        return _normalise_fraction(
            _mapping_first(mapping, "display_p3_coverage", "p3_coverage", "dci_p3_coverage")
        )
    panel_type = _display_panel_type(mapping)
    if name == "display_panel_oled":
        return 1.0 if "oled" in panel_type else 0.0 if panel_type else math.nan
    if name == "display_panel_qled":
        return (
            1.0
            if ("qled" in panel_type or "quantum" in panel_type)
            else (0.0 if panel_type else math.nan)
        )
    if name == "display_panel_lcd":
        is_lcd = "oled" not in panel_type and any(
            token in panel_type for token in ("lcd", "mini-led", "miniled", "led", "ips", "va")
        )
        return 1.0 if is_lcd else 0.0 if panel_type else math.nan
    if name == "display_local_dimming":
        value = _mapping_first(
            mapping,
            "display_local_dimming",
            "local_dimming",
            "full_array_local_dimming",
        )
        if value is not None:
            return _boolish_feature(value)
        return (
            1.0
            if any(token in panel_type for token in ("mini-led", "miniled", "fald"))
            else math.nan
        )
    if name == "display_tone_mapping_dynamic":
        value = _mapping_first(mapping, "display_tone_mapping_dynamic", "tone_mapping_dynamic")
        if value is not None:
            return _boolish_feature(value)
        tone_mapping = _display_tone_mapping(mapping)
        if not tone_mapping:
            return math.nan
        dynamic = any(
            token in tone_mapping for token in ("dynamic", "hdr10+", "hdr10plus", "dolby", "vision")
        )
        return 1.0 if dynamic else 0.0
    return math.nan


def _display_profile_candidate(raw: dict[str, Any]) -> dict[str, Any]:
    candidate = dict(raw)
    for key in ("display", "panel", "profile", "features"):
        nested = raw.get(key)
        if isinstance(nested, dict):
            candidate.update(nested)
    return candidate


def _normalise_display_profile_payload(raw: dict[str, Any]) -> dict[str, float]:
    candidate = _display_profile_candidate(raw)
    values: dict[str, float] = {}
    for name in CHUG_HDR_DISPLAY_FEATURES:
        value = _display_feature_from_mapping(candidate, name)
        if math.isfinite(value):
            values[name] = float(value)
    if not values:
        raise ValueError("display profile JSON did not contain any recognised display fields")
    return values


def _load_display_profile(path: Path | None) -> DisplayProfile | None:
    if path is None:
        return None
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict):
        raise ValueError("display profile JSON must be an object")
    values = _normalise_display_profile_payload(raw)
    return DisplayProfile(source_path=str(path), sha256=sha256(path), feature_values=values)


def _parse_bitrate_mbps(value: Any) -> float:
    """Parse CHUG labels such as ``0.2M`` or ``500K`` into Mbps."""
    if isinstance(value, (int, float)):
        raw = _safe_float(value)
        return raw if math.isfinite(raw) else math.nan
    text = str(value or "").strip().lower().replace("bps", "")
    if not text:
        return math.nan
    multiplier = 1.0
    if text.endswith(("mb", "m")):
        multiplier = 1.0
        text = text.removesuffix("mb").removesuffix("m")
    elif text.endswith(("kb", "k")):
        multiplier = 0.001
        text = text.removesuffix("kb").removesuffix("k")
    raw = _safe_float(text)
    if not math.isfinite(raw):
        return math.nan
    return raw * multiplier


def _row_feature_value(
    row: dict[str, Any],
    name: str,
    display_profile: dict[str, float] | None = None,
) -> float:
    if name == "chug_bitrate_mbps":
        if row.get("chug_bitrate_label") is not None:
            return _parse_bitrate_mbps(row.get("chug_bitrate_label"))
        mbps = _safe_float(row.get("bitrate_mbps"))
        if math.isfinite(mbps):
            return mbps
        kbps = _safe_float(row.get("bitrate_kbps"))
        return kbps / 1000.0 if math.isfinite(kbps) else math.nan
    if name == "chug_orientation_portrait":
        orientation = str(row.get("chug_orientation") or "").strip().lower()
        if orientation.startswith("portrait"):
            return 1.0
        if orientation.startswith("landscape"):
            return 0.0
        return math.nan
    if name == "distorted_width_norm":
        return _normalise_positive(row.get("width"), 3840.0)
    if name == "distorted_height_norm":
        return _normalise_positive(row.get("height"), 2160.0)
    if name == "reference_width_norm":
        return _normalise_positive(row.get("feature_width"), 3840.0)
    if name == "reference_height_norm":
        return _normalise_positive(row.get("feature_height"), 2160.0)
    if name == "duration_s_norm":
        return _normalise_positive(row.get("duration_s"), 60.0)
    if name == "n_feature_frames_norm":
        return _normalise_positive(row.get("n_feature_frames"), 1800.0)
    if name == "feature_bitdepth_norm":
        bitdepth = _safe_float(row.get("feature_bitdepth"))
        return (bitdepth - 8.0) / 4.0 if math.isfinite(bitdepth) else math.nan
    if name in CHUG_HDR_DISPLAY_FEATURES:
        value = _display_feature_from_mapping(row, name)
        if math.isfinite(value):
            return value
        if display_profile is not None:
            return _safe_float(display_profile.get(name))
        return math.nan

    value = row.get(name)
    primary_f = _safe_float(value)
    if not math.isfinite(primary_f):
        # Parquet corpora produced by materialisers store per-clip temporal
        # averages under ``<feature>_mean`` (e.g. ``adm2_mean``).  When a
        # parquet file has mixed columns, pandas fills absent slots with NaN
        # rather than omitting the key, so NaN must also fall back.
        value = row.get(f"{name}_mean")
        primary_f = _safe_float(value)
    return primary_f


def _row_to_features(
    row: dict[str, Any],
    feature_columns: Sequence[str] = FEATURE_COLUMNS,
    display_profile: dict[str, float] | None = None,
) -> tuple[np.ndarray, float] | None:
    """Project one corpus row to ``(features, mos)`` or ``None`` to skip.

    Phase 1/2 KonViD JSONL rows carry per-clip aggregates only — they
    do *not* yet carry the canonical-6 libvmaf features, the saliency
    extractor's output, or TransNet shot-metadata. The trainer
    accepts whichever subset of those columns the row carries and
    fills the rest with content-independent defaults; that lets this
    script run today against the in-flight Phase 1/2 JSONL while the
    canonical-6 / saliency / shot-metadata columns get bolted on in
    follow-up PRs.
    """
    mos = row.get("mos")
    try:
        mos_f = float(mos)
    except (TypeError, ValueError):
        mos_f = math.nan
    if not math.isfinite(mos_f):
        raw_mos = row.get("mos_raw_0_100")
        try:
            mos_f = MOS_MIN + (MOS_MAX - MOS_MIN) * (float(raw_mos) / 100.0)
        except (TypeError, ValueError):
            mos_f = math.nan
    if not (math.isfinite(mos_f) and MOS_MIN <= mos_f <= MOS_MAX):
        # KonViD's published MOS values live in [1, 5]; out-of-range
        # rows indicate a schema mismatch and are dropped rather than
        # silently clamped.
        return None
    feats = np.zeros(len(feature_columns), dtype=np.float32)
    for idx, name in enumerate(feature_columns):
        value = _row_feature_value(row, name, display_profile=display_profile)
        if math.isfinite(value):
            feats[idx] = float(value)
    return feats, mos_f


def _load_corpus_arrays(
    paths: Sequence[Path],
    jsonl_paths: Sequence[Path] = (),
    parquet_paths: Sequence[Path] = (),
    feature_columns: Sequence[str] = FEATURE_COLUMNS,
    display_profile: dict[str, float] | None = None,
) -> CorpusArrays:
    """Load + project one or more JSONL or parquet corpus paths.

    Returns features, encoder one-hot, MOS, and optional split labels.
    The encoder one-hot is always ``[1.0]`` because ENCODER_VOCAB v4 has
    a single slot.
    """
    rows: list[dict[str, Any]] = []
    for p in (*paths, *jsonl_paths, *parquet_paths):
        if p is not None and p.is_file():
            rows.extend(_load_corpus_rows(p))
    pairs: list[tuple[np.ndarray, float]] = []
    splits: list[str] = []
    for row in rows:
        proj = _row_to_features(
            row,
            feature_columns=feature_columns,
            display_profile=display_profile,
        )
        if proj is not None:
            pairs.append(proj)
            splits.append(_normalise_split(row.get("split")))
    if not pairs:
        return CorpusArrays(
            features=np.empty((0, len(feature_columns)), dtype=np.float32),
            encoder=np.empty((0, N_ENCODERS), dtype=np.float32),
            mos=np.empty((0,), dtype=np.float32),
            splits=np.empty((0,), dtype="<U5"),
        )
    feats = np.stack([p[0] for p in pairs]).astype(np.float32)
    mos = np.asarray([p[1] for p in pairs], dtype=np.float32)
    encoder = np.ones((feats.shape[0], N_ENCODERS), dtype=np.float32)
    return CorpusArrays(
        features=feats,
        encoder=encoder,
        mos=mos,
        splits=np.asarray(splits, dtype="<U5"),
    )


def _load_corpus(paths: Sequence[Path]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Load + project one or more JSONL or parquet corpus paths."""
    arrays = _load_corpus_arrays(paths)
    return arrays.features, arrays.encoder, arrays.mos


def _heldout_split_indices(splits: np.ndarray) -> tuple[np.ndarray, np.ndarray] | None:
    """Return train/eval indices when corpus rows carry explicit splits."""
    if splits.size == 0:
        return None
    train = np.flatnonzero(splits == "train")
    val = np.flatnonzero(splits == "val")
    test = np.flatnonzero(splits == "test")
    eval_idx = val if len(val) > 0 else test
    if len(train) == 0 or len(eval_idx) == 0:
        return None
    return train, eval_idx


# ---------------------------------------------------------------------
# Synthetic-corpus generator — used only by explicit smoke/dev fallback paths.
# ---------------------------------------------------------------------


def _synthesize_corpus(
    n_rows: int = 600,
    seed: int = 0,
    feature_columns: Sequence[str] = FEATURE_COLUMNS,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Synthesise a deterministic-seeded synthetic MOS corpus.

    The synthetic generator picks plausible canonical-6 ranges that
    mirror the fr_regressor_v2 trainer's generator, then derives a
    learnable MOS target as a smooth nonlinear function of
    ``(adm2, motion2, saliency_mean)`` plus 1-rater Gaussian noise.
    The generator is purely deterministic at the per-seed level so a
    fresh checkout reproduces the gate verdict bit-for-bit.
    """
    rng = np.random.default_rng(seed)
    base_feats = np.column_stack(
        [
            rng.uniform(0.4, 1.0, size=n_rows),  # adm2
            rng.uniform(0.2, 0.95, size=n_rows),  # vif_scale0
            rng.uniform(0.3, 0.95, size=n_rows),  # vif_scale1
            rng.uniform(0.4, 0.97, size=n_rows),  # vif_scale2
            rng.uniform(0.5, 0.98, size=n_rows),  # vif_scale3
            rng.uniform(0.0, 30.0, size=n_rows),  # motion2
            rng.uniform(0.0, 0.6, size=n_rows),  # saliency_mean
            rng.uniform(0.0, 0.05, size=n_rows),  # saliency_var
            rng.uniform(0.0, 1.0, size=n_rows),  # shot_count_norm
            rng.uniform(0.0, 1.0, size=n_rows),  # shot_mean_len_norm
            rng.uniform(0.0, 0.1, size=n_rows),  # shot_cut_density
        ]
    ).astype(np.float32)
    base_map = {name: base_feats[:, idx] for idx, name in enumerate(FEATURE_COLUMNS)}
    columns: list[np.ndarray] = []
    for name in feature_columns:
        if name in base_map:
            columns.append(base_map[name])
            continue
        if name.endswith("_p10") and name.removesuffix("_p10") in base_map:
            columns.append(np.maximum(0.0, base_map[name.removesuffix("_p10")] * 0.90))
            continue
        if name.endswith("_p90") and name.removesuffix("_p90") in base_map:
            columns.append(base_map[name.removesuffix("_p90")] * 1.08)
            continue
        if name.endswith("_std") and name.removesuffix("_std") in base_map:
            columns.append(rng.uniform(0.005, 0.05, size=n_rows).astype(np.float32))
            continue
        if name == "chug_bitrate_mbps":
            columns.append(rng.choice([0.2, 0.5, 1.0, 2.0, 4.0], size=n_rows).astype(np.float32))
            continue
        if name == "chug_orientation_portrait":
            columns.append(rng.integers(0, 2, size=n_rows).astype(np.float32))
            continue
        if name == "chug_ref":
            columns.append(np.zeros(n_rows, dtype=np.float32))
            continue
        if name in {
            "distorted_width_norm",
            "distorted_height_norm",
            "reference_width_norm",
            "reference_height_norm",
        }:
            columns.append(rng.uniform(0.16, 1.0, size=n_rows).astype(np.float32))
            continue
        if name == "duration_s_norm":
            columns.append(rng.uniform(0.05, 0.25, size=n_rows).astype(np.float32))
            continue
        if name == "n_feature_frames_norm":
            columns.append(rng.uniform(0.05, 0.25, size=n_rows).astype(np.float32))
            continue
        if name == "feature_bitdepth_norm":
            columns.append(np.full(n_rows, 0.5, dtype=np.float32))
            continue
        if name == "display_peak_luminance_nits_norm":
            columns.append(rng.uniform(0.04, 0.20, size=n_rows).astype(np.float32))
            continue
        if name == "display_black_luminance_nits_norm":
            columns.append(rng.uniform(0.0001, 0.02, size=n_rows).astype(np.float32))
            continue
        if name == "display_contrast_ratio_log_norm":
            columns.append(rng.uniform(0.55, 0.95, size=n_rows).astype(np.float32))
            continue
        if name == "display_ambient_lux_norm":
            columns.append(rng.uniform(0.0, 0.25, size=n_rows).astype(np.float32))
            continue
        if name in {"display_bt2020_coverage", "display_p3_coverage"}:
            columns.append(rng.uniform(0.65, 1.0, size=n_rows).astype(np.float32))
            continue
        if name in {
            "display_panel_oled",
            "display_panel_qled",
            "display_panel_lcd",
            "display_local_dimming",
            "display_tone_mapping_dynamic",
        }:
            columns.append(rng.integers(0, 2, size=n_rows).astype(np.float32))
            continue
        columns.append(np.zeros(n_rows, dtype=np.float32))
    feats = np.column_stack(columns).astype(np.float32)

    feature_columns_list = list(feature_columns)

    def col(column: str, fallback: float = 0.0) -> np.ndarray:
        try:
            return feats[:, feature_columns_list.index(column)]
        except ValueError:
            return np.full(n_rows, fallback, dtype=np.float32)

    # Smooth nonlinear MOS target — anchored so well-encoded
    # high-saliency content lives near 4.5 and grainy fast-motion
    # content near 1.5. The form is a sum of three monotone terms
    # plus a small noise floor, which keeps a small MLP capable of
    # recovering ≥0.75 PLCC on the synthetic split.
    base = (
        2.5
        + 1.6 * col("adm2")  # adm2 lift
        - 0.04 * col("motion2")  # motion2 penalty
        + 0.6 * col("saliency_mean")  # saliency lift
        + 0.3 * col("vif_scale3")  # vif_scale3 lift
    )
    noise = rng.normal(0.0, 0.10, size=n_rows)  # ~rater noise
    target = np.clip(base + noise, MOS_MIN, MOS_MAX).astype(np.float32)
    encoder = np.ones((n_rows, N_ENCODERS), dtype=np.float32)
    return feats, encoder, target


# ---------------------------------------------------------------------
# Model — small MLP. ~30K-100K params, ONNX-allowlist conformant
# (Gemm + ReLU + dropout-as-Identity at eval time + LayerNorm).
# ---------------------------------------------------------------------


def _build_model(
    in_features: int = N_FEATURES,
    n_encoders: int = N_ENCODERS,
    hidden: int = 64,
    depth: int = 2,
    dropout: float = 0.1,
):  # type: ignore[no-untyped-def]
    """Build a small ``nn.Module`` MLP. Imported torch lazily."""
    import torch
    from torch import nn

    class MOSHead(nn.Module):
        def __init__(self) -> None:
            super().__init__()
            in_dim = in_features + n_encoders
            layers: list[nn.Module] = [nn.LayerNorm(in_dim)]
            prev = in_dim
            for _ in range(depth):
                layers.append(nn.Linear(prev, hidden))
                layers.append(nn.ReLU())
                layers.append(nn.Dropout(dropout))
                prev = hidden
            layers.append(nn.Linear(prev, 1))
            self.body = nn.Sequential(*layers)

        def forward(
            self,
            features: "torch.Tensor",
            encoder_onehot: "torch.Tensor",
        ) -> "torch.Tensor":
            x = torch.cat([features, encoder_onehot], dim=-1)
            raw = self.body(x).squeeze(-1)
            # Sigmoid + affine clamp into [MOS_MIN, MOS_MAX] — keeps
            # the head's output in the documented MOS range without
            # relying on a runtime Clip op.
            return MOS_MIN + (MOS_MAX - MOS_MIN) * torch.sigmoid(raw)

    return MOSHead()


def _count_parameters(model) -> int:  # type: ignore[no-untyped-def]
    return int(sum(p.numel() for p in model.parameters()))


def _resolve_device(device: str):  # type: ignore[no-untyped-def]
    """Resolve the requested PyTorch training device."""
    import torch

    requested = device.strip().lower()
    if requested == "auto":
        requested = "cuda" if torch.cuda.is_available() else "cpu"
    if requested == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested for MOS-head training but torch.cuda is unavailable")
    if requested != "cpu" and not requested.startswith("cuda"):
        raise RuntimeError(f"unsupported MOS-head training device: {device}")
    return torch.device(requested)


# ---------------------------------------------------------------------
# Cross-validation training loop.
# ---------------------------------------------------------------------


def _kfold_indices(n: int, k: int, seed: int) -> list[tuple[np.ndarray, np.ndarray]]:
    """Split ``n`` row indices into ``k`` (train, val) folds, deterministic."""
    rng = np.random.default_rng(seed)
    perm = rng.permutation(n)
    folds = np.array_split(perm, k)
    out: list[tuple[np.ndarray, np.ndarray]] = []
    for i in range(k):
        val = folds[i]
        train = np.concatenate([folds[j] for j in range(k) if j != i])
        out.append((train, val))
    return out


def _plcc(a: np.ndarray, b: np.ndarray) -> float:
    if len(a) < 2 or len(b) < 2:
        return float("nan")
    if a.std() == 0.0 or b.std() == 0.0:
        return float("nan")
    return float(np.corrcoef(a, b)[0, 1])


def _srocc(a: np.ndarray, b: np.ndarray) -> float:
    if len(a) < 2 or len(b) < 2:
        return float("nan")
    ra = np.argsort(np.argsort(a))
    rb = np.argsort(np.argsort(b))
    return _plcc(ra.astype(np.float64), rb.astype(np.float64))


def _rmse(a: np.ndarray, b: np.ndarray) -> float:
    if len(a) == 0:
        return float("nan")
    return float(np.sqrt(np.mean((a - b) ** 2)))


def _train_one_fold(
    *,
    features_train: np.ndarray,
    encoder_train: np.ndarray,
    mos_train: np.ndarray,
    features_val: np.ndarray,
    encoder_val: np.ndarray,
    mos_val: np.ndarray,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    seed: int,
    n_features: int,
    device: str = "cpu",
) -> tuple[np.ndarray, dict[str, float]]:
    """Train one fold; return ``(val_pred, fold_metrics)``."""
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    _set_seed(seed)
    torch_device = _resolve_device(device)
    model = _build_model(in_features=n_features).to(torch_device)
    ds = TensorDataset(
        torch.from_numpy(features_train.astype(np.float32)),
        torch.from_numpy(encoder_train.astype(np.float32)),
        torch.from_numpy(mos_train.astype(np.float32)),
    )
    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, drop_last=False)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=weight_decay)
    loss_fn = torch.nn.MSELoss()
    model.train()
    for _ep in range(epochs):
        for fb, eb, yb in loader:
            fb = fb.to(torch_device)
            eb = eb.to(torch_device)
            yb = yb.to(torch_device)
            opt.zero_grad()
            pred = model(fb, eb)
            loss = loss_fn(pred, yb)
            loss.backward()
            opt.step()
    model.eval()
    with torch.no_grad():
        val_pred = (
            model(
                torch.from_numpy(features_val.astype(np.float32)).to(torch_device),
                torch.from_numpy(encoder_val.astype(np.float32)).to(torch_device),
            )
            .cpu()
            .numpy()
        )
    val_pred = np.asarray(val_pred, dtype=np.float32)
    metrics = {
        "n_train": len(mos_train),
        "n_val": len(mos_val),
        "plcc": _plcc(val_pred, mos_val),
        "srocc": _srocc(val_pred, mos_val),
        "rmse": _rmse(val_pred, mos_val),
    }
    return val_pred, metrics


def _train_full(
    *,
    features: np.ndarray,
    encoder: np.ndarray,
    mos: np.ndarray,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    seed: int,
    n_features: int,
    device: str = "cpu",
):  # type: ignore[no-untyped-def]
    """Train one model on the full corpus — the ship checkpoint."""
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    _set_seed(seed)
    torch_device = _resolve_device(device)
    model = _build_model(in_features=n_features).to(torch_device)
    ds = TensorDataset(
        torch.from_numpy(features.astype(np.float32)),
        torch.from_numpy(encoder.astype(np.float32)),
        torch.from_numpy(mos.astype(np.float32)),
    )
    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, drop_last=False)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=weight_decay)
    loss_fn = torch.nn.MSELoss()
    model.train()
    for _ep in range(epochs):
        for fb, eb, yb in loader:
            fb = fb.to(torch_device)
            eb = eb.to(torch_device)
            yb = yb.to(torch_device)
            opt.zero_grad()
            pred = model(fb, eb)
            loss = loss_fn(pred, yb)
            loss.backward()
            opt.step()
    model.eval()
    return model


def _export_onnx(
    model,
    onnx_path: Path,
    n_features: int = N_FEATURES,
) -> str:  # type: ignore[no-untyped-def]
    """Export the trained model as opset-17 ONNX. Returns sha256."""
    import torch

    onnx_path.parent.mkdir(parents=True, exist_ok=True)
    model = model.cpu()
    dummy_features = torch.zeros(1, n_features, dtype=torch.float32)
    dummy_encoder = torch.ones(1, N_ENCODERS, dtype=torch.float32)
    torch.onnx.export(
        model,
        (dummy_features, dummy_encoder),
        str(onnx_path),
        input_names=["features", "encoder_onehot"],
        output_names=["mos"],
        dynamic_axes={
            "features": {0: "batch"},
            "encoder_onehot": {0: "batch"},
            "mos": {0: "batch"},
        },
        opset_version=17,
    )
    return sha256(onnx_path)


# ---------------------------------------------------------------------
# Gate evaluation — mirrors ADR-0303 shape.
# ---------------------------------------------------------------------


def _evaluate_gate(folds: list[dict[str, Any]], *, synthetic: bool) -> dict[str, Any]:
    """Apply the production-flip gate to per-fold metrics. No threshold lowering."""
    plcc_vals = [f["plcc"] for f in folds if not math.isnan(f["plcc"])]
    srocc_vals = [f["srocc"] for f in folds if not math.isnan(f["srocc"])]
    rmse_vals = [f["rmse"] for f in folds if not math.isnan(f["rmse"])]
    mean_plcc = float(np.mean(plcc_vals)) if plcc_vals else float("nan")
    mean_srocc = float(np.mean(srocc_vals)) if srocc_vals else float("nan")
    mean_rmse = float(np.mean(rmse_vals)) if rmse_vals else float("nan")
    spread = float(max(plcc_vals) - min(plcc_vals)) if plcc_vals else float("nan")
    if synthetic:
        # Synthetic gate is a single PLCC threshold per the task brief
        # — the real PLCC/SROCC/RMSE/spread gate comes online when a
        # real corpus is on disk.
        passed = (not math.isnan(mean_plcc)) and mean_plcc >= SYNTHETIC_GATE_PLCC
        gate_used: dict[str, Any] = {
            "kind": "synthetic",
            "plcc_min": SYNTHETIC_GATE_PLCC,
        }
    else:
        passed = (
            (not math.isnan(mean_plcc))
            and mean_plcc >= GATE_MEAN_PLCC
            and (not math.isnan(mean_srocc))
            and mean_srocc >= GATE_SROCC
            and (not math.isnan(mean_rmse))
            and mean_rmse <= GATE_RMSE_MAX
            and (not math.isnan(spread))
            and spread <= GATE_SPREAD_MAX
        )
        gate_used = {
            "kind": "real",
            "plcc_min": GATE_MEAN_PLCC,
            "srocc_min": GATE_SROCC,
            "rmse_max": GATE_RMSE_MAX,
            "spread_max": GATE_SPREAD_MAX,
        }
    return {
        "passed": bool(passed),
        "mean_plcc": mean_plcc,
        "mean_srocc": mean_srocc,
        "mean_rmse": mean_rmse,
        "plcc_spread": spread,
        "gate": gate_used,
    }


# ---------------------------------------------------------------------
# Manifest writer.
# ---------------------------------------------------------------------


def _build_manifest(
    *,
    onnx_path: Path,
    sha256: str,
    model_id: str = DEFAULT_MODEL_ID,
    feature_schema: str = FEATURE_SCHEMA_KONVID_V1,
    feature_columns: Sequence[str] = FEATURE_COLUMNS,
    folds: list[dict[str, Any]],
    gate: dict[str, Any],
    feature_mean: list[float],
    feature_std: list[float],
    n_params: int,
    smoke: bool,
    n_rows: int,
    epochs: int,
    seed: int,
    display_profile: DisplayProfile | None = None,
    run_provenance: dict[str, Any] | None = None,
) -> dict[str, Any]:
    manifest: dict[str, Any] = {
        "id": model_id,
        "kind": "mos",
        "onnx": onnx_path.name,
        "sha256": sha256,
        "opset": 17,
        "feature_schema": feature_schema,
        "feature_order": list(feature_columns),
        "feature_mean": feature_mean,
        "feature_std": feature_std,
        "encoder_vocab": list(ENCODER_VOCAB_V4),
        "encoder_vocab_version": ENCODER_VOCAB_V4_VERSION,
        "mos_range": [MOS_MIN, MOS_MAX],
        "n_parameters": n_params,
        "training_recipe": {
            "epochs": epochs,
            "seed": seed,
            "n_rows": n_rows,
            "smoke": smoke,
            "k_folds": len(folds),
        },
        "folds": folds,
        "gate": gate,
        "adr": "0336",
        "phase": "ADR-0325 Phase 3",
    }
    if display_profile is not None:
        manifest["display_profile"] = {
            "schema": "chug-display-profile-v1",
            "source_path": display_profile.source_path,
            "sha256": display_profile.sha256,
            "feature_values": display_profile.feature_values,
        }
    if run_provenance is not None:
        manifest["run_provenance"] = run_provenance
    return manifest


# ---------------------------------------------------------------------
# Entry point.
# ---------------------------------------------------------------------


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="train_konvid_mos_head.py")
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    _konvid_1k_dir = Path(
        os.environ.get(
            "VMAF_KONVID_1K_DIR",
            str(Path(__file__).resolve().parents[2] / ".corpus" / "konvid-1k"),
        )
    )
    _konvid_150k_dir = Path(
        os.environ.get(
            "VMAF_KONVID_150K_DIR",
            str(Path(__file__).resolve().parents[2] / ".corpus" / "konvid-150k"),
        )
    )
    ap.add_argument(
        "--konvid-1k",
        type=Path,
        default=_konvid_1k_dir / "konvid_1k.jsonl",
        help=(
            "Path to the KonViD-1k JSONL corpus drop (Phase 1, PR #440). "
            "Parent dir overridden via ``VMAF_KONVID_1K_DIR``."
        ),
    )
    ap.add_argument(
        "--konvid-150k",
        type=Path,
        default=_konvid_150k_dir / "konvid_150k.jsonl",
        help=(
            "Path to the KonViD-150k JSONL corpus drop (Phase 2, PR #447). "
            "Parent dir overridden via ``VMAF_KONVID_150K_DIR``."
        ),
    )
    ap.add_argument(
        "--feature-parquet",
        type=Path,
        action="append",
        default=[],
        help=(
            "FULL_FEATURES parquet corpus table from extract_k150k_features.py; "
            "may be repeated for CHUG/K150K shards."
        ),
    )
    ap.add_argument(
        "--feature-jsonl",
        type=Path,
        action="append",
        default=[],
        help=argparse.SUPPRESS,
    )
    ap.add_argument(
        "--model-id",
        default=DEFAULT_MODEL_ID,
        help=argparse.SUPPRESS,
    )
    ap.add_argument(
        "--feature-schema",
        choices=tuple(FEATURE_SCHEMAS),
        default=FEATURE_SCHEMA_KONVID_V1,
        help=argparse.SUPPRESS,
    )
    ap.add_argument(
        "--display-profile-json",
        type=Path,
        default=None,
        help=argparse.SUPPRESS,
    )
    ap.add_argument(
        "--log-prefix",
        default="konvid-mos",
        help=argparse.SUPPRESS,
    )
    ap.add_argument(
        "--smoke",
        action="store_true",
        help="Synthesize a deterministic-seeded corpus instead of loading "
        "from disk; used by CI smoke + the test harness.",
    )
    ap.add_argument(
        "--allow-synthetic-fallback",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--smoke-epochs", type=int, default=30)
    ap.add_argument("--batch-size", type=int, default=64)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--weight-decay", type=float, default=1e-5)
    ap.add_argument("--k-folds", type=int, default=5)
    ap.add_argument("--seed", type=int, default=20260508)
    ap.add_argument(
        "--device",
        default="cpu",
        help=(
            "PyTorch training device: cpu, cuda, cuda:N, or auto. Defaults to "
            "cpu so CI smoke runs stay deterministic; local CHUG experiments "
            "can pass --device cuda."
        ),
    )
    ap.add_argument(
        "--out-onnx",
        type=Path,
        default=REPO_ROOT / "model" / "konvid_mos_head_v1.onnx",
    )
    ap.add_argument(
        "--out-card",
        type=Path,
        default=REPO_ROOT / "model" / "konvid_mos_head_v1_card.md",
        help="Model-card path (this script does not rewrite a hand-authored card; "
        "set this to a tmp path to force the auto-generated stub).",
    )
    ap.add_argument(
        "--out-manifest",
        type=Path,
        default=REPO_ROOT / "model" / "konvid_mos_head_v1.json",
    )
    ap.add_argument(
        "--no-export",
        action="store_true",
        help="Skip ONNX export + manifest write (dev mode).",
    )
    ap.add_argument("--run-entrypoint", type=Path, default=SCRIPT_PATH, help=argparse.SUPPRESS)
    ap.add_argument("--run-argv-json", default=None, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    run_argv = raw_argv
    if args.run_argv_json is not None:
        try:
            parsed_argv = json.loads(args.run_argv_json)
        except json.JSONDecodeError:
            print(
                f"[{args.log_prefix}] error: --run-argv-json must decode to list[str]",
                file=sys.stderr,
            )
            return 2
        if not isinstance(parsed_argv, list) or not all(
            isinstance(item, str) for item in parsed_argv
        ):
            print(
                f"[{args.log_prefix}] error: --run-argv-json must decode to list[str]",
                file=sys.stderr,
            )
            return 2
        run_argv = parsed_argv
    feature_columns = _feature_columns_for_schema(args.feature_schema)
    display_profile = _load_display_profile(args.display_profile_json)
    uses_display_profile = any(name in feature_columns for name in CHUG_HDR_DISPLAY_FEATURES)
    display_profile_values = (
        display_profile.feature_values
        if display_profile is not None and uses_display_profile
        else None
    )
    if display_profile is not None and not uses_display_profile:
        print(
            f"[{args.log_prefix}] display profile ignored by feature schema "
            f"{args.feature_schema}",
            file=sys.stderr,
        )

    if args.smoke:
        n_rows = 600
        features, encoder, mos = _synthesize_corpus(
            n_rows=n_rows,
            seed=args.seed,
            feature_columns=feature_columns,
        )
        splits = np.empty((features.shape[0],), dtype="<U5")
        epochs = args.smoke_epochs
        synthetic = True
        print(
            f"[{args.log_prefix}] smoke mode: {n_rows} synthetic rows, "
            f"epochs={epochs}, seed={args.seed}, device={args.device}",
            flush=True,
        )
    else:
        paths = [p for p in (args.konvid_1k, args.konvid_150k) if p is not None]
        feature_jsonls = [p for p in args.feature_jsonl if p is not None]
        feature_parquets = [p for p in args.feature_parquet if p is not None]
        arrays = _load_corpus_arrays(
            paths,
            jsonl_paths=feature_jsonls,
            parquet_paths=feature_parquets,
            feature_columns=feature_columns,
            display_profile=display_profile_values,
        )
        features, encoder, mos, splits = (
            arrays.features,
            arrays.encoder,
            arrays.mos,
            arrays.splits,
        )
        if features.shape[0] == 0:
            attempted = [str(p) for p in (*paths, *feature_jsonls, *feature_parquets)]
            if not args.allow_synthetic_fallback:
                print(
                    f"[{args.log_prefix}] error: no real MOS-labelled rows found at "
                    f"{attempted}. Add mos/mos_raw_0_100 with "
                    "ai/scripts/materialize_mos_labels.py, or pass --smoke for an "
                    "explicit synthetic pipeline run.",
                    file=sys.stderr,
                )
                return 2
            print(
                f"[{args.log_prefix}] no real corpus rows found at {attempted}; "
                "falling back to synthetic because --allow-synthetic-fallback was set. "
                "Prefer --smoke for CI smoke runs.",
                file=sys.stderr,
            )
            features, encoder, mos = _synthesize_corpus(
                n_rows=600,
                seed=args.seed,
                feature_columns=feature_columns,
            )
            splits = np.empty((features.shape[0],), dtype="<U5")
            synthetic = True
        else:
            synthetic = False
        epochs = args.epochs
        print(
            f"[{args.log_prefix}] {'real' if not synthetic else 'synthetic-fallback'} "
            f"mode: {features.shape[0]} rows, epochs={epochs}, device={args.device}",
            flush=True,
        )

    if features.shape[0] < args.k_folds * 2:
        print(
            f"[{args.log_prefix}] error: corpus has only {features.shape[0]} rows; "
            f"need at least {args.k_folds * 2} for {args.k_folds}-fold CV.",
            file=sys.stderr,
        )
        return 2

    # Per-corpus standardisation. The MLP carries a LayerNorm at its
    # input so this is informational only (recorded in the sidecar so
    # downstream consumers can replicate the recipe), but we still
    # compute it for parity with the fr_regressor_v2_ensemble manifest.
    feature_mean = features.mean(axis=0).astype(np.float32).tolist()
    feature_std = features.std(axis=0).astype(np.float32).tolist()

    t0 = time.time()
    folds_report: list[dict[str, Any]] = []
    split_indices = None if synthetic else _heldout_split_indices(splits)
    if split_indices is None:
        fold_indices = _kfold_indices(features.shape[0], args.k_folds, args.seed)
        validation_policy = f"{args.k_folds}-fold-random"
    else:
        train_idx, val_idx = split_indices
        fold_indices = [(train_idx, val_idx)]
        validation_policy = "explicit-corpus-split"
        print(
            f"[{args.log_prefix}] explicit split validation: "
            f"n_train={len(train_idx)} n_val={len(val_idx)}",
            flush=True,
        )
    for fold_idx, (train_idx, val_idx) in enumerate(fold_indices):
        _val_pred, metrics = _train_one_fold(
            features_train=features[train_idx],
            encoder_train=encoder[train_idx],
            mos_train=mos[train_idx],
            features_val=features[val_idx],
            encoder_val=encoder[val_idx],
            mos_val=mos[val_idx],
            epochs=epochs,
            batch_size=args.batch_size,
            lr=args.lr,
            weight_decay=args.weight_decay,
            seed=args.seed + fold_idx,
            n_features=features.shape[1],
            device=args.device,
        )
        metrics["fold"] = fold_idx
        metrics["validation_policy"] = validation_policy
        folds_report.append(metrics)
        print(
            f"[{args.log_prefix}] fold {fold_idx}: "
            f"plcc={metrics['plcc']:.4f} srocc={metrics['srocc']:.4f} "
            f"rmse={metrics['rmse']:.4f} (n_val={metrics['n_val']})",
            flush=True,
        )
    gate = _evaluate_gate(folds_report, synthetic=synthetic)
    print(
        f"[{args.log_prefix}] gate={'PASS' if gate['passed'] else 'FAIL'} "
        f"mean_plcc={gate['mean_plcc']:.4f} "
        f"mean_srocc={gate['mean_srocc']:.4f} "
        f"mean_rmse={gate['mean_rmse']:.4f} "
        f"spread={gate['plcc_spread']:.4f}",
        flush=True,
    )

    # Train the ship checkpoint on the full corpus once the LOSO
    # report is in. Per ADR-0303 / fr_regressor_v3 §Training recipe
    # the LOSO fold *is* the gate — the ship checkpoint goes on the
    # entire corpus.
    if args.no_export:
        print(f"[{args.log_prefix}] --no-export: skipping ONNX + manifest write", flush=True)
        return 0
    if split_indices is None:
        ship_features = features
        ship_encoder = encoder
        ship_mos = mos
    else:
        ship_idx = split_indices[0]
        ship_features = features[ship_idx]
        ship_encoder = encoder[ship_idx]
        ship_mos = mos[ship_idx]
    ship_model = _train_full(
        features=ship_features,
        encoder=ship_encoder,
        mos=ship_mos,
        epochs=epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        weight_decay=args.weight_decay,
        seed=args.seed,
        n_features=features.shape[1],
        device=args.device,
    )
    n_params = _count_parameters(ship_model)
    sha256 = _export_onnx(ship_model, args.out_onnx, n_features=features.shape[1])
    run_provenance = build_run_provenance(
        entrypoint=args.run_entrypoint,
        repo_root=REPO_ROOT,
        argv=run_argv,
        args=args,
        inputs={
            "konvid_1k": args.konvid_1k,
            "konvid_150k": args.konvid_150k,
            "feature_jsonl": args.feature_jsonl,
            "feature_parquet": args.feature_parquet,
            "display_profile_json": args.display_profile_json,
        },
        outputs={
            "onnx": args.out_onnx,
            "card": args.out_card,
            "manifest": args.out_manifest,
        },
        exclude_args={"run_entrypoint", "run_argv_json"},
    )
    if args.run_entrypoint.resolve() != SCRIPT_PATH:
        run_provenance["shared_trainer"] = describe_path(SCRIPT_PATH, repo_root=REPO_ROOT)
    manifest = _build_manifest(
        onnx_path=args.out_onnx,
        sha256=sha256,
        model_id=args.model_id,
        feature_schema=args.feature_schema,
        feature_columns=feature_columns,
        folds=folds_report,
        gate=gate,
        feature_mean=feature_mean,
        feature_std=feature_std,
        n_params=n_params,
        smoke=synthetic,
        n_rows=int(ship_features.shape[0]),
        epochs=epochs,
        seed=args.seed,
        display_profile=display_profile if uses_display_profile else None,
        run_provenance=run_provenance,
    )
    write_manifest_json(args.out_manifest, manifest)
    wall_s = time.time() - t0
    print(
        f"[{args.log_prefix}] wrote {args.out_onnx} ({n_params} params, "
        f"sha256={sha256[:16]}…); manifest={args.out_manifest.name}; "
        f"wall={wall_s:.1f}s",
        flush=True,
    )
    return 0


__all__ = [
    "CANONICAL_6",
    "CHUG_HDR_DISPLAY_FEATURES",
    "CHUG_HDR_DISPLAY_FEATURE_COLUMNS",
    "CHUG_HDR_FEATURE_COLUMNS",
    "CHUG_HDR_METADATA_FEATURES",
    "CHUG_HDR_TEMPORAL_FEATURES",
    "DEFAULT_MODEL_ID",
    "ENCODER_VOCAB_V4",
    "ENCODER_VOCAB_V4_VERSION",
    "EXTRA_FEATURES",
    "FEATURE_COLUMNS",
    "FEATURE_SCHEMAS",
    "FEATURE_SCHEMA_CHUG_HDR_DISPLAY_V1",
    "FEATURE_SCHEMA_CHUG_HDR_WIDE_V1",
    "FEATURE_SCHEMA_KONVID_V1",
    "GATE_MEAN_PLCC",
    "GATE_RMSE_MAX",
    "GATE_SPREAD_MAX",
    "GATE_SROCC",
    "MOS_MAX",
    "MOS_MIN",
    "N_ENCODERS",
    "N_FEATURES",
    "SYNTHETIC_GATE_PLCC",
    "CorpusArrays",
    "DisplayProfile",
    "_evaluate_gate",
    "_feature_columns_for_schema",
    "_heldout_split_indices",
    "_kfold_indices",
    "_load_corpus",
    "_load_corpus_arrays",
    "_load_display_profile",
    "_normalise_display_profile_payload",
    "_resolve_device",
    "_row_to_features",
    "_synthesize_corpus",
    "main",
]


if __name__ == "__main__":
    sys.exit(main())
