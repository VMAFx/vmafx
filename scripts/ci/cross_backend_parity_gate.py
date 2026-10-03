#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cross-backend GPU-parity CI gate (T6-8 / ADR-0214).

Generalisation of ``cross_backend_vif_diff.py``: iterates every
configured ``(feature, backend_pair)`` cell, runs the ``vmaf`` binary
once per backend, then diffs the per-frame metrics with a feature-
specific absolute tolerance.

The single-feature script gates one cell per CLI invocation; this gate
runs the whole matrix in one process and emits two artefacts that
downstream consumers can read without re-parsing stdout:

* ``--json-out`` — machine-readable summary, one record per cell
  (status, max_abs_diff per metric, frame count, tolerance, command).
* ``--md-out``   — human-readable Markdown table, suitable for pasting
  into a PR comment or rendering in CI logs.

Tolerance policy (T6-8 / ADR-0214 § "Tolerance schema"):

* Most integer-pipeline features (``vif``, ``adm``, ``motion``,
  ``motion_v2``, ``psnr``, ``float_moment``) lock places=4 — already
  the production contract from ADR-0125 / ADR-0138 / ADR-0140.
* Float-pipeline features that hit transcendentals (``ciede``,
  ``psnr_hvs``, ``ssimulacra2``) carry a per-feature relaxation
  declared inline in ``FEATURE_TOLERANCE`` with the ADR justifying it.
* The ``--fp16-features`` flag forces the FP16 contract (1e-2 absolute)
  on the listed feature names, used by the future ONNX tiny-AI lane
  once T7-39 lands.

Exit code: 0 if every cell within tolerance, 1 if any cell exceeds
its tolerance, 2 on a binary / fixture failure.

The gate **never modifies** any feature implementation — it only
verifies. Per CLAUDE.md §12 r1 (Netflix golden assertions are
untouchable), any tightening of tolerance must come from a
measurement-driven ADR, not from the CI lane.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path
from typing import Any, cast

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

# The repository root above makes the sibling import canonical for both direct
# script execution and package-aware type checking.
from scripts.ci.cross_backend_calibration import (
    DEFAULT_CALIBRATION_PATH,
    EXACT_TWIN_FRAGMENTS,
    EXACT_TWIN_PRECISION,
    EXACT_TWIN_SOURCE,
    EXACT_TWIN_TOLERANCE,
    EXACT_TWINS,
    LIBM_TWIN_SOURCE,
    LIBM_TWINS,
    CalibrationTable,
    area_tolerance_factor,
    is_exact_pair,
    libm_pair_tolerance,
    load_calibration_table,
    metric_delta,
    validate_exact_twins,
)
from scripts.lib.safe_subprocess import run as run_command

# ---------------------------------------------------------------------------
# Feature → metric-name list. Mirror of ``FEATURE_METRICS`` in
# ``cross_backend_vif_diff.py`` (single source of truth for the
# extractor name → emitted-metric mapping). When a new feature gets a
# GPU twin, add it here.
# ---------------------------------------------------------------------------

FEATURE_METRICS: dict[str, tuple[str, ...]] = {
    "vif": (
        "integer_vif_scale0",
        "integer_vif_scale1",
        "integer_vif_scale2",
        "integer_vif_scale3",
    ),
    # T3-15(c) / ADR-0219: the GPU motion twins emit motion3_score. The
    # five-frame window (motion_five_frame_window=true) has cells of its
    # own, `motion_mffw` and `motion_v2_mffw` below (ADR-1491).
    # The SAD score is what the CPU appends on every frame and derives
    # motion2 / motion3 from; a twin that lacks it fails the cell (ADR-1418).
    "motion": (
        "VMAF_integer_feature_motion_sad_score",
        "integer_motion2",
        "integer_motion3",
    ),
    "motion_debug": (
        "VMAF_integer_feature_motion_sad_score",
        "integer_motion",
        "integer_motion2",
        "integer_motion3",
    ),
    "motion_v2": (
        "VMAF_integer_feature_motion_v2_sad_score",
        "VMAF_integer_feature_motion2_v2_score",
    ),
    # ADR-1491: `motion` and `motion_v2` with the five-frame window and the
    # moving average, the option set of the vmaf_v1.0.16_hfr_* models. The
    # SAD is taken against frame n-2 on the device; motion2 and motion3 come
    # from the CPU's window function on both sides.
    "motion_mffw": (
        "VMAF_integer_feature_motion_sad_score_mffw_mma",
        "integer_motion2_mffw_mma",
        "integer_motion3_mffw_mma",
    ),
    "motion_v2_mffw": (
        "VMAF_integer_feature_motion_v2_sad_score_mffw_mma",
        "VMAF_integer_feature_motion2_v2_score_mffw_mma",
        "VMAF_integer_feature_motion3_v2_score_mffw_mma",
    ),
    "adm": (
        "integer_adm2",
        "integer_adm_scale0",
        "integer_adm_scale1",
        "integer_adm_scale2",
        "integer_adm_scale3",
    ),
    # All three planes: CPU and every twin emit them by default, and a cell
    # that lists one of three outputs leaves two unguarded (ADR-1460).
    "psnr": ("psnr_y", "psnr_cb", "psnr_cr"),
    "float_moment": (
        "float_moment_ref1st",
        "float_moment_dis1st",
        "float_moment_ref2nd",
        "float_moment_dis2nd",
    ),
    "ciede": ("ciede2000",),
    # ADR-1424: the fixed-point `ssim` extractor (integer_ssim.c). Its twins
    # are registered as `integer_ssim_<backend>`, see BACKEND_EXTRACTOR_ALIASES.
    "ssim": ("ssim",),
    "float_ssim": ("float_ssim",),
    # ADR-1382: `float_ssim` with `enable_lcs=true` adds the frame means of
    # the per-pixel luminance / contrast / structure terms (`float_ssim_l`,
    # `float_ssim_c`, `float_ssim_s`) on top of the score. Same float
    # reductions as the score, same places=4 contract.
    "float_ssim_lcs": (
        "float_ssim",
        "float_ssim_l",
        "float_ssim_c",
        "float_ssim_s",
    ),
    "float_ms_ssim": ("float_ms_ssim",),
    # T7-35 / ADR-0215: enable_lcs adds 15 per-scale L/C/S triples on
    # top of the combined float_ms_ssim score. The CUDA/SYCL kernels
    # already produce the per-scale L/C/S means; gating only the
    # feature_collector_append calls keeps default-path output
    # bit-identical. Cell uses extractor `float_ms_ssim` with the
    # `enable_lcs=true` option pass-through (resolved by
    # `cross_backend_vif_diff.py`'s FEATURE_ALIASES on the per-
    # feature lane). The matrix gate runs the LCS variant only when
    # opted in via `--features` (skipped by default to keep
    # parity-matrix-gate cheap).
    "float_ms_ssim_lcs": (
        "float_ms_ssim",
        "float_ms_ssim_l_scale0",
        "float_ms_ssim_l_scale1",
        "float_ms_ssim_l_scale2",
        "float_ms_ssim_l_scale3",
        "float_ms_ssim_l_scale4",
        "float_ms_ssim_c_scale0",
        "float_ms_ssim_c_scale1",
        "float_ms_ssim_c_scale2",
        "float_ms_ssim_c_scale3",
        "float_ms_ssim_c_scale4",
        "float_ms_ssim_s_scale0",
        "float_ms_ssim_s_scale1",
        "float_ms_ssim_s_scale2",
        "float_ms_ssim_s_scale3",
        "float_ms_ssim_s_scale4",
    ),
    # T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06: `float_ms_ssim` with
    # `enable_chroma=true` scores each plane through the same pyramid and
    # adds `float_ms_ssim_cb` / `float_ms_ssim_cr`. Every chroma plane must be
    # at least 176 pixels on a side, so the gate skips this cell, with the
    # reason, on a fixture with smaller chroma (FEATURE_MIN_CHROMA_DIM).
    "float_ms_ssim_chroma": (
        "float_ms_ssim",
        "float_ms_ssim_cb",
        "float_ms_ssim_cr",
    ),
    "float_psnr": ("float_psnr",),
    # `motion3` too: every twin the gate runs (CUDA, SYCL, HIP) emits the CPU's
    # motion3; a twin that lacks it fails the cell
    # (T-GPU-FLOAT-MOTION3-MISSING-2026-09-30).
    "float_motion": (
        "motion",
        "motion2",
        "motion3",
    ),
    "float_vif": (
        "vif_scale0",
        "vif_scale1",
        "vif_scale2",
        "vif_scale3",
    ),
    "psnr_hvs": (
        "psnr_hvs_y",
        "psnr_hvs_cb",
        "psnr_hvs_cr",
        "psnr_hvs",
    ),
    "float_adm": (
        "adm2",
        "adm_scale0",
        "adm_scale1",
        "adm_scale2",
        "adm_scale3",
    ),
    "ssimulacra2": ("ssimulacra2",),
    "cambi": ("cambi",),
    # ADR-1430: the three scores of `speed_chroma` (speed.c).
    "speed_chroma": (
        "speed_chroma_u",
        "speed_chroma_v",
        "speed_chroma_uv",
    ),
    # ADR-1460: the one score of `speed_temporal` (speed.c), the last
    # registered CUDA / SYCL / HIP twin that had no gate feature
    # (scripts/ci/tests/test_gate_covers_registered_twins.py).
    "speed_temporal": ("speed_temporal",),
}

# ---------------------------------------------------------------------------
# Per-feature tolerance contract. Absolute |a - b| ceiling. Mirrors
# the ``--places`` flags wired into the existing per-feature lanes
# (places=4 → 5e-5, places=3 → 5e-4, places=2 → 5e-3); kept as raw
# floats so the gate can express future FP16 tiers (1e-2) without
# overloading the places vocabulary.
#
# Default for any feature not listed: ``DEFAULT_FP32_TOLERANCE``
# (5e-5, equivalent to places=4) — same contract the per-feature
# lane defaults to in `cross_backend_vif_diff.py`.
# ---------------------------------------------------------------------------

DEFAULT_FP32_TOLERANCE = 5e-5  # places=4
DEFAULT_FP16_TOLERANCE = 1e-2  # T6-8 FP16 contract (future tiny-AI lane)

FEATURE_TOLERANCE: dict[str, float] = {
    # Integer pipeline — places=4 (5e-5). ADR-0138 / ADR-0140.
    "vif": 5e-5,
    # The CPU, CUDA and HIP cells are exact instead (EXACT_TWINS, ADR-1416,
    # ADR-1423) and never read this value.
    "adm": 5e-5,
    "motion": 5e-5,
    "motion_debug": 5e-5,
    "motion_v2": 5e-5,
    # The CUDA, SYCL and HIP cells are exact instead (exact_twins.d,
    # ADR-1491) and never read these values.
    "motion_mffw": 5e-5,
    "motion_v2_mffw": 5e-5,
    "psnr": 5e-5,
    "float_moment": 5e-5,
    # int64 moments, one double term per pixel; a twin that reduces per block
    # is a few ulp of the double sum away. The CPU <-> CUDA cell is exact
    # instead (EXACT_TWINS, ADR-1424) and never reads this value.
    "ssim": 5e-5,
    # Float pipeline, well-conditioned. places=4.
    "float_ssim": 5e-5,
    # ADR-1382: L / C / S means are the score's own reductions — places=4.
    "float_ssim_lcs": 5e-5,
    "float_ms_ssim": 5e-5,
    # T7-35 / ADR-0215: LCS triples are the same float reductions
    # that feed the Wang combine — same conditioning, same places=4.
    "float_ms_ssim_lcs": 5e-5,
    # The chroma planes run the luma pipeline: same contract.
    "float_ms_ssim_chroma": 5e-5,
    "float_psnr": 5e-5,
    # The CPU, CUDA, SYCL and HIP cells are exact instead (EXACT_TWINS,
    # ADR-1409, ADR-1411, ADR-1419) and never read this value.
    "float_motion": 5e-5,
    # The CPU <-> CUDA cell is exact instead (EXACT_TWINS, ADR-1412) and
    # never reads this value.
    "float_vif": 5e-5,
    "float_adm": 5e-5,
    # Transcendentals / DCT — relaxed contract per ADR-0187 / ADR-0188.
    # per-pixel pow/sqrt/sin/atan2 in fp32 — places=2. The CPU <-> CUDA cell
    # takes its tolerance from LIBM_TWINS instead (ADR-1426).
    "ciede": 5e-3,
    # DCT + per-block float reductions — places=3 at 576x324; grows with
    # sqrt(term count) above it (area_tolerance_factor, ADR-1361). This is the
    # contract of a twin that sums per block. A cell whose sides are the CPU
    # or a twin listed in EXACT_TWINS (CUDA, SYCL and HIP: ADR-1397, ADR-1401)
    # is compared exactly instead and never reads this value.
    "psnr_hvs": 5e-4,
    # XYB cube root + IIR blur reassociation — places=2 per ADR-0192.
    "ssimulacra2": 5e-3,
    # Integer pipeline — places=4 (5e-5). ADR-0360.
    "cambi": 5e-5,
    # places=4 for a twin that is not listed. The CPU <-> CUDA cell takes its
    # tolerance from LIBM_TWINS instead (ADR-1430).
    "speed_chroma": 5e-5,
    # places=4 for a twin that is not listed. The CPU, CUDA, HIP and SYCL
    # cells take their tolerance from LIBM_TWINS instead (ADR-1460).
    "speed_temporal": 5e-5,
}

# Backend → extractor-name suffix and CLI device-selection flag.
# ADR-1496: `metal` runs on an Apple device, from the macOS tester bundle
# (tools/rc1-tester/src/vmaf_rc1_tester/hw_gate.py); no hosted runner has one.
BACKEND_SUFFIX: dict[str, str] = {
    "cpu": "",
    "cuda": "_cuda",
    "sycl": "_sycl",
    "hip": "_hip",
    "metal": "_metal",
}
BACKEND_DEVICE_FLAG: dict[str, str] = {
    "cuda": "--gpumask",
    "sycl": "--sycl_device",
    "hip": "--hip_device",
    "metal": "--metal_device",
}

# Default device index per backend. CUDA gpumask=1 picks the first GPU;
# SYCL, HIP and Metal device 0 is the first compute-capable one.
BACKEND_DEFAULT_DEVICE: dict[str, int] = {
    "cuda": 1,
    "sycl": 0,
    "hip": 0,
    "metal": 0,
}

# ADR-1496: `--hold-exact <backend>` compares every cell of that backend as an
# exact twin (tolerance 0, `--precision max`), or at the LIBM_TWINS bound for a
# feature whose twins differ only in the math library, before any fragment in
# scripts/ci/exact_twins.d declares it. The macOS tester bundle measures the
# Metal twins that way; a fragment follows the measurement, never this flag.
HELD_EXACT_SOURCE = "held-exact:ADR-1496"

# A fragment in scripts/ci/exact_twins.d naming a feature or backend this gate
# does not run fails at import, not as a cell that is silently never exact.
validate_exact_twins(EXACT_TWIN_FRAGMENTS, FEATURE_METRICS, BACKEND_SUFFIX)


@dataclasses.dataclass(frozen=True)
class Cell:
    """One (feature, backend_a, backend_b) cell of the parity matrix."""

    feature: str
    backend_a: str
    backend_b: str


@dataclasses.dataclass
class CellResult:
    """Outcome of running one cell of the matrix."""

    feature: str
    backend_a: str
    backend_b: str
    tolerance: float
    n_frames: int
    per_metric_max: dict[str, float]
    per_metric_mismatches: dict[str, int]
    status: str  # "OK" | "FAIL" | "SKIP" | "ERROR"
    note: str = ""
    # ADR-0234 calibration provenance — which calibration entry
    # supplied the tolerance, or "default" when the gate fell back
    # to FEATURE_TOLERANCE / DEFAULT_FP32_TOLERANCE. Surfaced in the
    # JSON / Markdown artefacts so reviewers can audit per-arch
    # tolerance decisions without re-reading the YAML.
    tolerance_source: str = "default"


# ---------------------------------------------------------------------------
# Matrix construction helpers. The gate covers every pairwise comparison
# between the user-selected backend list, for every user-selected
# feature. CPU is always included as the canonical reference; if the
# user selects only one extra backend, the matrix degenerates to
# (CPU, that_backend) per feature — same shape as the legacy gate.
# ---------------------------------------------------------------------------


def build_matrix(features: Iterable[str], backends: Iterable[str]) -> list[Cell]:
    backend_list = list(backends)
    cells: list[Cell] = []
    for feature in features:
        for i, a in enumerate(backend_list):
            for b in backend_list[i + 1 :]:
                cells.append(Cell(feature=feature, backend_a=a, backend_b=b))
    return cells


# Pseudo-features that map to a real extractor + a `:opt=val` option
# pass-through. T7-35 / ADR-0215: float_ms_ssim_lcs reuses the
# `float_ms_ssim` extractor with `enable_lcs=true` to gate the 15
# extra L/C/S metrics.
FEATURE_ALIASES: dict[str, tuple[str, str]] = {
    "float_ms_ssim_lcs": ("float_ms_ssim", "enable_lcs=true"),
    "float_ms_ssim_chroma": ("float_ms_ssim", "enable_chroma=true"),
    # ADR-1382: the float_ssim L / C / S outputs.
    "float_ssim_lcs": ("float_ssim", "enable_lcs=true"),
    "motion_debug": ("motion", "debug=true"),
    # ADR-1491: the five-frame window, with the moving average the HFR
    # models pair it with.
    "motion_mffw": ("motion", "motion_five_frame_window=true:motion_moving_average=true"),
    "motion_v2_mffw": ("motion_v2", "motion_five_frame_window=true:motion_moving_average=true"),
}

# Extractors whose backend twin is not `<feature><suffix>`, keyed by the
# base extractor name (FEATURE_ALIASES resolved). The HIP MS-SSIM twin keeps
# its upstream-mirror file name as its registered name (ADR-0549).
BACKEND_EXTRACTOR_ALIASES: dict[tuple[str, str], str] = {
    ("float_ms_ssim", "hip"): "integer_ms_ssim_hip",
    # The CPU extractor is `ssim`; its twins carry the CPU file's name
    # (integer_ssim.c), ADR-0564.
    ("ssim", "cuda"): "integer_ssim_cuda",
    ("ssim", "sycl"): "integer_ssim_sycl",
    ("ssim", "hip"): "integer_ssim_hip",
    # The Metal twins of the fixed-point extractors carry the CPU file's name
    # (integer_<file>.c, ADR-0421); ADR-1496.
    ("adm", "metal"): "integer_adm_metal",
    ("cambi", "metal"): "integer_cambi_metal",
    ("ciede", "metal"): "integer_ciede_metal",
    ("motion", "metal"): "integer_motion_metal",
    ("psnr", "metal"): "integer_psnr_metal",
    ("psnr_hvs", "metal"): "integer_psnr_hvs_metal",
    ("ssim", "metal"): "integer_ssim_metal",
    ("vif", "metal"): "integer_vif_metal",
}


# Cells that score the chroma planes, and the smallest chroma plane side they
# accept: float_ms_ssim's 5-level, 11-tap pyramid (11 << 4) applies to every
# plane enable_chroma scores. On a fixture with smaller chroma (the 576x324
# 4:2:0 pair has 288x162) the CPU extractor refuses the request at init, so
# the cell is reported SKIP with the reason instead of being run.
FEATURE_MIN_CHROMA_DIM: dict[str, int] = {"float_ms_ssim_chroma": 176}


def chroma_plane_size(width: int, height: int, pix_fmt: str) -> tuple[int, int]:
    """The chroma plane of a `pix_fmt` frame, ceil-subsampled as vmaf_picture_alloc() does."""
    ss_hor = 0 if pix_fmt == "444" else 1
    ss_ver = 1 if pix_fmt == "420" else 0
    return (width + ss_hor) >> ss_hor, (height + ss_ver) >> ss_ver


def chroma_skip_note(feature: str, width: int, height: int, pix_fmt: str) -> str:
    """Why `feature` cannot run on this fixture's chroma, or "" when it can."""
    minimum = FEATURE_MIN_CHROMA_DIM.get(feature)
    if minimum is None:
        return ""
    chroma_w, chroma_h = chroma_plane_size(width, height, pix_fmt)
    if chroma_w >= minimum and chroma_h >= minimum:
        return ""
    return (
        f"chroma {chroma_w}x{chroma_h} below the {minimum}-pixel minimum of "
        f"{feature}; run it on a pair whose chroma planes clear it"
    )


def feature_extractor_name(feature: str, backend: str) -> str:
    """Map (feature, backend) to the extractor name `--feature` accepts.

    Pseudo-features in ``FEATURE_ALIASES`` are resolved to
    ``base_extractor=opt=val`` so libvmaf's option parser flips the
    underlying extractor into the right mode (e.g. ``enable_lcs``).
    """

    suffix = BACKEND_SUFFIX[backend]
    base_name, opt_string = FEATURE_ALIASES.get(feature, (feature, ""))
    extractor = BACKEND_EXTRACTOR_ALIASES.get((base_name, backend), f"{base_name}{suffix}")
    return f"{extractor}={opt_string}" if opt_string else extractor


def build_command(
    binary: Path,
    ref: Path,
    dist: Path,
    width: int,
    height: int,
    pix_fmt: str,
    bitdepth: int,
    feature: str,
    backend: str,
    device: int | None,
    output: Path,
    precision: str | None = None,
) -> list[str]:
    extractor = feature_extractor_name(feature, backend)
    cmd: list[str] = [
        str(binary),
        "--reference",
        str(ref),
        "--distorted",
        str(dist),
        "--width",
        str(width),
        "--height",
        str(height),
        "--pixel_format",
        pix_fmt,
        "--bitdepth",
        str(bitdepth),
        "--feature",
        extractor,
        "--no_prediction",
        "--output",
        str(output),
        "--json",
        "--backend",
        backend,
    ]
    if backend != "cpu" and device is not None:
        cmd += [BACKEND_DEVICE_FLAG[backend], str(device)]
    if precision is not None:
        cmd += ["--precision", precision]
    return cmd


def run_one(
    binary: Path,
    ref: Path,
    dist: Path,
    width: int,
    height: int,
    pix_fmt: str,
    bitdepth: int,
    feature: str,
    backend: str,
    device: int | None,
    output: Path,
    precision: str | None = None,
) -> tuple[int, str]:
    """Run a single ``vmaf`` invocation. Returns (returncode, stderr)."""

    cmd = build_command(
        binary,
        ref,
        dist,
        width,
        height,
        pix_fmt,
        bitdepth,
        feature,
        backend,
        device,
        output,
        precision,
    )
    proc = run_command(
        cmd,
        allowed_executables=(binary,),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=600,
        max_output_bytes=16 * 1_048_576,
    )
    if proc.returncode != 0:
        return proc.returncode, (proc.stderr or proc.stdout)
    return 0, ""


def load_frames(path: Path) -> list[dict[str, Any]]:
    with path.open() as f:
        payload: Any = json.load(f)
    if not isinstance(payload, dict) or not isinstance(payload.get("frames"), list):
        raise ValueError(f"{path}: expected an object containing a frames array")
    frames = payload["frames"]
    if not all(isinstance(frame, dict) for frame in frames):
        raise ValueError(f"{path}: every frame must be an object")
    return [cast(dict[str, Any], frame) for frame in frames]


def missing_metrics(frames: list[dict[str, Any]], metrics: tuple[str, ...]) -> list[str]:
    """Metrics of ``metrics`` that at least one frame of a run does not carry."""

    return [m for m in metrics if any(m not in frame.get("metrics", {}) for frame in frames)]


def diff_frames(
    a_frames: list[dict[str, Any]],
    b_frames: list[dict[str, Any]],
    metrics: tuple[str, ...],
    tolerance: float,
) -> tuple[dict[str, float], dict[str, int]]:
    """Per-metric max abs diff and mismatch count across two runs."""

    per_max = dict.fromkeys(metrics, 0.0)
    per_mismatch = dict.fromkeys(metrics, 0)
    for fa, fb in zip(a_frames, b_frames, strict=True):
        for m in metrics:
            d = metric_delta(fa["metrics"][m], fb["metrics"][m])
            per_max[m] = max(per_max[m], d)
            if d > tolerance:
                per_mismatch[m] += 1
    return per_max, per_mismatch


def resolve_cell_tolerance(
    feature: str,
    *,
    fp16_features: Iterable[str],
    calibration: CalibrationTable | None,
    gpu_id: str | None,
    width: int | None = None,
    height: int | None = None,
    backends: tuple[str, str] | None = None,
    held_exact: Iterable[str] = (),
) -> tuple[float, str]:
    """Resolve ``(tolerance_abs, source_label)`` for one cell.

    ``backends`` names the two sides of the cell. When both return the CPU
    extractor's bits for the feature (``is_exact_pair``, ADR-1397) the cell
    is compared exactly: tolerance 0, whatever the frame size or calibration
    row. Callers that pass no backends get the feature's tolerance contract.

    The FP32 tolerance (table default or calibration row) is the contract at
    the reference geometry; for an area-scaled feature it is multiplied by
    ``area_tolerance_factor`` for the fixture's ``width x height`` and the
    label gains ``+area x<factor>`` (ADR-1361). The FP16 contract is absolute.

    Source-label vocabulary (recorded on every CellResult):

    * ``"fp16"``      — feature opted into the FP16 contract.
    * ``"exact:ADR-1397"`` — both sides are bit-exact with the CPU
      extractor; tolerance 0.
    * ``"libm:ADR-1426"`` — both sides run the CPU extractor's arithmetic
      and differ only in their math library; the ``LIBM_TWINS`` tolerance.
    * ``"held-exact:ADR-1496"`` — a side is a backend in ``held_exact`` and
      the cell is compared exactly although no fragment lists it
      (``held_exact_tolerance``).
    * ``"calibrated:<pattern>"`` — calibration table matched and
      supplied a per-feature override; ``status: calibrated`` row.
    * ``"placeholder:<pattern>"`` — calibration table matched but
      the row is a placeholder (no per-feature override); fell back
      to ``FEATURE_TOLERANCE``.
    * ``"placeholder-default:<pattern>"`` — match was a placeholder
      and the matched row also lacked the feature; same numeric
      fallback as ``placeholder:`` but kept distinct so future
      audits can tell whether the row was deliberately silent.
    * ``"no-calibration:<gpu_id>"`` — ``--gpu-id`` supplied but no
      row matched.
    * ``"default"``   — neither FP16 nor calibration applied.
    """

    if feature in fp16_features:
        return DEFAULT_FP16_TOLERANCE, "fp16"
    paired = _pair_tolerance(feature, backends, held_exact) if backends is not None else None
    if paired is not None:
        return paired

    tolerance, source = _reference_tolerance(feature, calibration=calibration, gpu_id=gpu_id)
    factor = area_tolerance_factor(feature, width, height)
    if factor > 1.0:
        return tolerance * factor, f"{source}+area x{factor:.2f}"
    return tolerance, source


def _pair_tolerance(
    feature: str, backends: tuple[str, str], held_exact: Iterable[str]
) -> tuple[float, str] | None:
    """The tolerance the two sides of a cell fix by themselves: listed exact twins,
    math-library twins, a held-exact backend; None when the feature's applies."""

    if is_exact_pair(feature, *backends):
        return EXACT_TWIN_TOLERANCE, EXACT_TWIN_SOURCE
    libm_tolerance = libm_pair_tolerance(feature, *backends)
    if libm_tolerance is not None:
        return libm_tolerance, LIBM_TWIN_SOURCE
    return held_exact_tolerance(feature, backends, held_exact)


def held_exact_tolerance(
    feature: str, backends: tuple[str, str], held_exact: Iterable[str]
) -> tuple[float, str] | None:
    """The tolerance of a cell that a held-exact backend takes part in, or None.

    A side qualifies when it is ``cpu``, a backend in ``held_exact``, or a
    backend that ``EXACT_TWINS`` lists for the feature; at least one side must
    be held. The cell is compared exactly, or at the largest ``LIBM_TWINS``
    bound when the feature's twins differ from the CPU only in the math
    library (ADR-1426), which is what the twins of the other backends are held
    to. ADR-1496.
    """

    held = set(held_exact)
    listed = EXACT_TWINS.get(feature, frozenset())
    if not any(backend in held for backend in backends):
        return None
    if not all(b == "cpu" or b in held or b in listed for b in backends):
        return None
    libm = LIBM_TWINS.get(feature)
    if libm:
        return max(libm.values()), LIBM_TWIN_SOURCE
    return EXACT_TWIN_TOLERANCE, HELD_EXACT_SOURCE


def _reference_tolerance(
    feature: str,
    *,
    calibration: CalibrationTable | None,
    gpu_id: str | None,
) -> tuple[float, str]:
    """FP32 tolerance at the reference geometry, from the table or a calibration row."""

    feature_default = FEATURE_TOLERANCE.get(feature, DEFAULT_FP32_TOLERANCE)
    if calibration is None or gpu_id is None:
        return feature_default, "default"

    entry = calibration.lookup(gpu_id)
    if entry is None:
        return feature_default, f"no-calibration:{gpu_id}"
    if feature in entry.features:
        label_kind = "calibrated" if entry.status == "calibrated" else "placeholder"
        return float(entry.features[feature]), f"{label_kind}:{entry.gpu_id_pattern}"
    # Matched arch row, no per-feature override — typical of placeholder
    # rows whose ``features:`` block is empty until measured.
    return feature_default, f"placeholder-default:{entry.gpu_id_pattern}"


def _cell_error(
    cell: Cell,
    tolerance: float,
    tolerance_source: str,
    note: str,
    *,
    metrics: tuple[str, ...],
    n_frames: int = 0,
) -> CellResult:
    return CellResult(
        feature=cell.feature,
        backend_a=cell.backend_a,
        backend_b=cell.backend_b,
        tolerance=tolerance,
        n_frames=n_frames,
        per_metric_max=dict.fromkeys(metrics, 0.0),
        per_metric_mismatches=dict.fromkeys(metrics, 0),
        status="ERROR",
        note=note,
        tolerance_source=tolerance_source,
    )


def _diff_cell_outputs(
    cell: Cell,
    out_a: Path,
    out_b: Path,
    *,
    metrics: tuple[str, ...],
    tolerance: float,
    tolerance_source: str,
) -> CellResult:
    a_frames = load_frames(out_a)
    b_frames = load_frames(out_b)
    if len(a_frames) != len(b_frames):
        note = f"frame-count mismatch a={len(a_frames)} b={len(b_frames)}"
        return _cell_error(cell, tolerance, tolerance_source, note, metrics=metrics)

    missing_a = missing_metrics(a_frames, metrics)
    missing_b = missing_metrics(b_frames, metrics)
    if missing_a or missing_b:
        note = (
            f"missing metrics: backend_a {cell.backend_a} lacks {missing_a}; "
            f"backend_b {cell.backend_b} lacks {missing_b}"
        )
        return _cell_error(
            cell, tolerance, tolerance_source, note, metrics=metrics, n_frames=len(a_frames)
        )

    per_max, per_mismatch = diff_frames(a_frames, b_frames, metrics, tolerance)
    fail = any(c > 0 for c in per_mismatch.values())
    return CellResult(
        feature=cell.feature,
        backend_a=cell.backend_a,
        backend_b=cell.backend_b,
        tolerance=tolerance,
        n_frames=len(a_frames),
        per_metric_max=per_max,
        per_metric_mismatches=per_mismatch,
        status="FAIL" if fail else "OK",
        tolerance_source=tolerance_source,
    )


def _run_cell_side(
    cell: Cell,
    label: str,
    backend: str,
    out: Path,
    ctx: tuple[Path, Path, Path, int, int, str, int],
    devices: dict[str, int],
    precision: str | None,
    tolerance: float,
    tolerance_source: str,
    metrics: tuple[str, ...],
) -> CellResult | None:
    rc, err = run_one(
        *ctx,
        cell.feature,
        backend,
        devices.get(backend),
        out,
        precision,
    )
    if rc != 0:
        note = f"{label} {backend} failed: {err.strip()[:200]}"
        return _cell_error(cell, tolerance, tolerance_source, note, metrics=metrics)
    return None


def run_cell(
    cell: Cell,
    *,
    binary: Path,
    ref: Path,
    dist: Path,
    width: int,
    height: int,
    pix_fmt: str,
    bitdepth: int,
    workdir: Path,
    devices: dict[str, int],
    tolerance: float,
    tolerance_source: str = "default",
) -> CellResult:
    """Execute one cell of the parity matrix and diff it."""
    metrics = FEATURE_METRICS[cell.feature]
    skip_note = chroma_skip_note(cell.feature, width, height, pix_fmt)
    if skip_note:
        skipped = _cell_error(cell, tolerance, tolerance_source, skip_note, metrics=metrics)
        return dataclasses.replace(skipped, status="SKIP")
    full_precision = tolerance_source in (EXACT_TWIN_SOURCE, LIBM_TWIN_SOURCE, HELD_EXACT_SOURCE)
    precision = EXACT_TWIN_PRECISION if full_precision else None
    out_a = workdir / f"{cell.feature}_{cell.backend_a}.json"
    out_b = workdir / f"{cell.feature}_{cell.backend_b}.json"

    ctx = (binary, ref, dist, width, height, pix_fmt, bitdepth)
    # backend_a first, then backend_b; the first side that fails is the result.
    for label, backend, out in (
        ("backend_a", cell.backend_a, out_a),
        ("backend_b", cell.backend_b, out_b),
    ):
        failed = _run_cell_side(
            cell, label, backend, out, ctx, devices, precision, tolerance, tolerance_source, metrics
        )
        if failed is not None:
            return failed

    return _diff_cell_outputs(
        cell,
        out_a,
        out_b,
        metrics=metrics,
        tolerance=tolerance,
        tolerance_source=tolerance_source,
    )


def emit_json(results: list[CellResult], path: Path) -> None:
    payload = {
        "schema_version": 1,
        "cells": [
            {
                "feature": r.feature,
                "backend_a": r.backend_a,
                "backend_b": r.backend_b,
                "tolerance_abs": r.tolerance,
                "tolerance_source": r.tolerance_source,
                "n_frames": r.n_frames,
                "status": r.status,
                "note": r.note,
                "per_metric_max_abs_diff": r.per_metric_max,
                "per_metric_mismatches": r.per_metric_mismatches,
            }
            for r in results
        ],
    }
    with path.open("w") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")


def emit_md(results: list[CellResult], path: Path) -> None:
    lines: list[str] = []
    lines.append("# Cross-backend parity gate (T6-8)")
    lines.append("")
    lines.append("| feature | backend pair | tolerance | source | frames | max abs diff | status |")
    lines.append("|---|---|---:|---|---:|---:|---|")
    for r in results:
        max_diff = max(r.per_metric_max.values()) if r.per_metric_max else 0.0
        pair = f"{r.backend_a} ↔ {r.backend_b}"
        lines.append(
            f"| `{r.feature}` | {pair} | {r.tolerance:.1e} | "
            f"`{r.tolerance_source}` | {r.n_frames} | "
            f"{max_diff:.3e} | {r.status} |"
        )
    lines.append("")
    fails = [r for r in results if r.status in ("FAIL", "ERROR")]
    if fails:
        lines.append("## Failures detail")
        lines.append("")
        for r in fails:
            lines.append(f"### `{r.feature}` ({r.backend_a} ↔ {r.backend_b}) — {r.status}")
            if r.note:
                lines.append(f"- note: {r.note}")
            for m, d in r.per_metric_max.items():
                miss = r.per_metric_mismatches.get(m, 0)
                lines.append(f"- `{m}`: max_abs_diff={d:.3e}, mismatches={miss}")
            lines.append("")
    with path.open("w") as f:
        f.write("\n".join(lines))


def add_output_and_calibration_args(ap: argparse.ArgumentParser) -> None:
    """Add artifact paths and ADR-0234 calibration controls."""
    ap.add_argument(
        "--workdir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "vmaf_parity_gate",
    )
    ap.add_argument(
        "--json-out",
        type=Path,
        default=None,
        help="write machine-readable summary to this path",
    )
    ap.add_argument(
        "--md-out",
        type=Path,
        default=None,
        help="write Markdown summary to this path",
    )
    ap.add_argument(
        "--gpu-id",
        type=str,
        default=None,
        help=(
            "runtime GPU identifier (Research-0041 schema, e.g. "
            "'cuda:8.6' for Ampere RTX 30, 'sycl:0' for Intel Arc). "
            "Used to look up per-arch tolerances in the "
            "ADR-0234 calibration table; falls back to "
            "FEATURE_TOLERANCE when omitted."
        ),
    )
    ap.add_argument(
        "--calibration-table",
        type=Path,
        default=DEFAULT_CALIBRATION_PATH,
        help=f"path to the ADR-0234 calibration YAML (default: {DEFAULT_CALIBRATION_PATH})",
    )


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vmaf-binary", type=Path, required=True, help="path to core/build/tools/vmaf")
    ap.add_argument("--reference", type=Path, required=True)
    ap.add_argument("--distorted", type=Path, required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--pixel-format", default="420")
    ap.add_argument("--bitdepth", type=int, default=8)
    _add_matrix_arguments(ap)
    _add_runtime_arguments(ap)
    return ap.parse_args()


def _add_matrix_arguments(ap: argparse.ArgumentParser) -> None:
    ap.add_argument(
        "--features",
        nargs="+",
        default=sorted(FEATURE_METRICS.keys()),
        choices=sorted(FEATURE_METRICS.keys()),
        help="features to include in the matrix (default: all registered)",
    )
    ap.add_argument(
        "--backends",
        nargs="+",
        default=["cpu", "cuda"],
        choices=sorted(BACKEND_SUFFIX.keys()),
        help="backends to pair (default: cpu + cuda)",
    )
    ap.add_argument(
        "--hold-exact",
        nargs="*",
        default=[],
        choices=sorted(b for b in BACKEND_SUFFIX if b != "cpu"),
        help=(
            "compare every cell of these backends exactly (tolerance 0, --precision max; "
            "the LIBM_TWINS bound for a math-library feature) although no fragment "
            "lists them: the measurement that precedes a fragment (ADR-1496)"
        ),
    )
    ap.add_argument(
        "--fp16-features",
        nargs="*",
        default=[],
        help=(
            "feature names that should use the FP16 tolerance "
            f"({DEFAULT_FP16_TOLERANCE:.1e}) instead of their FP32 default"
        ),
    )


def _add_runtime_arguments(ap: argparse.ArgumentParser) -> None:
    ap.add_argument(
        "--cuda-device",
        type=int,
        default=BACKEND_DEFAULT_DEVICE["cuda"],
    )
    ap.add_argument(
        "--sycl-device",
        type=int,
        default=BACKEND_DEFAULT_DEVICE["sycl"],
    )
    ap.add_argument(
        "--hip-device",
        type=int,
        default=BACKEND_DEFAULT_DEVICE["hip"],
    )
    ap.add_argument(
        "--metal-device",
        type=int,
        default=BACKEND_DEFAULT_DEVICE["metal"],
    )
    add_output_and_calibration_args(ap)


def requested_calibration(args: argparse.Namespace) -> CalibrationTable | None:
    """Load and report the matching calibration row, when requested."""
    # ADR-0234: load the calibration table once. ``None`` is the
    # backward-compatible signal (pyyaml missing, file absent, or
    # ``--gpu-id`` not supplied) and forces the per-feature default
    # path everywhere downstream.
    if args.gpu_id is None:
        return None
    calibration = load_calibration_table(args.calibration_table)
    if calibration is None:
        return None
    entry = calibration.lookup(args.gpu_id)
    if entry is None:
        print(f"calibration: no row matches gpu_id={args.gpu_id}; using FEATURE_TOLERANCE defaults")
    else:
        print(
            f"calibration: matched '{entry.gpu_id_pattern}' ({entry.label}, status={entry.status})"
        )
    return calibration


def run_cells(
    args: argparse.Namespace,
    cells: list[Cell],
    calibration: CalibrationTable | None,
    devices: dict[str, int],
) -> list[CellResult]:
    """Execute and report each backend-pair cell."""
    results: list[CellResult] = []
    for cell in cells:
        tolerance, tolerance_source = resolve_cell_tolerance(
            cell.feature,
            fp16_features=args.fp16_features,
            calibration=calibration,
            gpu_id=args.gpu_id,
            width=args.width,
            height=args.height,
            backends=(cell.backend_a, cell.backend_b),
            held_exact=args.hold_exact,
        )
        result = run_cell(
            cell,
            binary=args.vmaf_binary,
            ref=args.reference,
            dist=args.distorted,
            width=args.width,
            height=args.height,
            pix_fmt=args.pixel_format,
            bitdepth=args.bitdepth,
            workdir=args.workdir,
            devices=devices,
            tolerance=tolerance,
            tolerance_source=tolerance_source,
        )
        results.append(result)
        max_diff = max(result.per_metric_max.values()) if result.per_metric_max else 0.0
        print(
            f"{result.feature:<14} "
            f"{result.backend_a:<6} ↔ {result.backend_b:<6}  "
            f"tol={result.tolerance:.1e} ({result.tolerance_source})  "
            f"max_abs_diff={max_diff:.3e}  "
            f"{result.status}" + (f"  ({result.note})" if result.note else "")
        )
    return results


def main() -> int:
    args = parse_args()
    try:
        args.vmaf_binary = args.vmaf_binary.expanduser().resolve(strict=True)
    except OSError as exc:
        sys.stderr.write(f"vmaf binary not found: {args.vmaf_binary}: {exc}\n")
        return 2
    if not args.vmaf_binary.is_file() or not os.access(args.vmaf_binary, os.X_OK):
        sys.stderr.write(f"vmaf binary is not an executable file: {args.vmaf_binary}\n")
        return 2
    for path in (args.reference, args.distorted):
        if not path.exists():
            sys.stderr.write(f"fixture not found: {path}\n")
            return 2
    cells = build_matrix(args.features, args.backends)
    if not cells:
        sys.stderr.write("empty matrix — supply at least two backends or one feature\n")
        return 2
    args.workdir.mkdir(parents=True, exist_ok=True)
    devices = {
        "cuda": args.cuda_device,
        "sycl": args.sycl_device,
        "hip": args.hip_device,
        "metal": args.metal_device,
    }
    results = run_cells(args, cells, requested_calibration(args), devices)
    if args.json_out is not None:
        emit_json(results, args.json_out)
    if args.md_out is not None:
        emit_md(results, args.md_out)

    fail = any(r.status in ("FAIL", "ERROR") for r in results)
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
