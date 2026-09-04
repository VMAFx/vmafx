# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Per-frame feature extraction via the libvmaf CLI.

The extractor mirrors what ``vmaf_v0.6.1`` consumes::

    DEFAULT_FEATURES = ("adm2", "vif_scale0", "vif_scale1",
                        "vif_scale2", "vif_scale3", "motion2")

It calls the local CPU build of ``vmaf`` (default
``core/build-cpu/tools/vmaf``) in JSON mode, parses the per-frame
metrics, and returns a NumPy ``(n_frames, n_features)`` matrix plus a 4-stat aggregate
``(mean, p10, p90, std)`` per feature for clip-level pooling.

The binary path can be overridden via ``VMAF_BIN`` or the explicit
``vmaf_binary`` argument. If the binary does not exist, the extractor
raises :class:`RuntimeError` with the canonical build instructions
rather than emitting a misleading subprocess error.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np

DEFAULT_FEATURES: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)

DEFAULT_VMAF_BINARY = Path("core") / "build-cpu" / "tools" / "vmaf"

# Full feature set the fork's extractors can produce (per Research-0026,
# extended by ADR-0559 to include SpEED chroma/temporal features).
# Excludes lpips (DNN-based, expensive) and float_moment (image
# statistics, not quality-relevant). The 26 features below cover the
# bit-exact CPU + AVX2 + AVX-512 + NEON + (mostly) CUDA / SYCL / Vulkan
# extractors registered in core/src/feature/.  The 4 speed_* features
# are CPU-only until GPU twins land (ADR-0557, ADR-0558).
FULL_FEATURES: tuple[str, ...] = (
    # ADM (5 features) — `adm2` is the detail-loss aggregate; the
    # bare `adm` is not emitted as a separate JSON metric by the
    # integer extractor. `adm_scale0..3` carry per-scale signal.
    "adm2",
    "adm_scale0",
    "adm_scale1",
    "adm_scale2",
    "adm_scale3",
    # VIF (4 features) — only per-scale entries are emitted by the
    # integer extractor. The bare `vif` aggregate is not a separate
    # metric.
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    # Motion (3 features) — frame-to-frame energy + 5-frame-window
    # variant (Netflix#1486 / motion_v2).
    "motion",
    "motion2",
    "motion3",
    # PSNR (3 features) — per-plane peak SNR.
    "psnr_y",
    "psnr_cb",
    "psnr_cr",
    # Structural similarity (2 features).
    "float_ssim",
    "float_ms_ssim",
    # Perceptual (4 features) — banding, color difference, HVS-weighted
    # PSNR, modern SSIMULACRA 2 (libjxl).
    "cambi",
    "ciede2000",
    "psnr_hvs",
    "ssimulacra2",
    # SpEED chroma/temporal (4 features) — CPU-only extractors from
    # core/src/feature/speed.c (Netflix speed_ported branch, ported to
    # fork per ADR-0559).  Required by the anticipated Netflix HDR VMAF model.
    # GPU twins are tracked in ADR-0557 (CUDA) and ADR-0558 (HIP); until
    # those land these features are always extracted on the CPU residual pass.
    "speed_temporal",
    "speed_chroma_u",
    "speed_chroma_v",
    "speed_chroma_uv",
    # ADM3 (1 feature) — integer_adm3 emitted by adm extractor; required by v1 models (ADR-1173).
    "adm3",
)

# Predefined feature-set names for CLI / API ergonomics. Custom subsets
# can still be passed through the ``features=`` keyword argument
# directly. See Research-0026 for the rationale behind ``full``'s
# exclusions (lpips, float_moment).
FEATURE_SETS: dict[str, tuple[str, ...]] = {
    "canonical": DEFAULT_FEATURES,
    "full": FULL_FEATURES,
}


def resolve_feature_set(name: str) -> tuple[str, ...]:
    """Look up a named feature set or raise ``ValueError`` with the
    list of registered names. Pass-through for the canonical names
    used by ``ai/train/train_combined.py``'s ``--feature-set`` flag.
    """
    try:
        return FEATURE_SETS[name]
    except KeyError as e:
        raise ValueError(f"unknown feature set {name!r}; valid: {sorted(FEATURE_SETS)}") from e


# Mapping from the metric names the JSON output uses to the extractor
# names the CLI's ``--feature`` flag wants. Mirrors ``feature_dump.py``
# but kept private to this module so tests can exercise it cheaply.
_METRIC_TO_EXTRACTOR: dict[str, str] = {
    # ADM
    "adm2": "adm",
    "adm_scale0": "adm",
    "adm_scale1": "adm",
    "adm_scale2": "adm",
    "adm_scale3": "adm",
    "adm3": "adm",
    # VIF
    "vif_scale0": "vif",
    "vif_scale1": "vif",
    "vif_scale2": "vif",
    "vif_scale3": "vif",
    # Motion. ``motion3`` lives in integer_motion_v2 — the CLI
    # extractor name is ``motion_v2`` (not ``motion3``); the JSON
    # output key is ``integer_motion3`` which ``_lookup`` finds via
    # the ``integer_`` fallback.
    "motion": "motion",
    "motion2": "motion",
    "motion3": "motion_v2",
    # PSNR (per-plane Y/Cb/Cr emitted by the ``psnr`` extractor).
    "psnr_y": "psnr",
    "psnr_cb": "psnr",
    "psnr_cr": "psnr",
    # SSIM / MS-SSIM (float variants are the canonical SIMD'd path).
    "ssim": "float_ssim",
    "float_ssim": "float_ssim",
    "float_ms_ssim": "float_ms_ssim",
    # Perceptual
    "cambi": "cambi",
    "ciede2000": "ciede",
    "psnr_hvs": "psnr_hvs",
    "psnr_hvs_y": "psnr_hvs",
    "psnr_hvs_cb": "psnr_hvs",
    "psnr_hvs_cr": "psnr_hvs",
    "ssimulacra2": "ssimulacra2",
    # SpEED chroma/temporal — CPU-only (ADR-0559; GPU twins in ADR-0557/0558).
    # Short alias names registered in core/src/feature/alias.c.
    "speed_temporal": "speed_temporal",
    "speed_chroma_u": "speed_chroma",
    "speed_chroma_v": "speed_chroma",
    "speed_chroma_uv": "speed_chroma",
}


@dataclass
class FeatureExtractionResult:
    """Per-clip feature payload produced by :func:`extract_features`."""

    feature_names: tuple[str, ...]
    per_frame: np.ndarray  # shape (n_frames, n_features), float32
    n_frames: int

    def to_jsonable(self) -> dict:
        return {
            "feature_names": list(self.feature_names),
            "per_frame": self.per_frame.tolist(),
            "n_frames": int(self.n_frames),
        }

    @classmethod
    def from_jsonable(cls, payload: dict) -> "FeatureExtractionResult":
        per_frame = np.asarray(payload["per_frame"], dtype=np.float32)
        return cls(
            feature_names=tuple(payload["feature_names"]),
            per_frame=per_frame,
            n_frames=int(payload["n_frames"]),
        )


def default_vmaf_binary() -> Path:
    env = os.environ.get("VMAF_BIN")
    if env:
        return Path(env)
    return DEFAULT_VMAF_BINARY


def _ensure_binary(binary: Path) -> None:
    if not binary.is_file():
        raise RuntimeError(
            "libvmaf CLI not found at "
            f"{binary}. Build it first with `meson setup core/build-cpu "
            "&& ninja -C core/build-cpu`, or set $VMAF_BIN to point at an "
            "existing binary."
        )


def _extractors_for(metrics: tuple[str, ...]) -> list[str]:
    seen: list[str] = []
    for m in metrics:
        ex = _METRIC_TO_EXTRACTOR.get(m, m)
        if ex not in seen:
            seen.append(ex)
    return seen


def _lookup(metrics: dict, name: str):
    """libvmaf may emit ``integer_<name>`` for fixed-point kernels."""
    if name in metrics:
        return metrics[name]
    val = metrics.get(f"integer_{name}")
    if val is not None:
        return val
    prefix = f"integer_{name}_"
    for k, v in metrics.items():
        if k.startswith(prefix) and v is not None:
            return v
    return None


def _run_vmaf_json(
    binary: Path,
    ref: Path,
    dis: Path,
    width: int,
    height: int,
    *,
    pix_fmt: str = "420",
    bitdepth: int = 8,
    features: tuple[str, ...] = DEFAULT_FEATURES,
    extra_args: tuple[str, ...] = (),
) -> dict:
    feat_args: list[str] = []
    for f in _extractors_for(features):
        feat_args += ["--feature", f]
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
        out_path = Path(tf.name)
    try:
        cmd = [
            str(binary),
            "-r",
            str(ref),
            "-d",
            str(dis),
            "-w",
            str(width),
            "-h",
            str(height),
            "-p",
            pix_fmt,
            "-b",
            str(bitdepth),
            "--json",
            "-o",
            str(out_path),
            *feat_args,
            *extra_args,
        ]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        return json.loads(out_path.read_text())
    finally:
        out_path.unlink(missing_ok=True)


def extract_features(
    ref: Path,
    dis: Path,
    width: int,
    height: int,
    *,
    features: tuple[str, ...] = DEFAULT_FEATURES,
    vmaf_binary: Path | None = None,
    pix_fmt: str = "420",
    bitdepth: int = 8,
) -> FeatureExtractionResult:
    """Extract per-frame ``features`` from ``(ref, dis)``.

    Returns:
        A :class:`FeatureExtractionResult` whose ``per_frame`` matrix has
        one row per decoded frame and ``len(features)`` columns. Missing
        metrics are reported as NaN — callers should mask or drop those
        frames before training.
    """
    binary = Path(vmaf_binary) if vmaf_binary is not None else default_vmaf_binary()
    _ensure_binary(binary)

    doc = _run_vmaf_json(
        binary,
        ref,
        dis,
        width,
        height,
        pix_fmt=pix_fmt,
        bitdepth=bitdepth,
        features=features,
    )
    rows: list[list[float]] = []
    for frame in doc.get("frames", []):
        fmetrics = frame.get("metrics", {})
        row = []
        for f in features:
            v = _lookup(fmetrics, f)
            row.append(float("nan") if v is None else float(v))
        rows.append(row)
    arr = (
        np.asarray(rows, dtype=np.float32)
        if rows
        else np.zeros((0, len(features)), dtype=np.float32)
    )
    return FeatureExtractionResult(
        feature_names=tuple(features),
        per_frame=arr,
        n_frames=arr.shape[0],
    )


def aggregate_clip_stats(features: np.ndarray) -> np.ndarray:
    """Reduce ``(n_frames, n_features)`` to ``(4 * n_features,)``.

    Returns one row containing ``[mean..., p10..., p90..., std...]`` —
    matches what the existing SVR consumes after temporal pooling.
    NaN frames are ignored via ``np.nanmean`` etc.; if a feature is
    entirely NaN the corresponding stat is also NaN (caller's problem).
    """
    if features.ndim != 2:
        raise ValueError(f"expected 2-D features, got shape {features.shape}")
    if features.shape[0] == 0:
        return np.full((4 * features.shape[1],), np.nan, dtype=np.float32)
    mean = np.nanmean(features, axis=0)
    p10 = np.nanpercentile(features, 10, axis=0)
    p90 = np.nanpercentile(features, 90, axis=0)
    std = np.nanstd(features, axis=0)
    return np.concatenate([mean, p10, p90, std]).astype(np.float32)
