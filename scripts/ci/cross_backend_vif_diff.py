#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cross-backend feature diff — gates CUDA and SYCL compute kernels
against the CPU scalar reference. Runs `vmaf` twice on the
same (ref, dist) pair: once with the CPU integer extractor, once with
the chosen GPU backend's named twin (e.g. ``adm_cuda`` /
``adm_sycl``). Compares per-frame scores at ``places=4`` and prints a
per-metric verdict.

The script's filename is historical; its scope is controlled by
``--feature`` and ``--backend {cuda,sycl}``.

Default tolerance is ``places=4`` (matches the fork's GPU-vs-CPU
snapshot contract — see ``docs/principles.md`` and the user's "GPU is
NOT bit-exact" invariant). Feature-specific calibration can override
that default when an architecture has a measured tolerance.

The gate uses an absolute-tolerance check
(``abs(cpu - gpu) <= 0.5e-places``) rather than Python's
``round(cpu, places) == round(gpu, places)`` because banker rounding
on the IEEE-754 representation flips at the rounding boundary even
when the underlying floats agree to better than the contract — see
PR #120's commit message for the worked example.

Exit code 0 on agreement, 1 on a places=4 mismatch, 2 on a binary or
fixture failure.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

try:
    from scripts.lib.safe_subprocess import run as run_command
except ModuleNotFoundError:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from lib.safe_subprocess import run as run_command

# The calibration loader lives next to this script; ensure the
# script directory is on sys.path so ``python3 scripts/ci/<this>.py``
# (which doesn't add the script's parent to sys.path the way
# ``-m`` invocation would) finds the sibling module reliably.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cross_backend_calibration import DEFAULT_CALIBRATION_PATH, load_calibration_table

FEATURE_METRICS: dict[str, tuple[str, ...]] = {
    "vif": (
        "integer_vif_scale0",
        "integer_vif_scale1",
        "integer_vif_scale2",
        "integer_vif_scale3",
    ),
    # T3-15(c) / ADR-0219: GPU motion now emits motion3_score in
    # 3-frame window mode. The 5-frame window mode
    # (motion_five_frame_window=true) remains deferred — the GPU
    # extractors reject it with -ENOTSUP at init().
    "motion": (
        "integer_motion",
        "integer_motion2",
        "integer_motion3",
    ),
    # GPU long-tail batch 3 part 1 (T7-23 / ADR-0192 / ADR-0193):
    # motion_v2 stateless variant. Same 5-tap separable filter as
    # motion, applied to (prev_ref - cur_ref) — exploits convolution
    # linearity so we can compute the score in one dispatch with no
    # per-frame blurred-state buffer. Bit-exact vs CPU on 8/10-bit
    # (max_abs_diff = 0.0 across 48 frames at 576x324 on Mesa anv +
    # Intel Arc A380); precision target places=4 in the gate.
    "motion_v2": (
        "VMAF_integer_feature_motion_v2_sad_score",
        "VMAF_integer_feature_motion2_v2_score",
    ),
    "adm": (
        "integer_adm2",
        "integer_adm_scale0",
        "integer_adm_scale1",
        "integer_adm_scale2",
        "integer_adm_scale3",
    ),
    # GPU long-tail batch 1 (T7-23 / ADR-0182): PSNR. The host loop
    # runs three dispatches per frame (Y, Cb, Cr); CPU, CUDA, and SYCL
    # emit the same psnr_y/cb/cr metric set.
    "psnr": ("psnr_y", "psnr_cb", "psnr_cr"),
    # GPU long-tail batch 1d (T7-23 / ADR-0182): float_moment.
    # The CPU extractor is registered as `float_moment`; its GPU twins
    # use the backend suffix. The 4 emitted metrics — 1st and
    # 2nd raw moment of ref + dis luma — match byte-for-byte at JSON
    # precision (int64 sum is exact on integer YUV inputs).
    "float_moment": (
        "float_moment_ref1st",
        "float_moment_dis1st",
        "float_moment_ref2nd",
        "float_moment_dis2nd",
    ),
    # GPU long-tail batch 1c (T7-23 / ADR-0182 / ADR-0187):
    # ciede2000 ΔE. The CPU extractor is registered as `ciede`; the
    # GPU twins use the backend suffix. Per-pixel transcendentals
    # (pow / sqrt / sin / atan2) — places=2 contract, NOT bit-exact.
    "ciede": ("ciede2000",),
    # GPU long-tail batch 2 part 1 (T7-23 / ADR-0188 / ADR-0189):
    # float_ssim. Active CPU extractor is `float_ssim`; GPU twins use
    # the backend suffix. Single emitted metric;
    # places=4 contract per ADR-0189 (measure-then-set-the-contract,
    # relax to places=3 if the gate exceeds 5e-5 max_abs).
    "float_ssim": ("float_ssim",),
    # GPU long-tail batch 2 part 2 (T7-23 / ADR-0188 / ADR-0190):
    # float_ms_ssim. 5-level pyramid + Wang product combine. Single
    # emitted metric (the enable_lcs extras are gated separately
    # via "float_ms_ssim_lcs" — T7-35 / ADR-0215 — so the default
    # gate stays cheap).
    "float_ms_ssim": ("float_ms_ssim",),
    # T7-35 / ADR-0215: enable_lcs adds the 15 per-scale L/C/S
    # triples on top of the combined `float_ms_ssim` score. The
    # GPU kernels already produce l_means / c_means / s_means per
    # scale (the vert pass's "_lcs" suffix); this entry gates the
    # bit-identical-vs-CPU promise on the extra metrics. Use
    # `--feature float_ms_ssim_lcs --backend {cuda,sycl}` and
    # pass `enable_lcs=true` via the build_command's option-pass
    # path. places=4 contract per ADR-0215.
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
    # GPU long-tail batch 2 part 3 (T7-23 / ADR-0188 / ADR-0191):
    # float_psnr_hvs. DCT-based perceptual PSNR; emits 3 plane scores
    # + the combined `psnr_hvs`. CPU extractor is `psnr_hvs`; GPU
    # twins use the backend suffix. The CPU and GPU paths both
    # emit identical metric names — the suffix-renaming logic in
    # build_command takes care of routing. Precision target
    # places=2 per ADR-0188 (DCT integer-exact, but per-block float
    # reductions and per-plane log10 limit the floor).
    # GPU long-tail batch 3 part 3 (T7-23 / ADR-0192 / ADR-0195):
    # float_psnr. Float twin of the integer psnr kernels already on
    # GPU; per-pixel (ref-dis)² + log10. Bit-exact across all three
    # backends (max_abs_diff = 0.0 on 8/10-bit) — the kernel is so
    # simple there's no room for accumulator-order drift.
    "float_psnr": ("float_psnr",),
    # GPU long-tail batch 3 part 4 (T7-23 / ADR-0192 / ADR-0196):
    # float_motion. CPU emits short keys "motion" + "motion2" for the
    # float extractor (no `integer_` prefix; see float_motion.c).
    # places=4 contract — empirical floor 3e-6 (8-bit) / 1e-6 (10-bit).
    "float_motion": (
        "motion",
        "motion2",
    ),
    # GPU long-tail batch 3 part 5 (T7-23 / ADR-0192 / ADR-0197):
    # float_vif. 4-scale pyramid with 17/9/5/3-tap separable
    # filters + decimation between scales. places=4 contract.
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
    # GPU long-tail batch 3 part 6 (T7-23 / ADR-0192 / ADR-0199):
    # float_adm. CDF 9/7 (DB2) wavelet + decouple + CSF + Contrast
    # Measure pipeline (4 scales, 4 stages each). CPU extractor is
    # `float_adm`; GPU twins use the backend suffix. Emits the combined
    # `adm2` and 4 per-scale ratios. places=4 contract.
    "float_adm": (
        "adm2",
        "adm_scale0",
        "adm_scale1",
        "adm_scale2",
        "adm_scale3",
    ),
    # GPU long-tail batch 3 part 7 (T7-23 / ADR-0192 / ADR-0200):
    # ssimulacra2. Single emitted metric. Precision target places=2
    # per ADR-0192 (XYB cube root + IIR blur reassociation across
    # 6-scale pyramid; tighten if the kernel actually achieves
    # places>=3).
    "ssimulacra2": ("ssimulacra2",),
    # GPU long-tail batch 3 terminus (T7-36 / ADR-0205): cambi.
    # GPU twins use the Strategy II hybrid: the device runs integer phases
    # (preprocess, derivative, 7x7 SAT mask, decimate, mode filter);
    # the precision-sensitive sliding-histogram c_values + top-K
    # pool stay on the host. By construction the GPU output buffers
    # are byte-identical to the CPU's, so the host residual emits a
    # bit-identical score — places=4 (canonical floor) per
    # ADR-0205 §Precision contract.
    "cambi": ("Cambi_feature_cambi_score",),
}

# Some `--feature` keys here are pseudo-names that map to a real
# libvmaf extractor plus a `feature=NAME:opt=val` option pass-through.
# Used today only by `float_ms_ssim_lcs` (T7-35 / ADR-0215) — the
# enable_lcs option flips the same extractor (`float_ms_ssim`) into
# 16-metric mode. Each entry is (extractor_base_name, "opt=val").
FEATURE_ALIASES: dict[str, tuple[str, str]] = {
    "float_ms_ssim_lcs": ("float_ms_ssim", "enable_lcs=true"),
}

BACKEND_EXTRACTOR_ALIASES: dict[tuple[str, str], str] = {}

# Per-backend extractor-name suffix and the device-selection flag the
# CLI uses to actually route to it. CPU is the implicit baseline (no
# suffix, no device flag). If a future backend uses a different naming
# convention or a different device-selection flag, add it here.
BACKEND_SUFFIX: dict[str, str] = {
    "cuda": "_cuda",
    "sycl": "_sycl",
}
BACKEND_DEVICE_FLAG: dict[str, str] = {
    "cuda": "--gpumask",
    "sycl": "--sycl_device",
}


def feature_extractor_name(feature: str, backend: str | None) -> str:
    base_name, opt_string = FEATURE_ALIASES.get(feature, (feature, ""))
    if backend is None:
        extractor_name = base_name
    else:
        extractor_name = BACKEND_EXTRACTOR_ALIASES.get(
            (feature, backend), f"{base_name}{BACKEND_SUFFIX[backend]}"
        )
    return f"{extractor_name}={opt_string}" if opt_string else extractor_name


def run_vmaf(
    binary: Path,
    ref: Path,
    dist: Path,
    w: int,
    h: int,
    pix_fmt: str,
    bitdepth: int,
    feature: str,
    output: Path,
    backend: str | None,
    device: int | None,
) -> None:
    """Invoke `vmaf` with `--feature <feature>` (or its backend twin)
    and `--no_prediction` so the default model doesn't auto-load the
    CPU extractor alongside the GPU one and race them on the same
    feature names. See PR #120 commit message for the silent-CPU bug
    that motivated this contract.
    """
    extractor = feature_extractor_name(feature, backend)
    cmd = [
        str(binary),
        "--reference",
        str(ref),
        "--distorted",
        str(dist),
        "--width",
        str(w),
        "--height",
        str(h),
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
    ]
    if backend is not None:
        # --backend forces backend exclusivity so a build with multiple
        # backends doesn't try
        # to init the unselected ones (which can hang on SYCL when
        # the device map differs between backends). The device flag
        # still pins the index for the chosen backend.
        cmd += ["--backend", backend]
        if device is not None:
            cmd += [BACKEND_DEVICE_FLAG[backend], str(device)]
    if backend is None:
        cmd += ["--backend", "cpu"]
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
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit(2)


def load_frames(path: Path) -> list[dict]:
    with path.open() as f:
        return json.load(f)["frames"]


def diff(
    cpu: list[dict],
    gpu: list[dict],
    metrics: tuple[str, ...],
    places: int,
    tolerance_override: float | None = None,
    tolerance_source: str = "places",
) -> int:
    """Per-metric absolute-tolerance comparison.

    ``places`` retains the legacy contract — ``threshold = 0.5 * 10**-places``.
    ``tolerance_override``, when supplied, replaces that with an
    explicit absolute ceiling (used by the ADR-0234 calibration-table
    path). ``tolerance_source`` is the human-readable label printed
    in the verdict header so reviewers can see *why* the gate picked
    its threshold (places, calibrated arch, placeholder fallback).
    """

    if len(cpu) != len(gpu):
        print(f"FAIL: frame count mismatch (cpu={len(cpu)}, gpu={len(gpu)})")
        return 1

    threshold = tolerance_override if tolerance_override is not None else 0.5 * (10**-places)

    per_metric_max = dict.fromkeys(metrics, 0.0)
    per_metric_mismatch = dict.fromkeys(metrics, 0)

    for cf, gf in zip(cpu, gpu, strict=True):
        for m in metrics:
            c, v = cf["metrics"][m], gf["metrics"][m]
            d = abs(c - v)
            per_metric_max[m] = max(per_metric_max[m], d)
            if d > threshold:
                per_metric_mismatch[m] += 1

    print(
        f"cross-backend diff, {len(cpu)} frames, "
        f"tolerance={threshold:.3e} (source={tolerance_source})"
    )
    print(f"{'metric':<25} {'max_abs_diff':<15} mismatches")
    fail = False
    for m in metrics:
        verdict = "OK" if per_metric_mismatch[m] == 0 else "FAIL"
        print(
            f"  {m:<25} {per_metric_max[m]:<15.6e} {per_metric_mismatch[m]}/{len(cpu)}  {verdict}"
        )
        if per_metric_mismatch[m] > 0:
            fail = True

    return 1 if fail else 0


def build_parser() -> argparse.ArgumentParser:
    """Build the legacy single-feature cross-backend CLI."""
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vmaf-binary", type=Path, required=True, help="path to core/build/tools/vmaf")
    ap.add_argument("--reference", type=Path, required=True)
    ap.add_argument("--distorted", type=Path, required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--pixel-format", default="420")
    ap.add_argument("--bitdepth", type=int, default=8)
    ap.add_argument("--places", type=int, default=4)
    ap.add_argument(
        "--gpu-id",
        type=str,
        default=None,
        help=(
            "runtime GPU identifier (Research-0041 schema, e.g. "
            "'cuda:8.6' for Ampere RTX 30 or 'sycl:0x8086:0x56a5' "
            "for Intel Arc). When supplied, the per-feature tolerance is "
            "looked up in the ADR-0234 calibration table; otherwise "
            "--places remains authoritative."
        ),
    )
    ap.add_argument(
        "--calibration-table",
        type=Path,
        default=DEFAULT_CALIBRATION_PATH,
        help=(f"path to the ADR-0234 calibration YAML (default: {DEFAULT_CALIBRATION_PATH})"),
    )
    ap.add_argument(
        "--feature",
        choices=tuple(FEATURE_METRICS),
        default="vif",
        help="extractor to gate (vif | motion | adm)",
    )
    ap.add_argument(
        "--backend",
        choices=tuple(BACKEND_SUFFIX),
        default="cuda",
        help="GPU backend to compare against CPU (cuda | sycl)",
    )
    ap.add_argument(
        "--device",
        type=int,
        default=None,
        help=(
            "device selector for the chosen backend. SYCL uses a zero-based "
            "index; CUDA uses a gpumask (for example 1 = first GPU). "
            "Defaults: sycl=0, cuda=1."
        ),
    )
    ap.add_argument(
        "--workdir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "vmaf_cross_backend",
    )
    return ap


def normalize_device(args: argparse.Namespace) -> None:
    """Apply the backend-specific default device."""
    if args.device is None:
        # Per-backend defaults: gpumask=1 picks the first GPU on CUDA;
        # device 0 is the first compute-capable device on SYCL.
        args.device = 1 if args.backend == "cuda" else 0


def run_pair(args: argparse.Namespace, cpu_json: Path, gpu_json: Path) -> None:
    """Run the CPU reference and selected device twin."""
    print(f"running CPU {args.feature} → {cpu_json}")
    run_vmaf(
        args.vmaf_binary,
        args.reference,
        args.distorted,
        args.width,
        args.height,
        args.pixel_format,
        args.bitdepth,
        args.feature,
        cpu_json,
        backend=None,
        device=None,
    )
    print(f"running {args.backend} {args.feature} (device {args.device}) → {gpu_json}")
    run_vmaf(
        args.vmaf_binary,
        args.reference,
        args.distorted,
        args.width,
        args.height,
        args.pixel_format,
        args.bitdepth,
        args.feature,
        gpu_json,
        backend=args.backend,
        device=args.device,
    )


def calibrated_tolerance(args: argparse.Namespace) -> tuple[float | None, str]:
    """Resolve the ADR-0234 override and its audit label."""
    tolerance: float | None = None
    source = f"places={args.places}"
    if args.gpu_id is None:
        return tolerance, source
    table = load_calibration_table(args.calibration_table)
    if table is None:
        return tolerance, source
    entry = table.lookup(args.gpu_id)
    if entry is not None and args.feature in entry.features:
        return (
            float(entry.features[args.feature]),
            f"calibration {entry.gpu_id_pattern} ({entry.status})",
        )
    if entry is not None:
        source = (
            f"calibration {entry.gpu_id_pattern} "
            f"(no per-feature override; places={args.places} fallback)"
        )
    else:
        source = f"no calibration entry for {args.gpu_id}; places={args.places} fallback"
    return tolerance, source


def main() -> int:
    args = build_parser().parse_args()
    normalize_device(args)
    try:
        args.vmaf_binary = args.vmaf_binary.expanduser().resolve(strict=True)
    except OSError as exc:
        print(f"vmaf binary not found: {args.vmaf_binary}: {exc}")
        return 2
    if not args.vmaf_binary.is_file() or not os.access(args.vmaf_binary, os.X_OK):
        print(f"vmaf binary is not an executable file: {args.vmaf_binary}")
        return 2
    for p in (args.reference, args.distorted):
        if not p.exists():
            print(f"fixture not found: {p}")
            return 2

    args.workdir.mkdir(parents=True, exist_ok=True)
    cpu_json = args.workdir / f"cpu_{args.feature}.json"
    gpu_json = args.workdir / f"{args.backend}_{args.feature}.json"
    run_pair(args, cpu_json, gpu_json)

    # ADR-0234: when ``--gpu-id`` is supplied, look up the per-arch
    # tolerance for this feature. Falls back to ``--places`` when:
    #  - no --gpu-id was given (legacy callers, default behaviour);
    #  - pyyaml is unavailable (loader returns None);
    #  - the gpu_id matches no row;
    #  - the matched row has no override for ``args.feature``
    #    (placeholder rows: registered arch, no calibration data).
    tolerance_override, tolerance_source = calibrated_tolerance(args)
    return diff(
        load_frames(cpu_json),
        load_frames(gpu_json),
        FEATURE_METRICS[args.feature],
        args.places,
        tolerance_override=tolerance_override,
        tolerance_source=tolerance_source,
    )


if __name__ == "__main__":
    sys.exit(main())
