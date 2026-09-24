#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Keep registered metric collectors on the fail-closed production emitters."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "src" / "feature"

REQUIRED = {
    "vmaf_vif_emit_scores": (
        "float_vif.c",
        "integer_vif.c",
        "cuda/float_vif_cuda.c",
        "cuda/integer_vif_cuda.c",
        "hip/float_vif_hip.c",
        "hip/integer_vif_hip.c",
        "sycl/float_vif_sycl.cpp",
        "sycl/integer_vif_sycl.cpp",
        "metal/float_vif_metal.mm",
        "metal/integer_vif_metal.mm",
    ),
    "vmaf_adm_floor_pair_named": (
        "adm.c",
        "integer_adm.c",
        "cuda/float_adm_cuda.c",
        "cuda/integer_adm_cuda.c",
        "hip/float_adm_hip.c",
        "hip/integer_adm_hip.c",
        "sycl/float_adm_sycl.cpp",
        "sycl/integer_adm_sycl.cpp",
        "metal/float_adm_metal.mm",
        "metal/integer_adm_metal.mm",
    ),
    "vmaf_feature_emit_finite_scores": (
        "float_adm.c",
        "integer_adm.c",
        "cuda/float_adm_cuda.c",
        "cuda/integer_adm_cuda.c",
        "hip/float_adm_hip.c",
        "hip/integer_adm_hip.c",
        "sycl/float_adm_sycl.cpp",
        "sycl/integer_adm_sycl.cpp",
        "metal/float_adm_metal.mm",
        "metal/integer_adm_metal.mm",
    ),
    "vmaf_ms_ssim_emit_scores": (
        "float_ms_ssim.c",
        "cuda/integer_ms_ssim_cuda.c",
        "hip/integer_ms_ssim_hip.c",
        "sycl/integer_ms_ssim_sycl.cpp",
        "metal/float_ms_ssim_metal.mm",
    ),
    "vmaf_ssim_prepare_score_named": (
        "float_ms_ssim.c",
        "sycl/integer_ms_ssim_sycl.cpp",
    ),
}

SSIM_REQUIRED = {
    "float_ssim.c": "vmaf_ssim_emit_score(",
    "integer_ssim.c": "vmaf_ssim_emit_score_named(",
    "cuda/integer_ssim_cuda.c": "vmaf_ssim_emit_ratio_score_named(",
    "cuda/ssim_cuda.c": "vmaf_ssim_emit_ratio_score_named(",
    "hip/float_ssim_hip.c": "vmaf_ssim_emit_ratio_score_named(",
    "hip/integer_ssim_hip.c": "vmaf_ssim_emit_ratio_score_named(",
    "sycl/integer_ssim_sycl.cpp": "vmaf_ssim_emit_ratio_score_named(",
    "metal/float_ssim_metal.mm": "vmaf_ssim_emit_scores_named(",
    "metal/integer_ssim_metal.mm": "vmaf_ssim_emit_ratio_score_named(",
}

FORBIDDEN = {
    "vif.c": ("*score_den == 0.0 ? 1.0f",),
    "metal/float_ssim_metal.mm": (
        "(n_valid > 0.0) ? (ssim_sum / n_valid) : 1.0",
        "(n_valid > 0.0) ? (l_sum  / n_valid) : 1.0",
        "(n_valid > 0.0) ? (c_sum  / n_valid) : 1.0",
        "(n_valid > 0.0) ? (ss_sum / n_valid) : 1.0",
    ),
    "metal/float_ssim.metal": (
        "(lden > 0.0f) ? (lnum / lden) : 1.0f",
        "(cden > 0.0f) ? (cnum / cden) : 1.0f",
        "(sden > 0.0f) ? (snum / sden) : 1.0f",
    ),
}

METAL_LCS_GUARDS = ("!isfinite(lnum)", "!isfinite(cnum)", "!isfinite(snum)")

METAL_MS_SSIM_FIXED_DB_CALL = (
    "vmaf_ms_ssim_emit_scores(feature_collector, s->feature_name_dict, "
    '"float_ms_ssim_metal", "float_ms_ssim", msssim, false, INFINITY, '
    "l_means, c_means, s_means, MS_SSIM_SCALES, s->enable_lcs, index)"
)


def find_missing_required_symbols() -> list[str]:
    missing: list[str] = []
    for symbol, paths in REQUIRED.items():
        for relative in paths:
            if symbol not in (ROOT / relative).read_text(encoding="utf-8"):
                missing.append(f"{relative}: missing {symbol}")
    for relative, symbol in SSIM_REQUIRED.items():
        if symbol not in (ROOT / relative).read_text(encoding="utf-8"):
            missing.append(f"{relative}: missing {symbol}")
    return missing


def find_retained_fallbacks() -> list[str]:
    missing: list[str] = []
    for relative, patterns in FORBIDDEN.items():
        source = (ROOT / relative).read_text(encoding="utf-8")
        for pattern in patterns:
            if pattern in source:
                missing.append(f"{relative}: retained non-finite fallback {pattern!r}")
    return missing


def find_incomplete_vif_emitters() -> list[str]:
    missing: list[str] = []
    for relative in REQUIRED["vmaf_vif_emit_scores"]:
        source = (ROOT / relative).read_text(encoding="utf-8")
        if "vmaf_vif_emit_scale_scores" in source:
            missing.append(f"{relative}: bypasses the complete-set VIF emitter")
    return missing


def find_missing_metal_guards() -> list[str]:
    missing: list[str] = []
    metal_ssim = (ROOT / "metal/float_ssim.metal").read_text(encoding="utf-8")
    for guard in METAL_LCS_GUARDS:
        if guard not in metal_ssim:
            missing.append(f"metal/float_ssim.metal: missing raw L/C/S guard {guard!r}")
    return missing


def find_metal_ms_ssim_db_wiring_drift() -> list[str]:
    source = (ROOT / "metal/float_ms_ssim_metal.mm").read_text(encoding="utf-8")
    compact_source = " ".join(source.split())
    if METAL_MS_SSIM_FIXED_DB_CALL not in compact_source:
        return [
            "metal/float_ms_ssim_metal.mm: the shared emitter must preserve the "
            "ADR-0490/ADR-1221 fixed linear-score contract (false, INFINITY)"
        ]
    return []


def main() -> None:
    missing = [
        *find_missing_required_symbols(),
        *find_retained_fallbacks(),
        *find_incomplete_vif_emitters(),
        *find_missing_metal_guards(),
        *find_metal_ms_ssim_db_wiring_drift(),
    ]
    if missing:
        raise SystemExit("\n".join(missing))


if __name__ == "__main__":
    main()
