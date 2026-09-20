#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Extract FULL_FEATURES (Research-0026) from KoNViD-150k-A using FR-from-NR adapter.

KoNViD-150k-A (K150K-A) is a no-reference corpus: each clip carries a human MOS
label but no reference video.  To run full-reference libvmaf extractors we use the
FR-from-NR adapter pattern (ADR-0346): the same decoded YUV is fed as *both*
reference and distorted.  This makes all difference-based metrics (ciede2000,
psnr_hvs, ADM, VIF, SSIM) measure "identity" — they return null / floor at their
trivial value — while content-sensitive metrics (cambi, motion, vmaf teacher) remain
informative.  The NaN columns are expected and documented in ADR-0362.

``vmaf`` column: computed via the teacher model (defaulting to the fork default
model per ADR-1168/ADR-1173).  The model is SDR-trained and mis-calibrated on PQ HDR clips;
HDR inputs should be interpreted as a relative comparison baseline only, not as an
absolute quality prediction.  Replace with the Netflix HDR model when it ships.  The
~5–10 % CUDA wall-clock overhead of the model dispatch is acknowledged in
Research-0135 as an acceptable trade-off for preserving the vmaf relationship across
bitrate-ladder rungs.  See Research-0135 for the full analysis and the Option A vs
Option B decision matrix (supersedes PR #898 Option A / Research-0135 draft).

Output: ``runs/full_features_k150k.parquet`` (one row per clip, gitignored).

Schema (46 columns, parquet schema version v2):

    clip_name, mos,
    <21 features>_mean, <21 features>_std    (42 feature columns)

Feature columns follow the FEATURE_NAMES tuple order exactly (column-order-locked;
see ai/AGENTS.md §K150K-A corpus extraction invariants before reordering).

Restartability: a ``.done`` checkpoint file (one clip name per line, append-only)
lets interrupted runs resume without re-processing already-extracted clips.

I/O strategy (perf win — Research-0135):
  Rows are accumulated in memory throughout the run and written to a JSONL staging
  file (``<out>.rows.jsonl``) on every completion.  The parquet is written **once**
  at the end.  This eliminates the O(N²) read-concat-write pattern that the
  per-``--flush-every``-clips flush incurred on long runs.  The ``.done`` checkpoint
  remains the primary restartability signal; the JSONL staging file handles recovery
  of in-memory rows after an unclean exit.

ffprobe skip (Win 2 — Research-0135):
  When ``--metadata-jsonl`` is provided and the sidecar contains
  ``chug_width_manifest``, ``chug_height_manifest``, and
  ``chug_framerate_manifest`` for a clip, ffprobe is skipped for that clip.
  The pixel format is inferred from ``chug_bit_depth`` (10 → ``yuv420p10le``,
  else ``yuv420p``) or defaults to ``yuv420p``.  ffprobe remains necessary for
  clips not covered by the sidecar.

tmpfs scratch (Win 3 — Research-0135):
  When ``/dev/shm`` is writable and has >=20 GiB free, temporary YUV files
  are written there instead of the OS temp directory.  This eliminates NVMe
  I/O for the intermediate per-clip raw YUV (~1.5 GiB per 1080p 30 fps 240-
  frame clip) and reduces the per-clip wall time by an estimated 5–15 s on
  NVMe-bound hosts.  Pass ``--scratch-dir`` to override auto-selection.

Parallelism (ADR-0382): clips are dispatched to a
``concurrent.futures.ProcessPoolExecutor`` with ``--threads-cuda`` workers
(default 8).  Each worker independently decodes one clip to a worker-private YUV
scratch file, scores it via the selected fork binary, aggregates frames, removes
the YUV immediately, and returns the row dict.  The main process collects
results, writes the ``.done`` checkpoint, and flushes the parquet.  Worker
isolation ensures no shared mutable state and avoids backend context conflicts.

Usage::

    python ai/scripts/extract_k150k_features.py \\
        --clips-dir .workingdir2/konvid-150k/k150ka_extracted \\
        --scores   .workingdir2/konvid-150k/k150ka_scores.csv  \\
        --out      runs/full_features_k150k.parquet

Smoke-test (100 clips, 8 workers)::

    python ai/scripts/extract_k150k_features.py --limit 100 --threads-cuda 8

Resume command (same invocation; already-done clips are skipped)::

    python ai/scripts/extract_k150k_features.py

Hardware: the default path remains CPU-oriented because K150K-A 540p 5s clips are
CPU-bound in aggregate and the CUDA binary provides no per-clip speedup for that
geometry (ADR-0382).  Larger local corpora such as CHUG can opt into a CUDA-capable
``--vmaf-bin``; in that mode the script uses explicit CUDA feature names for the
stable GPU pass and ``--cpu-vmaf-bin`` for residual CPU-only extractors.  The
system ``/usr/local/bin/vmaf`` v3.0.0 lacks ssimulacra2 and motion_v2 — it must NOT
be used for this pipeline.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
from _script_bootstrap import bootstrap_ai_script
from ai.data.scores import DEFAULT_MODEL, resolve_teacher_model

from aiutils.run_manifest import build_run_provenance, write_manifest_json

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True, include_vmaf_tune_src=True)
REPO_ROOT = _SCRIPT_PATHS.repo_root
DEFAULT_CHUG_SPLIT_SEED = "chug-hdr-v1"


# ---------------------------------------------------------------------------
# Feature / extractor configuration (column-order-locked per ai/AGENTS.md)
# ---------------------------------------------------------------------------

# Extractor names passed via --feature to the vmaf CLI.
EXTRACTOR_NAMES: tuple[str, ...] = (
    "adm",
    "vif",
    "motion",
    "motion_v2",
    "psnr",
    "float_ssim",
    "float_ms_ssim",
    "cambi",
    "ciede",
    "psnr_hvs",
    "ssimulacra2",
)

CUDA_EXTRACTOR_NAMES: tuple[str, ...] = (
    "adm_cuda",
    "vif_cuda",
    "motion_cuda",
    "motion_v2_cuda",
    "psnr_cuda",
    "ciede_cuda",
    "float_ssim_cuda=scale=1",
    "float_ms_ssim_cuda",
    "psnr_hvs_cuda",
    # cambi_cuda intentionally not promoted: the CUDA extractor
    # segfaults on every input on the rebuilt 2026-05-15 binary
    # (Issue #857). cambi stays on the CPU residual pass below
    # until that's fixed.
    # float_ssim_cuda needs an explicit scale=1: libvmaf v1 supports
    # scale=1 only and refuses on auto-detected scale=4 at 1080p
    # ("libvmaf ERROR ssim_cuda: v1 supports scale=1 only").
)
# ssimulacra2 omitted from K150K/CHUG self-vs-self extraction — produces ~100 constant
# for identity pairs (ref == distorted), yielding zero training signal while consuming
# ~30-50% of GPU time per clip. Use CPU ssimulacra2 extractor for FR pairs where it
# remains informative (ADR-0431).

# 2026-05-15: float_ssim promoted from the CPU residual pass to the
# CUDA primary pass (it has shipped a CUDA implementation since
# core/src/feature/cuda/float_ssim_cuda.c landed). cambi stays
# on this CPU residual pass — its CUDA twin segfaults (Issue #857).
CUDA_CPU_RESIDUAL_EXTRACTOR_NAMES: tuple[str, ...] = (
    "cambi",
    # speed_temporal + speed_chroma added 2026-05-15. Lawrence's HDR
    # recipe (`hdr_custom_features.py`, Slack) called for both signals
    # in the K150K/CHUG feature set; they are CPU-only extractors
    # (no CUDA twin yet) so they ride the residual pass.
    "speed_temporal",
    "speed_chroma",
)

# Canonical 25-feature output columns (Research-0026, parquet schema v2 +
# 2026-05-15 speed-feature addition).
# WARNING: column order is locked — do not reorder without incrementing the
# parquet schema version and updating ai/AGENTS.md. New columns may only be
# appended at the END of the tuple, never inserted.
# ssimulacra2 dropped: in self-vs-self (FR-from-NR) mode it returns ~100 for every
# frame regardless of input (zero training signal); see ADR-0431 and the docstring
# near CUDA_EXTRACTOR_NAMES above.
FEATURE_NAMES: tuple[str, ...] = (
    "adm2",
    "adm_scale0",
    "adm_scale1",
    "adm_scale2",
    "adm_scale3",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion",
    "motion2",
    "motion3",
    "psnr_y",
    "psnr_cb",
    "psnr_cr",
    "float_ssim",
    "float_ms_ssim",
    "cambi",
    "ciede2000",
    "psnr_hvs",
    "vmaf",
    # 2026-05-15 additions — appended at end to preserve column order.
    # Source: lawrence's hdr_custom_features.py recipe (Slack).
    "speed_temporal",
    "speed_chroma_u",
    "speed_chroma_v",
    "speed_chroma_uv",
    # 2026-09-04 addition — adm3 required by v1 models (ADR-1173).
    "adm3",
)

# Map feature names to their JSON key(s) in libvmaf output.  libvmaf may emit
# ``integer_<name>`` for fixed-point kernels; try both in order.
_METRIC_ALIASES: dict[str, tuple[str, ...]] = {
    "adm2": ("adm2", "integer_adm2"),
    "adm_scale0": ("adm_scale0", "integer_adm_scale0"),
    "adm_scale1": ("adm_scale1", "integer_adm_scale1"),
    "adm_scale2": ("adm_scale2", "integer_adm_scale2"),
    "adm_scale3": ("adm_scale3", "integer_adm_scale3"),
    "vif_scale0": ("vif_scale0", "integer_vif_scale0"),
    "vif_scale1": ("vif_scale1", "integer_vif_scale1"),
    "vif_scale2": ("vif_scale2", "integer_vif_scale2"),
    "vif_scale3": ("vif_scale3", "integer_vif_scale3"),
    "motion": ("motion", "integer_motion"),
    "motion2": ("motion2", "integer_motion2"),
    "motion3": ("motion3", "integer_motion3"),
    "psnr_y": ("psnr_y", "integer_psnr_y"),
    "psnr_cb": ("psnr_cb", "integer_psnr_cb"),
    "psnr_cr": ("psnr_cr", "integer_psnr_cr"),
    "float_ssim": ("float_ssim",),
    "float_ms_ssim": ("float_ms_ssim",),
    "cambi": ("cambi",),
    "ciede2000": ("ciede2000",),
    "psnr_hvs": ("psnr_hvs",),
    "vmaf": ("vmaf",),
    # 2026-05-15 additions — short aliases registered in
    # core/src/feature/alias.c.
    "speed_temporal": (
        "speed_temporal",
        "Speed_temporal_feature_speed_temporal_score",
    ),
    "speed_chroma_u": (
        "speed_chroma_u",
        "Speed_chroma_feature_speed_chroma_u_score",
    ),
    "speed_chroma_v": (
        "speed_chroma_v",
        "Speed_chroma_feature_speed_chroma_v_score",
    ),
    "speed_chroma_uv": (
        "speed_chroma_uv",
        "Speed_chroma_feature_speed_chroma_uv_score",
    ),
    "adm3": ("adm3", "integer_adm3"),
}

# ---------------------------------------------------------------------------
# YUV decode / geometry helpers
# ---------------------------------------------------------------------------


def _geometry_from_sidecar(meta: dict | None) -> tuple[int, int, str, str] | None:
    """Extract (width, height, pix_fmt, fps) from a CHUG JSONL sidecar row.

    Returns ``None`` if ``meta`` is ``None`` or if any required geometry field
    is absent, so the caller can fall back to ffprobe.  Required fields:
    ``chug_width_manifest``, ``chug_height_manifest``,
    ``chug_framerate_manifest``.  The pixel format is inferred from
    ``chug_bit_depth`` (10 → ``yuv420p10le``, else ``yuv420p``).
    """
    if meta is None:
        return None
    w = meta.get("chug_width_manifest")
    h = meta.get("chug_height_manifest")
    fps = meta.get("chug_framerate_manifest")
    if w is None or h is None or fps is None:
        return None
    bit_depth = meta.get("chug_bit_depth")
    pix_fmt = "yuv420p10le" if bit_depth == 10 else "yuv420p"
    return int(w), int(h), pix_fmt, str(fps)


def _probe_geometry(mp4: Path) -> tuple[int, int, str, str, dict[str, str]]:
    """Return (width, height, pix_fmt, fps, color_meta) for the first video stream.

    The 5th element ``color_meta`` is a dict with keys
    ``color_primaries`` / ``color_transfer`` / ``color_space`` (each
    optional, populated when ffprobe surfaces the field).  Callers use
    this to decide HDR-aware feature options (CAMBI ``eotf=pq``,
    motion ``motion_fps_weight``).
    """
    proc = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,pix_fmt,r_frame_rate,color_primaries,color_transfer,color_space",
            "-of",
            "json",
            str(mp4),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    s = json.loads(proc.stdout)["streams"][0]
    pix_fmt: str = s.get("pix_fmt", "yuv420p")
    # Normalise to libvmaf-safe pixel formats.
    pix_fmt = "yuv420p10le" if "10" in pix_fmt else "yuv420p"
    color_meta = {
        "color_primaries": s.get("color_primaries", "") or "",
        "color_transfer": s.get("color_transfer", "") or "",
        "color_space": s.get("color_space", "") or "",
    }
    return (
        int(s["width"]),
        int(s["height"]),
        pix_fmt,
        s.get("r_frame_rate", "25/1"),
        color_meta,
    )


def _is_hdr_source(pix_fmt: str, color_meta: dict[str, str]) -> bool:
    """True when the source is HDR (PQ or HLG transfer characteristics).

    A source needs both:
    1. 10-bit (or higher) pix_fmt — SDR 8-bit can't be HDR.
    2. PQ (``smpte2084``) or HLG (``arib-std-b67``) transfer characteristics
       OR BT.2020 primaries (a weaker fallback when transfer is absent).

    Returns ``False`` on missing metadata to fail-safe to SDR defaults
    rather than mis-applying HDR options to an SDR source.
    """
    if "10" not in pix_fmt and "12" not in pix_fmt and "16" not in pix_fmt:
        return False
    transfer = color_meta.get("color_transfer", "").lower()
    if transfer in ("smpte2084", "arib-std-b67", "bt2020-10", "bt2020-12"):
        return True
    primaries = color_meta.get("color_primaries", "").lower()
    return primaries in ("bt2020", "bt2020nc", "bt2020c")


def _parse_fps(fps_str: str) -> float:
    """Parse the ffprobe ``r_frame_rate`` string ``"num/den"`` into a float.

    Returns 0.0 on parse failure so callers can fall back to a default.
    """
    if "/" in fps_str:
        try:
            num, den = fps_str.split("/", 1)
            den_f = float(den)
            return float(num) / den_f if den_f > 0 else 0.0
        except (ValueError, ZeroDivisionError):
            return 0.0
    try:
        return float(fps_str)
    except ValueError:
        return 0.0


def _motion_fps_weight(fps: float) -> float:
    """Compute the libvmaf ``motion_fps_weight`` for a given source fps.

    Motion features measure per-frame absolute differences. At 50/60 fps
    the per-frame delta on the same physical motion is ~half what it is
    at 25/30 fps; at 120 fps it's ~quarter. The libvmaf
    ``motion[_v2]_fps_weight`` knob multiplies the score to compensate.

    The reference fps for motion features is 30 (per Netflix golden
    fixtures).  Returns 1.0 (no correction) for any fps in [24, 32];
    otherwise returns ``30 / fps`` clamped to ``[0.25, 4.0]``.
    """
    if fps <= 0:
        return 1.0
    if 24.0 <= fps <= 32.0:
        return 1.0
    weight = 30.0 / fps
    return max(0.25, min(4.0, weight))


# Per-extractor option support. CUDA twins ship a reduced VmafOption
# table vs their CPU counterparts (verified against
# core/src/feature/cuda/{integer_cambi_cuda,integer_ms_ssim_cuda,
# integer_motion_cuda}.c); options not present here are silently
# dropped from the --feature arg rather than triggering
# "problem loading feature extractor" at runtime.
_FEATURE_OPTION_SUPPORT: dict[str, frozenset[str]] = {
    "cambi": frozenset({"eotf", "cambi_eotf", "full_ref"}),
    "cambi_cuda": frozenset({"eotf", "cambi_eotf"}),
    "float_ms_ssim": frozenset({"enable_db", "clip_db", "enable_lcs"}),
    "float_ms_ssim_cuda": frozenset({"enable_lcs"}),
    "motion": frozenset({"motion_fps_weight"}),
    "motion_cuda": frozenset({"motion_fps_weight"}),
    "motion_v2": frozenset({"motion_fps_weight"}),
    "motion_v2_cuda": frozenset({"motion_fps_weight"}),
}


def _feature_arg(extractor: str, is_hdr: bool, motion_fps_weight: float) -> str:
    """Build the ``--feature`` argument value for one extractor.

    Per core/tools/cli_parse.c the CLI grammar is
    ``EXTRACTOR=key1=val1:key2=val2``: ``strsep(&optarg, "=")`` consumes
    the extractor name first, then ``:`` separates the ``key=value``
    pairs.  The leading literal ``name=`` token is NOT part of the
    grammar — it parses as a feature called "name" with bad options
    and trips ``problem loading feature extractor: name``.

    Returns ``"<extractor>=k1=v1:k2=v2"`` when HDR-aware options apply
    AND the extractor advertises support for them; returns the bare
    ``<extractor>`` name otherwise (preserving pre-fix behaviour for
    SDR sources and silently dropping CUDA-unsupported options).

    HDR-aware options follow lawrence's 2026-05-15 guidance:
    - CAMBI: ``eotf=pq`` (HDR PQ visibility thresholds, not SDR);
      ``full_ref=true`` (FR-CAMBI matches the script's ref==dis
      topology; the CUDA twin doesn't expose this option, so the
      whitelist drops it for ``cambi_cuda``).
    - MS_SSIM: ``enable_db=false`` (linear scale per recipe);
      the CUDA twin doesn't expose this option either.
    - motion / motion_v2 (both CPU and CUDA): ``motion_fps_weight``
      when fps != 30.
    """
    desired: list[tuple[str, str]] = []
    base = extractor

    if is_hdr and base in ("cambi", "cambi_cuda"):
        desired.append(("eotf", "pq"))
        desired.append(("full_ref", "true"))
    if is_hdr and base in ("float_ms_ssim", "float_ms_ssim_cuda"):
        desired.append(("enable_db", "false"))
    if motion_fps_weight != 1.0 and base in (
        "motion",
        "motion_cuda",
        "motion_v2",
        "motion_v2_cuda",
    ):
        desired.append(("motion_fps_weight", f"{motion_fps_weight:.4f}"))

    supported = _FEATURE_OPTION_SUPPORT.get(base, frozenset())
    opts = [f"{k}={v}" for k, v in desired if k in supported]
    if not opts:
        return base
    return f"{base}=" + ":".join(opts)


def _decode_to_yuv(mp4: Path, yuv_path: Path, pix_fmt: str) -> None:
    """Decode ``mp4`` to raw YUV.  Writes atomically via a ``.tmp`` sibling."""
    tmp = yuv_path.with_suffix(".tmp")
    try:
        subprocess.run(
            [
                "ffmpeg",
                "-y",
                "-loglevel",
                "error",
                "-i",
                str(mp4),
                "-pix_fmt",
                pix_fmt,
                "-f",
                "rawvideo",
                str(tmp),
            ],
            check=True,
        )
        tmp.rename(yuv_path)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise


# ---------------------------------------------------------------------------
# vmaf invocation
# ---------------------------------------------------------------------------


def _build_vmaf_cmd(
    vmaf_bin: Path,
    yuv_path: Path,
    width: int,
    height: int,
    pix_fmt: str,
    out_json: Path,
    threads: int,
    extractor_names: tuple[str, ...],
    backend_args: list[str],
    is_hdr: bool = False,
    motion_fps_weight_value: float = 1.0,
) -> list[str]:
    bitdepth = "10" if "10" in pix_fmt else "8"
    feat_args: list[str] = []
    for ex in extractor_names:
        feat_args += ["--feature", _feature_arg(ex, is_hdr, motion_fps_weight_value)]

    return [
        str(vmaf_bin),
        "--reference",
        str(yuv_path),
        "--distorted",
        str(yuv_path),
        "--width",
        str(width),
        "--height",
        str(height),
        "--pixel_format",
        "420",
        "--bitdepth",
        bitdepth,
        *feat_args,
        "--threads",
        str(threads),
        *backend_args,
        "--output",
        str(out_json),
        "--json",
        "-q",
    ]


def _run_vmaf_json(
    vmaf_bin: Path,
    yuv_path: Path,
    width: int,
    height: int,
    pix_fmt: str,
    out_json: Path,
    threads: int,
    extractor_names: tuple[str, ...],
    backend_args: list[str],
    is_hdr: bool = False,
    motion_fps_weight_value: float = 1.0,
) -> list[dict]:
    """Run vmaf once and return a list of per-frame metric dicts."""
    cmd = _build_vmaf_cmd(
        vmaf_bin,
        yuv_path,
        width,
        height,
        pix_fmt,
        out_json,
        threads,
        extractor_names,
        backend_args,
        is_hdr=is_hdr,
        motion_fps_weight_value=motion_fps_weight_value,
    )
    subprocess.run(cmd, check=True, capture_output=True)
    with out_json.open() as f:
        data = json.load(f)
    return [fr["metrics"] for fr in data.get("frames", [])]


def _merge_frame_metrics(primary: list[dict], residual: list[dict]) -> list[dict]:
    """Merge per-frame metric dictionaries from two vmaf invocations."""
    frame_count = min(len(primary), len(residual))
    merged: list[dict] = []
    for idx in range(frame_count):
        row = dict(primary[idx])
        row.update(residual[idx])
        merged.append(row)
    return merged


def _run_feature_passes(
    vmaf_bin: Path,
    cpu_vmaf_bin: Path,
    yuv_path: Path,
    width: int,
    height: int,
    pix_fmt: str,
    out_json: Path,
    threads: int,
    use_cuda: bool,
    is_hdr: bool = False,
    motion_fps_weight_value: float = 1.0,
    teacher_model_arg: str | None = None,
) -> list[dict]:
    """Run vmaf feature extraction, splitting CUDA mode where required.

    The teacher model is dispatched on every invocation so that the
    ``vmaf`` JSON key is populated in the output.
    """
    m_arg = teacher_model_arg if teacher_model_arg is not None else f"version={DEFAULT_MODEL}"
    model_args: list[str] = ["--model", m_arg]

    if not use_cuda:
        return _run_vmaf_json(
            vmaf_bin,
            yuv_path,
            width,
            height,
            pix_fmt,
            out_json,
            threads,
            EXTRACTOR_NAMES,
            ["--backend", "cpu", *model_args],
            is_hdr=is_hdr,
            motion_fps_weight_value=motion_fps_weight_value,
        )

    cuda_json = out_json.with_name(out_json.stem + ".cuda.json")
    cpu_json = out_json.with_name(out_json.stem + ".cpu.json")
    try:
        cuda_frames = _run_vmaf_json(
            vmaf_bin,
            yuv_path,
            width,
            height,
            pix_fmt,
            cuda_json,
            threads,
            CUDA_EXTRACTOR_NAMES,
            ["--backend", "cuda", *model_args],
            is_hdr=is_hdr,
            motion_fps_weight_value=motion_fps_weight_value,
        )
        # CPU residual pass — kept structurally for future feature
        # additions that lack a CUDA implementation. As of 2026-05-15
        # the residual is empty (CAMBI + float_ssim got promoted to
        # the CUDA pass); the call short-circuits to an empty frames
        # list without spawning a subprocess.
        if CUDA_CPU_RESIDUAL_EXTRACTOR_NAMES:
            cpu_frames = _run_vmaf_json(
                cpu_vmaf_bin,
                yuv_path,
                width,
                height,
                pix_fmt,
                cpu_json,
                threads,
                CUDA_CPU_RESIDUAL_EXTRACTOR_NAMES,
                ["--backend", "cpu"],
                is_hdr=is_hdr,
                motion_fps_weight_value=motion_fps_weight_value,
            )
            frames = _merge_frame_metrics(cuda_frames, cpu_frames)
        else:
            frames = cuda_frames
        out_json.write_text(
            json.dumps({"frames": [{"metrics": row} for row in frames]}),
            encoding="utf-8",
        )
        return frames
    finally:
        cuda_json.unlink(missing_ok=True)
        cpu_json.unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# Metric lookup and per-clip aggregation
# ---------------------------------------------------------------------------


def _lookup_metric(metrics: dict, feature: str) -> float:
    """Return the float value for ``feature`` from a libvmaf metrics dict.

    Tries each alias in order; returns NaN if none match or value is None.
    """
    for alias in _METRIC_ALIASES.get(feature, (feature,)):
        v = metrics.get(alias)
        if v is not None:
            return float(v)
    for k, val in metrics.items():
        if val is not None and (k.startswith(f"integer_{feature}_") or k.startswith(f"{feature}_")):
            return float(val)
    return float("nan")


def _aggregate_frames(frames: list[dict]) -> dict[str, float]:
    """Return nanmean and nanstd per feature across all frames."""
    if not frames:
        result: dict[str, float] = {}
        for feat in FEATURE_NAMES:
            result[f"{feat}_mean"] = float("nan")
            result[f"{feat}_std"] = float("nan")
        return result

    data: dict[str, list[float]] = {feat: [] for feat in FEATURE_NAMES}
    for m in frames:
        for feat in FEATURE_NAMES:
            data[feat].append(_lookup_metric(m, feat))

    result = {}
    for feat in FEATURE_NAMES:
        arr = np.array(data[feat], dtype=np.float64)
        valid = arr[~np.isnan(arr)]
        # ciede2000 and psnr_hvs are all-NaN for identity pairs (ref == dis,
        # ADR-0362 §Negative consequences). Handle that domain state before
        # calling NumPy's reducers so it never becomes a hidden RuntimeWarning.
        if valid.size == 0 or (np.isposinf(valid).any() and np.isneginf(valid).any()):
            mean = std = float("nan")
        elif np.isposinf(valid).any():
            mean, std = float("inf"), float("nan")
        elif np.isneginf(valid).any():
            mean, std = float("-inf"), float("nan")
        else:
            mean = float(np.mean(valid))
            std = float(np.std(valid))
        result[f"{feat}_mean"] = mean
        result[f"{feat}_std"] = std
    return result


# ---------------------------------------------------------------------------
# Checkpoint helpers
# ---------------------------------------------------------------------------


def _content_split_for(content_name: str, *, seed: str = DEFAULT_CHUG_SPLIT_SEED) -> str:
    key = f"{seed}\0{content_name}".encode("utf-8")
    digest = hashlib.blake2s(key, digest_size=8).digest()
    value = int.from_bytes(digest, "big") / float(1 << 64)
    if value < 0.80:
        return "train"
    if value < 0.90:
        return "val"
    return "test"


def detect_fr_corpus_misuse(meta_by_clip: dict[str, dict[str, Any]]) -> dict[str, Any]:
    """Detect a real-reference corpus accidentally sent through the NR adapter."""
    ref_count = 0
    dis_count = 0
    by_content: dict[str, dict[str, int]] = {}
    for meta in meta_by_clip.values():
        classified = _classify_chug_row(meta)
        if classified is None:
            continue
        content, is_ref = classified
        bucket = by_content.setdefault(content, {"ref": 0, "dis": 0})
        if is_ref:
            ref_count += 1
            bucket["ref"] += 1
        else:
            dis_count += 1
            bucket["dis"] += 1
    mixed_groups = [
        content
        for content, counts in by_content.items()
        if counts["ref"] >= 1 and counts["dis"] >= 1
    ]
    return {
        "misuse_detected": bool(mixed_groups),
        "ref_count": ref_count,
        "dis_count": dis_count,
        "content_groups_with_both": len(mixed_groups),
        "example": mixed_groups[0] if mixed_groups else None,
    }


def _classify_chug_row(meta: Any) -> tuple[str, bool] | None:
    if not isinstance(meta, dict) or meta.get("chug_ref") is None:
        return None
    raw_flag = meta["chug_ref"]
    try:
        is_ref = bool(raw_flag) if isinstance(raw_flag, bool) else bool(int(raw_flag))
    except (TypeError, ValueError):
        return None
    content = str(meta.get("chug_content_name") or "").strip()
    return (content, is_ref) if content else None


def _load_jsonl_metadata(path: Path | None, *, split_seed: str) -> dict[str, dict[str, Any]]:
    """Load optional CHUG/K150K JSONL side metadata keyed by clip basename."""
    if path is None or not path.is_file():
        return {}
    keep = (
        "mos_raw_0_100",
        "chug_video_id",
        "chug_ref",
        "chug_name",
        "chug_bitladder",
        "chug_resolution",
        "chug_bitrate_label",
        "chug_orientation",
        "chug_framerate_manifest",
        "chug_content_name",
        "chug_height_manifest",
        "chug_width_manifest",
    )
    out: dict[str, dict[str, Any]] = {}
    with path.open("r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            src = Path(str(row.get("src") or row.get("filename") or "")).name
            if not src:
                continue
            meta = {key: row[key] for key in keep if key in row}
            split = str(row.get("split") or "").strip().lower()
            content = str(row.get("chug_content_name") or "").strip()
            if split not in {"train", "val", "test"} and content:
                split = _content_split_for(content, seed=split_seed)
            if split in {"train", "val", "test"}:
                meta["split"] = split
                meta["chug_split_key"] = content or src
                meta["chug_split_policy"] = "content-name-blake2s-80-10-10"
            out[src] = meta
    return out


def _load_done_set(done_path: Path) -> set[str]:
    """Load the set of already-processed clip names from the checkpoint file."""
    if not done_path.is_file():
        return set()
    with done_path.open() as f:
        return {line.strip() for line in f if line.strip()}


def _append_done(done_path: Path, clip_name: str) -> None:
    """Append a clip name to the checkpoint file (append-only, one per line)."""
    with done_path.open("a") as f:
        f.write(clip_name + "\n")


# ---------------------------------------------------------------------------
# JSONL staging + at-end parquet write (Win 1, Research-0135)
# ---------------------------------------------------------------------------


def _staging_path(out_path: Path) -> Path:
    """Return the JSONL staging file path for ``out_path``.

    The staging file accumulates all completed rows during the run so that a
    crash does not lose rows that are already past the ``.done`` checkpoint.
    Written in append-only mode; converted to parquet once at the end.
    """
    return out_path.with_suffix(".rows.jsonl")


def _append_row_to_staging(staging_path: Path, row: dict) -> None:
    """Append one row to the JSONL staging file (main process only)."""
    with staging_path.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(row, allow_nan=True) + "\n")


def _load_staging_rows(staging_path: Path) -> list[dict]:
    """Load all rows from the JSONL staging file, skipping malformed lines.

    Malformed lines are tolerated (a crash can truncate the in-flight final
    line) but the count is reported via WARNING so an operator can detect
    catastrophic staging corruption rather than have it silently swallowed
    (see ADR for k150k crash-restart row loss).
    """
    if not staging_path.is_file():
        return []
    rows: list[dict] = []
    skipped = 0
    with staging_path.open("r", encoding="utf-8") as fh:
        for raw in fh:
            raw = raw.strip()
            if not raw:
                continue
            try:
                rows.append(json.loads(raw))
            except json.JSONDecodeError:
                skipped += 1
                continue
    if skipped:
        print(
            f"[k150k] WARNING: skipped {skipped} malformed line(s) in "
            f"staging file {staging_path}; recovered {len(rows)} row(s). "
            f"If skipped is large, operator should re-extract affected clips.",
            file=sys.stderr,
            flush=True,
        )
    return rows


def _write_parquet_from_rows(rows: list[dict], out_path: Path) -> None:
    """Write ``rows`` to ``out_path`` atomically, deduplicating by clip_name.

    Parquet writes happen exactly once per run.  The per-clip JSONL staging
    file is the in-run durability mechanism; this function is called only at
    the end of ``main()`` (Research-0135 Win 1).
    """
    if not rows:
        return
    df = pd.DataFrame(rows)
    df = df.drop_duplicates(subset=["clip_name"], keep="last")
    tmp = out_path.with_suffix(".tmp")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    df.to_parquet(tmp, index=False)
    tmp.rename(out_path)


def _fsync_path(path: Path) -> None:
    """Best-effort fsync of a file and its parent directory.

    Called before unlinking the JSONL staging file so a power-loss between
    parquet rename(2) and staging unlink(2) cannot leave us with neither
    the parquet nor the staging.  Errors are non-fatal (e.g. tmpfs, FUSE)
    but logged for diagnosis.
    """
    try:
        if path.is_file():
            fd = os.open(str(path), os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        # Also fsync the parent directory so rename(2) is durable.
        parent = path.parent
        if parent.is_dir():
            dfd = os.open(str(parent), os.O_RDONLY)
            try:
                os.fsync(dfd)
            finally:
                os.close(dfd)
    except OSError as exc:
        print(
            f"[k150k] WARNING: fsync of {path} failed: {exc}; "
            f"durability not guaranteed on this filesystem.",
            file=sys.stderr,
            flush=True,
        )


def _parquet_row_count(path: Path) -> int:
    """Return row count for an existing parquet output, or 0 when absent."""
    if not path.is_file():
        return 0
    try:
        return len(pd.read_parquet(path, columns=["clip_name"]))
    except Exception:
        return len(pd.read_parquet(path))


def _write_extraction_manifest(
    *,
    manifest_out: Path,
    args: argparse.Namespace,
    use_cuda: bool,
    total_clips: int,
    done_before: int,
    pending_count: int,
    recovered_rows: int,
    ok: int,
    fail: int,
    elapsed_seconds: float,
    status: str,
) -> None:
    """Write the replay manifest for a K150K/FR-from-NR table extraction."""
    done_path = args.out.with_suffix(".done")
    staging_path = _staging_path(args.out)
    payload = {
        "schema": "k150k-feature-extraction-manifest-v1",
        "status": status,
        "stats": {
            "total_clips": int(total_clips),
            "done_before": int(done_before),
            "pending_at_start": int(pending_count),
            "recovered_rows": int(recovered_rows),
            "ok": int(ok),
            "fail": int(fail),
            "parquet_rows": _parquet_row_count(args.out),
            "elapsed_seconds": float(elapsed_seconds),
            "rate_clip_per_second": float(ok / elapsed_seconds) if elapsed_seconds > 0 else 0.0,
        },
        "features": list(FEATURE_NAMES),
        "teacher_model": resolve_teacher_model(getattr(args, "vmaf_model", None)).name,
        "extractors": {
            "cpu": list(EXTRACTOR_NAMES),
            "cuda_primary": list(CUDA_EXTRACTOR_NAMES),
            "cuda_cpu_residual": list(CUDA_CPU_RESIDUAL_EXTRACTOR_NAMES),
        },
        "backend": {
            "use_cuda": bool(use_cuda),
            "workers": int(args.threads_cuda),
            "threads_per_worker": int(args.threads),
        },
        "fr_from_nr_adapter": {
            "enabled": True,
            "allow_fr_from_nr": bool(args.allow_fr_from_nr),
            "split_seed": str(args.split_seed),
        },
        "run_provenance": build_run_provenance(
            entrypoint=Path(__file__),
            repo_root=REPO_ROOT,
            argv=sys.argv[1:],
            args=args,
            inputs={
                "clips_dir": args.clips_dir,
                "scores_csv": args.scores,
                "metadata_jsonl": args.metadata_jsonl,
                "vmaf_bin": args.vmaf_bin,
                "cpu_vmaf_bin": args.cpu_vmaf_bin,
            },
            outputs={
                "parquet": args.out,
                "done": done_path,
                "staging_jsonl": staging_path,
                "manifest": manifest_out,
            },
        ),
    }
    write_manifest_json(manifest_out, payload)


# ---------------------------------------------------------------------------
# Worker (runs in a subprocess via ProcessPoolExecutor)
# ---------------------------------------------------------------------------


def _process_clip(
    mp4_str: str,
    mos: float,
    vmaf_bin_str: str,
    cpu_vmaf_bin_str: str,
    scratch_dir_str: str,
    vmaf_threads: int,
    use_cuda: bool,
    worker_id: int,
    sidecar_meta: dict | None = None,
    teacher_model_arg: str | None = None,
    teacher_model_name: str | None = None,
) -> dict:
    """Decode one clip, score it, aggregate, and return the row dict.

    Runs in a subprocess worker.  Uses a worker-private YUV path so parallel
    workers never clobber each other.  Deletes the YUV unconditionally on exit
    (success or failure) to avoid scratch-dir disk saturation.

    When ``sidecar_meta`` contains the required CHUG geometry fields
    (``chug_width_manifest``, ``chug_height_manifest``,
    ``chug_framerate_manifest``), ffprobe is skipped for that clip (Win 2,
    Research-0135).

    Returns a dict with keys: clip_name, mos, width, height, teacher_model, <feat>_mean/std.
    Raises on any failure so the caller can log and skip.
    """
    mp4 = Path(mp4_str)
    vmaf_bin = Path(vmaf_bin_str)
    cpu_vmaf_bin = Path(cpu_vmaf_bin_str)
    scratch_dir = Path(scratch_dir_str)

    # Worker-private paths — include PID + worker_id for full isolation.
    stem = f"{mp4.stem}_w{worker_id}_{os.getpid()}"
    yuv_path = scratch_dir / f"{stem}.yuv"
    out_json = scratch_dir / f"{stem}.json"

    try:
        # Win 2: if the CHUG sidecar provides complete geometry, skip ffprobe
        # entirely for that clip (Research-0135).  Only fall back to ffprobe
        # when the sidecar is absent or missing required fields.
        # When sidecar geometry is present, color_meta defaults to {} so that
        # _is_hdr_source fails-safe to SDR (its documented behaviour on
        # missing metadata — see the function docstring).
        geom = _geometry_from_sidecar(sidecar_meta)
        if geom is not None:
            width, height, pix_fmt, fps_str = geom
            color_meta: dict[str, str] = {}
        else:
            _pw, _ph, _ppf, fps_str, color_meta = _probe_geometry(mp4)
            width, height, pix_fmt = _pw, _ph, _ppf
        is_hdr = _is_hdr_source(pix_fmt, color_meta)
        fps = _parse_fps(fps_str)
        motion_w = _motion_fps_weight(fps)
        _decode_to_yuv(mp4, yuv_path, pix_fmt)
        frames = _run_feature_passes(
            vmaf_bin,
            cpu_vmaf_bin,
            yuv_path,
            width,
            height,
            pix_fmt,
            out_json,
            vmaf_threads,
            use_cuda,
            is_hdr=is_hdr,
            motion_fps_weight_value=motion_w,
            teacher_model_arg=teacher_model_arg,
        )
        if not frames:
            # A clip that decodes but yields zero scored frames aggregates to an
            # all-NaN row. Writing that row + marking the clip done (see the
            # as_completed loop) would silently drop it from the retrain corpus
            # with no retry. Honour this function's "raises on any failure"
            # contract instead, so the caller logs + skips it for a later resume.
            raise ValueError(
                f"no frames scored for {mp4.name} ({width}x{height} {pix_fmt}); "
                "vmaf produced an empty frame list"
            )
        agg = _aggregate_frames(frames)
        return {
            "clip_name": mp4.name,
            "mos": mos,
            "width": width,
            "height": height,
            "fps": fps,
            "is_hdr": is_hdr,
            "motion_fps_weight": motion_w,
            "teacher_model": teacher_model_name or DEFAULT_MODEL,
            **agg,
        }
    finally:
        yuv_path.unlink(missing_ok=True)
        out_json.unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def _add_corpus_args(ap: argparse.ArgumentParser) -> None:
    _k150k_dir = os.environ.get(
        "VMAF_KONVID_150K_DIR",
        str(Path(__file__).resolve().parents[2] / ".corpus" / "konvid-150k"),
    )
    ap.add_argument(
        "--clips-dir",
        type=Path,
        default=Path(_k150k_dir) / "k150ka_extracted",
        help=(
            "Directory containing K150K-A .mp4 clips. Override the parent "
            "dir via the ``VMAF_KONVID_150K_DIR`` env var."
        ),
    )
    ap.add_argument(
        "--scores",
        type=Path,
        default=Path(_k150k_dir) / "k150ka_scores.csv",
        help=(
            "CSV with columns video_name, video_score (MOS labels). "
            "Parent dir overridden via ``VMAF_KONVID_150K_DIR``."
        ),
    )
    ap.add_argument(
        "--metadata-jsonl",
        type=Path,
        default=None,
        help=(
            "Optional corpus JSONL sidecar. For CHUG, this preserves content, "
            "ladder, raw MOS, and deterministic split metadata in the parquet."
        ),
    )
    ap.add_argument(
        "--split-seed",
        default=DEFAULT_CHUG_SPLIT_SEED,
        help="Seed for CHUG content-level split metadata when --metadata-jsonl has no split.",
    )


def _add_binary_args(ap: argparse.ArgumentParser) -> None:
    ap.add_argument(
        "--vmaf-bin",
        type=Path,
        default=REPO_ROOT / "core" / "build-cpu" / "tools" / "vmaf",
        help=(
            "Path to the fork vmaf binary (built with ssimulacra2 + motion_v2).  "
            "Default: core/build-cpu/tools/vmaf.  Passing a CUDA-capable binary "
            "enables the split CUDA-safe feature pass plus CPU residual pass "
            "(ADR-0431)."
        ),
    )
    ap.add_argument(
        "--cpu-vmaf-bin",
        type=Path,
        default=REPO_ROOT / "core" / "build-cpu" / "tools" / "vmaf",
        help=(
            "CPU vmaf binary used for residual CPU-only feature passes when "
            "--vmaf-bin points at a CUDA-capable binary. Default: "
            "core/build-cpu/tools/vmaf."
        ),
    )


def _add_source_args(ap: argparse.ArgumentParser) -> None:
    _add_corpus_args(ap)
    _add_binary_args(ap)
    ap.add_argument(
        "--out",
        type=Path,
        default=REPO_ROOT / "runs" / "full_features_k150k.parquet",
        help="Output parquet path (gitignored).",
    )
    ap.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar. Defaults to <out>.manifest.json. "
            "Records inputs, backend split, feature schema, restart counters, "
            "and the exact CLI args used to build the derived parquet."
        ),
    )


def _add_execution_args(ap: argparse.ArgumentParser) -> None:
    ap.add_argument(
        "--threads",
        type=int,
        default=2,
        help="vmaf --threads value per worker (inner threading).  Default 2.",
    )
    ap.add_argument(
        "--threads-cuda",
        type=int,
        default=8,
        help=(
            "Number of parallel worker processes (outer parallelism).  Each "
            "worker runs one vmaf invocation concurrently.  Default 8 is tuned "
            "for a 32-thread Zen5 CPU; reduce on machines with fewer cores.  "
            "Named --threads-cuda for historical reasons (ADR-0382); it controls "
            "outer process parallelism for both CPU and split CUDA modes."
        ),
    )
    ap.add_argument(
        "--progress-every",
        type=int,
        default=200,
        help=(
            "Print a progress line every N completed clips.  Default 200.  "
            "Previously named --flush-every; the name changed when the per-flush "
            "parquet rewrite was replaced with at-end-only writes (Research-0135)."
        ),
    )
    ap.add_argument(
        "--flush-every",
        type=int,
        default=None,
        help=argparse.SUPPRESS,  # Legacy alias; --progress-every takes precedence.
    )
    ap.add_argument(
        "--limit",
        type=int,
        default=None,
        help="Process at most N clips (smoke-test mode).",
    )
    ap.add_argument(
        "--no-cuda",
        action="store_true",
        help=(
            "Disable CUDA backend flags on the vmaf invocation.  This is the "
            "default when using core/build-cpu/tools/vmaf (the recommended "
            "binary); only needed if passing a CUDA-capable binary explicitly."
        ),
    )
    ap.add_argument(
        "--scratch-dir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "k150k_yuv_scratch",
        help="Scratch directory for temporary YUV files.  Cleaned per-clip.",
    )


def _add_policy_args(ap: argparse.ArgumentParser) -> None:
    ap.add_argument(
        "--allow-fr-from-nr",
        action="store_true",
        help=(
            "Acknowledge that the FR-from-NR adapter (ref == distorted) will be "
            "applied even when the --metadata-jsonl sidecar advertises real "
            "reference rows (e.g. CHUG ``chug_ref==1`` rows). Without this flag "
            "the script refuses to run on FR corpora and points the operator at "
            "ai/scripts/chug_extract_features.py instead. See ADR-0510."
        ),
    )
    ap.add_argument(
        "--vmaf-model",
        type=str,
        default=None,
        help=(
            "VMAF teacher model version string or JSON path. Defaults to fork "
            f"default ({DEFAULT_MODEL} via ADR-1168/ADR-1173)."
        ),
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="extract_k150k_features.py",
        description="Extract FULL_FEATURES from KoNViD-150k-A via FR-from-NR adapter (ADR-0346).",
    )
    _add_source_args(parser)
    _add_execution_args(parser)
    _add_policy_args(parser)
    return parser


@dataclass(frozen=True)
class _WorkPlan:
    clips: list[Path]
    done_path: Path
    done_set: set[str]
    pending: list[Path]
    staging_path: Path


@dataclass
class _RunResult:
    rows: list[dict]
    recovered_rows: list[dict]
    ok: int = 0
    fail: int = 0
    elapsed: float = 0.0


def _preflight(args: argparse.Namespace, use_cuda: bool) -> bool:
    if not args.clips_dir.is_dir():
        print(f"error: clips-dir not found: {args.clips_dir}", file=sys.stderr)
        return False
    if not args.scores.is_file():
        print(f"error: scores CSV not found: {args.scores}", file=sys.stderr)
        return False
    if not args.vmaf_bin.is_file():
        print(
            f"error: vmaf binary not found: {args.vmaf_bin}\n"
            "Build the fork vmaf binary with:\n"
            "  meson setup core/build-cpu core -Denable_cuda=false "
            "--buildtype=release && ninja -C core/build-cpu\n"
            "Then re-run with --vmaf-bin core/build-cpu/tools/vmaf",
            file=sys.stderr,
        )
        return False
    if use_cuda and not args.cpu_vmaf_bin.is_file():
        print(f"error: cpu-vmaf-bin not found: {args.cpu_vmaf_bin}", file=sys.stderr)
        return False
    return True


def _load_score_metadata(
    scores_path: Path,
) -> tuple[dict[str, float], dict[str, dict[str, Any]]] | None:
    scores_df = pd.read_csv(scores_path).rename(columns={"video_score": "mos"})
    duplicate_mask = scores_df["video_name"].duplicated(keep=False)
    if duplicate_mask.any():
        duplicates = sorted(scores_df.loc[duplicate_mask, "video_name"].astype(str).unique())
        preview = ", ".join(duplicates[:10]) + (" ..." if len(duplicates) > 10 else "")
        print(
            f"error: scores CSV {scores_path} has {len(duplicates)} duplicate "
            f"video_name key(s); each clip must appear once. Offending names: {preview}",
            file=sys.stderr,
        )
        return None
    mos_map = dict(zip(scores_df["video_name"], scores_df["mos"], strict=True))
    score_meta: dict[str, dict[str, Any]] = {}
    for row in scores_df.to_dict(orient="records"):
        name = str(row.get("video_name") or "")
        if not name:
            continue
        metadata: dict[str, Any] = {}
        if "mos_raw_0_100" in row:
            metadata["mos_raw_0_100"] = row["mos_raw_0_100"]
        score_meta[name] = metadata
    return mos_map, score_meta


def _reject_fr_misuse(args: argparse.Namespace, jsonl_meta: dict[str, dict[str, Any]]) -> bool:
    misuse = detect_fr_corpus_misuse(jsonl_meta)
    if not misuse["misuse_detected"] or args.allow_fr_from_nr:
        return False
    print(
        f"error: --metadata-jsonl {args.metadata_jsonl} advertises an FR corpus "
        f"with real reference rows ({misuse['ref_count']} ref + {misuse['dis_count']} "
        f"distorted, {misuse['content_groups_with_both']} content groups with both; "
        f"example: {misuse['example']!r}). This script runs the FR-from-NR adapter "
        "(ref == distorted) and would score every clip against itself, producing a "
        "parquet where all difference-based metrics degenerate (vmaf~99 across every "
        "bitrate-ladder rung).\n\nUse ai/scripts/chug_extract_features.py for FR "
        "corpora -- it pairs each distorted row with its matching reference. Pass "
        "--allow-fr-from-nr only if you genuinely want self-vs-self scoring (rare; "
        "see ADR-0509).",
        file=sys.stderr,
    )
    return True


def _build_work_plan(args: argparse.Namespace) -> _WorkPlan:
    clips = sorted(args.clips_dir.glob("*.mp4"))
    if args.limit is not None:
        clips = clips[: args.limit]
    done_path = args.out.with_suffix(".done")
    done_set = _load_done_set(done_path)
    return _WorkPlan(
        clips=clips,
        done_path=done_path,
        done_set=done_set,
        pending=[clip for clip in clips if clip.name not in done_set],
        staging_path=_staging_path(args.out),
    )


def _report_work_plan(args: argparse.Namespace, plan: _WorkPlan, use_cuda: bool) -> None:
    if args.limit is not None and plan.done_set:
        limited_done = sum(1 for clip in plan.clips if clip.name in plan.done_set)
        if limited_done:
            print(
                f"[k150k] NOTE: --limit {args.limit} was applied before the resume "
                f"filter; {limited_done} of the first {len(plan.clips)} clips are "
                f"already done, so only {len(plan.pending)} will be processed this run. "
                "For batched resume pass --limit (done_count + batch_size).",
                file=sys.stderr,
                flush=True,
            )
    print(
        f"[k150k] total={len(plan.clips)} done={len(plan.done_set)} "
        f"pending={len(plan.pending)} cuda={'yes' if use_cuda else 'no'} "
        f"workers={args.threads_cuda} threads/worker={args.threads} out={args.out}",
        flush=True,
    )


def _finish_noop(args: argparse.Namespace, plan: _WorkPlan, use_cuda: bool) -> int:
    recovered_rows = _load_staging_rows(plan.staging_path)
    if recovered_rows:
        _write_parquet_from_rows(recovered_rows, args.out)
        _fsync_path(args.out)
        plan.staging_path.unlink(missing_ok=True)
    parquet_rows = _parquet_row_count(args.out)
    accounted = parquet_rows if recovered_rows else parquet_rows + len(recovered_rows)
    if len(plan.done_set) > accounted:
        missing = len(plan.done_set) - accounted
        raise RuntimeError(
            f"[k150k] CONSISTENCY ERROR: .done lists {len(plan.done_set)} completed "
            f"clip(s) but parquet has only {parquet_rows} row(s) (+{len(recovered_rows)} "
            f"recovered from staging). {missing} clip(s) appear to have been lost by "
            f"a prior crash mid-write. Operator must re-extract them: remove the "
            f"affected entries from {plan.done_path} (or delete it to re-extract "
            "everything) and re-run. See ADR "
            "k150k-crash-restart-row-loss-consistency-check."
        )
    _write_extraction_manifest(
        manifest_out=args.manifest_out,
        args=args,
        use_cuda=use_cuda,
        total_clips=len(plan.clips),
        done_before=len(plan.done_set),
        pending_count=0,
        recovered_rows=len(recovered_rows),
        ok=0,
        fail=0,
        elapsed_seconds=0.0,
        status="complete-noop",
    )
    print("[k150k] nothing to do.", flush=True)
    return 0


def _check_mos_coverage(pending: list[Path], mos_map: dict[str, float]) -> None:
    covered = sum(1 for clip in pending if clip.name in mos_map or clip.stem in mos_map)
    if pending and covered == 0:
        raise SystemExit(
            f"[k150k] FATAL: MOS-label join matched 0/{len(pending)} pending clips. "
            "The scores CSV 'video_name' column does not line up with the corpus "
            f"filenames — every label would be NaN. Example clip: {pending[0].name!r}; "
            f"example score keys: {list(mos_map)[:3]!r}. Fix the join key (e.g. the "
            "file extension) before extracting."
        )
    if pending and covered < len(pending):
        print(
            f"[k150k] WARNING: {len(pending) - covered}/{len(pending)} pending "
            "clips have no MOS label and will carry mos=NaN.",
            file=sys.stderr,
            flush=True,
        )


def _submit_extraction_jobs(
    executor: concurrent.futures.ProcessPoolExecutor,
    args: argparse.Namespace,
    plan: _WorkPlan,
    mos_map: dict[str, float],
    jsonl_meta: dict[str, dict[str, Any]],
    teacher,
    use_cuda: bool,
) -> dict[concurrent.futures.Future, str]:
    future_to_clip: dict[concurrent.futures.Future, str] = {}
    for index, mp4 in enumerate(plan.pending):
        mos = mos_map.get(mp4.name)
        if mos is None:
            mos = mos_map.get(mp4.stem, float("nan"))
        future = executor.submit(
            _process_clip,
            str(mp4),
            mos,
            str(args.vmaf_bin),
            str(args.cpu_vmaf_bin),
            str(args.scratch_dir),
            args.threads,
            use_cuda,
            index % args.threads_cuda,
            jsonl_meta.get(mp4.name),
            teacher.arg,
            teacher.name,
        )
        future_to_clip[future] = mp4.name
    return future_to_clip


def _consume_extraction_jobs(
    future_to_clip: dict[concurrent.futures.Future, str],
    args: argparse.Namespace,
    plan: _WorkPlan,
    recovered_rows: list[dict],
    score_meta: dict[str, dict[str, Any]],
    jsonl_meta: dict[str, dict[str, Any]],
) -> _RunResult:
    result = _RunResult(rows=list(recovered_rows), recovered_rows=recovered_rows)
    recovered_names = {row.get("clip_name") for row in recovered_rows if row.get("clip_name")}
    started = time.time()
    for completed, future in enumerate(concurrent.futures.as_completed(future_to_clip), 1):
        clip_name = future_to_clip[future]
        try:
            row = future.result()
            row.update(score_meta.get(clip_name, {}))
            row.update(jsonl_meta.get(clip_name, {}))
            if clip_name not in recovered_names:
                _append_row_to_staging(plan.staging_path, row)
            result.rows.append(row)
            _append_done(plan.done_path, clip_name)
            result.ok += 1
        except Exception as exc:
            print(f"[k150k] FAIL {clip_name}: {exc}", file=sys.stderr, flush=True)
            result.fail += 1
        if completed % args.progress_every == 0 or completed == len(plan.pending):
            elapsed = time.time() - started
            rate = completed / elapsed if elapsed > 0 else 0.0
            remaining = (
                (len(plan.pending) - completed) / rate / 3600.0 if rate > 0 else float("nan")
            )
            print(
                f"[k150k] {completed}/{len(plan.pending)} ok={result.ok} "
                f"fail={result.fail} {rate:.2f} clip/s eta={remaining:.1f}h",
                flush=True,
            )
    result.elapsed = time.time() - started
    return result


def _run_extraction(
    args: argparse.Namespace,
    plan: _WorkPlan,
    mos_map: dict[str, float],
    score_meta: dict[str, dict[str, Any]],
    jsonl_meta: dict[str, dict[str, Any]],
    teacher,
    use_cuda: bool,
) -> _RunResult:
    recovered_rows = _load_staging_rows(plan.staging_path)
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.threads_cuda) as executor:
        futures = _submit_extraction_jobs(
            executor, args, plan, mos_map, jsonl_meta, teacher, use_cuda
        )
        return _consume_extraction_jobs(futures, args, plan, recovered_rows, score_meta, jsonl_meta)


def _persist_extraction(args: argparse.Namespace, plan: _WorkPlan, result: _RunResult) -> None:
    if not result.rows:
        return
    expected = len(result.recovered_rows) + result.ok
    if len(result.rows) != expected:
        raise RuntimeError(
            f"[k150k] CONSISTENCY ERROR at end-of-run: produced {len(result.rows)} "
            f"row(s) but accounting expected {expected} (= {len(result.recovered_rows)} "
            f"recovered + {result.ok} new ok). fail={result.fail}. Refusing to write "
            f"parquet with mismatched bookkeeping; the staging file at "
            f"{plan.staging_path} is preserved for forensic recovery. See ADR "
            "k150k-crash-restart-row-loss-consistency-check."
        )
    _write_parquet_from_rows(result.rows, args.out)
    _fsync_path(args.out)
    plan.staging_path.unlink(missing_ok=True)


def main() -> int:
    args = _build_parser().parse_args()
    if args.manifest_out is None:
        args.manifest_out = args.out.with_suffix(".manifest.json")

    resolved_teacher = resolve_teacher_model(args.vmaf_model)

    use_cuda = not args.no_cuda

    if not _preflight(args, use_cuda):
        return 2

    score_data = _load_score_metadata(args.scores)
    if score_data is None:
        return 2
    mos_map, score_meta = score_data
    jsonl_meta = _load_jsonl_metadata(args.metadata_jsonl, split_seed=args.split_seed)

    if _reject_fr_misuse(args, jsonl_meta):
        return 2

    plan = _build_work_plan(args)
    _report_work_plan(args, plan, use_cuda)

    if not plan.pending:
        return _finish_noop(args, plan, use_cuda)

    args.scratch_dir.mkdir(parents=True, exist_ok=True)
    _check_mos_coverage(plan.pending, mos_map)
    result = _run_extraction(
        args, plan, mos_map, score_meta, jsonl_meta, resolved_teacher, use_cuda
    )
    _persist_extraction(args, plan, result)
    rate = result.ok / result.elapsed if result.elapsed > 0 else 0.0
    print(
        f"[k150k] done. ok={result.ok} fail={result.fail} "
        f"total_time={result.elapsed:.1f}s "
        f"rate={rate:.2f} clip/s out={args.out}",
        flush=True,
    )
    _write_extraction_manifest(
        manifest_out=args.manifest_out,
        args=args,
        use_cuda=use_cuda,
        total_clips=len(plan.clips),
        done_before=len(plan.done_set),
        pending_count=len(plan.pending),
        recovered_rows=len(result.recovered_rows),
        ok=result.ok,
        fail=result.fail,
        elapsed_seconds=result.elapsed,
        status="complete" if result.fail == 0 else "failed",
    )
    return 0 if result.fail == 0 else 1


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
