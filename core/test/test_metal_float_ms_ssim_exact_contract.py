#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact float_ms_ssim_metal design (ADR-1498, after ADR-1414 / ADR-1466).

The CPU extractor (``float_ms_ssim.c`` through ``ms_ssim.c``) decimates with
``ms_ssim_decimate.c`` (one fused multiply-add per tap), runs ``iqa_ssim()``
on each of five scales (eleven-tap Gaussian, fp32 window values, fp64 l and c
quotients, one double per sum, raster order, fp32 means) and combines the
scales with ``pow()``. Metal has no fp64 type, so ``float_ms_ssim_metal``,
like its SYCL twin:

- decimates with two separable kernels, every tap
  ``vmaf_mtl_msdec_tap() = fma(sample, tap, acc)`` in the CPU's tap order;
- forms the window sums, fp32 values and fp64 quotients with the header it
  shares with ``float_ssim_metal`` (``metal_ssim_terms.h``);
- stores every window's lv, cv and sv at its raster position in its
  (plane, scale) region, with no reduction on the device;
- adds each region on the host in index order, rounds each mean to fp32 and
  combines the scales with ``ms_ssim.c``'s expression.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-port twin had (a nine-by-nine decimation with unfused
partial sums, ``simd_sum`` partials per threadgroup, fp32 quotients, host
partial sums) or a regrouping that rounds elsewhere, so the contract fails on
the old design and passes on the new one. ``test_metal_float_ms_ssim_math``
checks the arithmetic and the whole twin on the host;
``test_metal_float_ms_ssim_parity`` checks the scores on a device.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import test_metal_float_ssim_exact_contract as ssim_contract  # noqa: E402

ROOT = ssim_contract.ROOT
FEATURE_ROOT = ssim_contract.FEATURE_ROOT
_code = ssim_contract._code
_folded = ssim_contract._folded

KERNEL = "metal/float_ms_ssim.metal"
HOST = "metal/float_ms_ssim_metal.mm"
MATH = "metal/metal_ms_ssim_math.h"
TERMS = ssim_contract.TERMS
DECIMATE = "ms_ssim_decimate.c"
REFERENCE = "ms_ssim.c"
CPU_EXTRACTOR = "float_ms_ssim.c"
MATH_TEST = ROOT / "core" / "test" / "test_metal_float_ms_ssim_math.c"

DOUBLE_TYPE = ssim_contract.DOUBLE_TYPE
REDUCTION = ssim_contract.REDUCTION
FORCED_ONE = ssim_contract.FORCED_ONE
HOST_ONLY = ssim_contract.HOST_ONLY
SUBSET = ssim_contract.SUBSET

KERNEL_PIECES = (
    ('#include "metal_ms_ssim_math.h"', "the kernel does not include the decimation header"),
    ('#include "metal_ssim_terms.h"', "the kernel does not include the shared terms header"),
    ("kernel void ms_ssim_decimate_h(", "the horizontal decimation kernel is gone"),
    ("kernel void ms_ssim_decimate_v(", "the vertical decimation kernel is gone"),
    ("kernel void ms_ssim_horiz(", "the horizontal window kernel is gone"),
    ("kernel void ms_ssim_vert_lcs(", "the window term kernel is gone"),
    (
        "acc = vmaf_mtl_msdec_tap(acc, src[gid.y * dims.width + (uint)xi],"
        " vmaf_mtl_msdec_lpf[tap]);",
        "the horizontal decimation tap is not one fused multiply-add",
    ),
    (
        "acc = vmaf_mtl_msdec_tap(acc, tmp[(uint)yi * dims.output_width + gid.x],"
        " vmaf_mtl_msdec_lpf[tap]);",
        "the vertical decimation tap is not one fused multiply-add",
    ),
    ("float acc = 0.0f;", "the decimation does not start its sum at zero"),
    ("vmaf_mtl_ssim_add_horizontal_tap(sums, ref_in[index], cmp_in[index],", "pair sum is gone"),
    ("vmaf_mtl_ssim_add_vertical_tap(sums, row, vmaf_mtl_ssim_gauss[tap]);", "pair sum is gone"),
    ("vmaf_mtl_ssim_double_terms(", "the fp64 quotients are not the header's"),
    ("luminance[index] = vmaf_mtl_signed_bits(terms.luminance);", "lv is not stored"),
    ("contrast[index] = vmaf_mtl_signed_bits(terms.contrast);", "cv is not stored"),
    ("structure[index] = terms.structure;", "sv is not stored"),
    (
        "const uint index = params.offset + gid.y * params.final_width + gid.x;",
        "the terms are not stored at their raster position of the region",
    ),
)

HOST_PIECES = (
    (
        "vmaf_mtl_ssim_lcs_sums(lum + offset, con + offset, str + offset, windows)",
        "the terms of a region are not added in raster order",
    ),
    ("vmaf_mtl_ms_ssim_scale_mean(sums.luminance, pixels)", "the l mean is not rounded to fp32"),
    ("vmaf_mtl_ms_ssim_scale_mean(sums.contrast, pixels)", "the c mean is not rounded to fp32"),
    ("vmaf_mtl_ms_ssim_scale_mean(sums.structure, pixels)", "the s mean is not rounded to fp32"),
    (
        "vmaf_mtl_ms_ssim_combine(l_means, c_means, s_means)",
        "the scales are not combined as ms_ssim.c does",
    ),
    ("picture_copy((float *)[dst contents]", "the planes are not the CPU's picture_copy()"),
    ('@"ms_ssim_decimate_h"', "the host does not build the horizontal decimation"),
    ('@"ms_ssim_decimate_v"', "the host does not build the vertical decimation"),
    ('@"ms_ssim_vert_lcs"', "the host does not build the window term kernel"),
    ("vmaf_mtl_msdec_extent(geom->scale_w[i - 1])", "the pyramid extents are not the CPU's"),
    ("vmaf_metal_ms_ssim_max_db(s->clip_db, bpc, w, h)", "the dB ceiling is not the CPU's"),
    ("vmaf_ms_ssim_emit_scores(", "the scores do not go through the CPU's helper"),
    ("[enc setBytes:&params length:sizeof(params) atIndex:4];", "kernel arguments are not shared"),
)
HOST_FORBIDDEN = (
    (re.compile(r"\bl_partials\b|\bc_partials\b|\bs_partials\b|\bpartials\b"), "float partials"),
    (re.compile(r"\bsimd_sum\b|\bwg_reduce"), "a device reduction is back"),
    (re.compile(r"\bg_alphas\b|\bg_betas\b|\bg_gammas\b"), "a private copy of the Wang weights"),
    (re.compile(r"\binv_scaler\b|\bs->scaler\b"), "the host normalises the planes itself"),
    (re.compile(r"\"ms_ssim_decimate\""), "the unfused one-kernel decimation is back"),
    (re.compile(r"\bpow\("), "the combine is written in the host file"),
)

# The lines of the CPU the headers mirror (comments stripped, whitespace folded).
DECIMATE_LINES = (
    "acc = vmaf_fmaf_exact(src_row[xi], ms_ssim_lpf_h[k], acc);",
    "acc = vmaf_fmaf_exact(tmp[(size_t)yi * (size_t)w_out + (size_t)x_out], ms_ssim_lpf_v[k], acc);",
    "const int w_out = (w / 2) + (w & 1);",
    "const int h_out = (h / 2) + (h & 1);",
    "const int xi = ms_ssim_decimate_mirror(x_src + k - half, w);",
    "const int yi = ms_ssim_decimate_mirror(y_src + k - half, h);",
)
REFERENCE_LINES = (
    "msssim *= pow(fabs((double)l), (double)alphas[idx]) * "
    "pow(fabs((double)c), (double)betas[idx]) * pow(fabs((double)s), (double)gammas[idx]);",
    "cur_w = cur_w / 2 + (cur_w & 1);",
    "iqa_ssim(ref_img, cmp_img, cur_w, cur_h, window, NULL, NULL, l, c, s);",
)
MATH_STEPS = {
    "a decimation tap is one fused multiply-add": ("return VMAF_MTL_FMA(sample, tap, acc);",),
    "the mirror is period 2n": (
        "const vmaf_mtl_i32 period = 2 * n;",
        "vmaf_mtl_i32 r = idx % period;",
        "r = period - r - 1;",
    ),
    "the extent is n / 2 + (n & 1)": ("return (n / 2u) + (n & 1u);",),
    "the combine multiplies the three powers left to right": (
        "score *= pow(fabs(luminance[scale]), (double)vmaf_mtl_ms_ssim_alphas[scale]) *",
        "pow(fabs(contrast[scale]), (double)vmaf_mtl_ms_ssim_betas[scale]) *",
        "pow(fabs(structure[scale]), (double)vmaf_mtl_ms_ssim_gammas[scale]);",
    ),
    "the scale mean is rounded to fp32": ("return (double)(float)(sum / pixels);",),
}
LPF = re.compile(r"ms_ssim_lpf_h\[MS_SSIM_DECIMATE_LPF_LEN\]\s*=\s*\{([^}]*)\}")
LPF_METAL = re.compile(r"vmaf_mtl_msdec_lpf\[VMAF_MTL_MSDEC_TAPS\]\s*=\s*\{([^}]*)\}")
WANG = ("alphas", "betas", "gammas")


def _sources() -> dict[str, str]:
    names = (KERNEL, HOST, MATH, TERMS, DECIMATE, REFERENCE, CPU_EXTRACTOR)
    sources = {name: (FEATURE_ROOT / name).read_text(encoding="utf-8") for name in names}
    sources["test"] = MATH_TEST.read_text(encoding="utf-8")
    return sources


def _numbers(source: str, pattern: re.Pattern[str]) -> list[str]:
    match = pattern.search(_code(source))
    return re.findall(r"-?\d\.\d+f", match.group(1)) if match else []


def _kernel_failures(kernel: str) -> list[str]:
    failures: list[str] = []
    code = _folded(kernel)
    for piece, what in KERNEL_PIECES:
        if re.sub(r"\s+", " ", piece) not in code:
            failures.append(f"{KERNEL}: {what}")
    if REDUCTION.search(code):
        failures.append(f"{KERNEL}: the terms are reduced on the device")
    if DOUBLE_TYPE.search(code):
        failures.append(f"{KERNEL}: the kernel uses double")
    if FORCED_ONE.search(code):
        failures.append(f"{KERNEL}: a forced 1 or a clamp on a window's terms")
    if "/" in code or re.search(r"\bsqrt\(", code):
        failures.append(f"{KERNEL}: the window values are formed outside the header")
    if re.search(r"\bacc\s*\+=|\brow_acc\b|\bacc\s*=\s*acc\s*\+", code):
        failures.append(f"{KERNEL}: an unfused decimation sum")
    return failures


def _host_failures(host: str, cpu: str) -> list[str]:
    failures: list[str] = []
    code = _folded(host)
    for piece, what in HOST_PIECES:
        if re.sub(r"\s+", " ", piece) not in code:
            failures.append(f"{HOST}: {what}")
    for pattern, what in HOST_FORBIDDEN:
        if pattern.search(code):
            failures.append(f"{HOST}: {what}")
    if ssim_contract._options(host) != ssim_contract._options(cpu):
        failures.append(f"{HOST}: the option table is not float_ms_ssim.c's")
    return failures


def _math_failures(math: str) -> list[str]:
    failures: list[str] = []
    code = _folded(math)
    device = _folded(math.split(HOST_ONLY)[0])
    for what, lines in MATH_STEPS.items():
        for line in lines:
            if re.sub(r"\s+", " ", line) not in code:
                failures.append(f"{MATH}: {what}: `{line.strip()}` is gone")
    for pattern, what in SUBSET:
        if pattern.search(device):
            failures.append(f"{MATH}: leaves the MSL/C/C++ subset: {what}")
    if re.search(r"\*\s*tap\s*\+\s*acc|acc\s*\+\s*sample\s*\*", device):
        failures.append(f"{MATH}: a decimation tap is not one fused multiply-add")
    return failures


def _reference_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    decimate = _folded(sources[DECIMATE])
    for line in DECIMATE_LINES:
        if line not in decimate:
            failures.append(f"{DECIMATE}: no longer holds `{line}`; the twin mirrors it")
    reference = _folded(sources[REFERENCE])
    for line in REFERENCE_LINES:
        if line not in reference:
            failures.append(f"{REFERENCE}: no longer holds `{line}`; the twin mirrors it")
    taps = _numbers(sources[DECIMATE], LPF)
    if len(taps) != 9 or taps != _numbers(sources[MATH], LPF_METAL):
        failures.append(f"{MATH}: the low-pass taps are not ms_ssim_decimate.c's")
    for name in WANG:
        cpu = re.findall(rf"g_{name}\[\]\s*=\s*\{{([^}}]*)\}}", _code(sources[REFERENCE]))
        cpu_taps = re.findall(r"\d\.\d+f", cpu[0]) if cpu else []
        metal = re.findall(
            rf"vmaf_mtl_ms_ssim_{name}\[VMAF_MTL_MS_SSIM_SCALES\]\s*=\s*\{{([^}}]*)\}}",
            _code(sources[MATH]),
        )
        metal_taps = re.findall(r"\d\.\d+f", metal[0]) if metal else []
        if len(cpu_taps) != 5 or cpu_taps != metal_taps:
            failures.append(f"{MATH}: the Wang {name} are not ms_ssim.c's")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST], sources[CPU_EXTRACTOR])
        + _math_failures(sources[MATH])
        + ssim_contract._terms_failures(sources[TERMS])
        + _reference_failures(sources)
    )


def _replaced(name: str, old: str, new: str) -> list[str]:
    """The contract's failures with `old` replaced by `new` in one source."""
    sources = _sources()
    pattern = re.compile(r"\s+".join(re.escape(part) for part in old.split()))
    if not pattern.search(sources[name]):
        raise AssertionError(f"{name}: `{old}` not found, the planted regression is stale")
    sources[name] = pattern.sub(lambda _: new, sources[name], count=1)
    return _contract_failures(sources)


def _appended(name: str, text: str) -> list[str]:
    sources = _sources()
    sources[name] += text
    return _contract_failures(sources)


class FloatMsSsimMetalExactContract(unittest.TestCase):
    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_unfused_decimation_tap_is_detected(self) -> None:
        # The pre-port kernel: `row_acc += src[...] * LPF[ku];`.
        failures = _replaced(
            MATH, "return VMAF_MTL_FMA(sample, tap, acc);", "return sample * tap + acc;"
        )
        self._assert_detected(failures, "decimation tap is one fused multiply-add")

    def test_unfused_kernel_sum_is_detected(self) -> None:
        failures = _appended(KERNEL, "\ninline float t(float acc, float x) { acc += x; return acc; }\n")
        self._assert_detected(failures, "unfused decimation sum")

    def test_one_pass_decimation_is_detected(self) -> None:
        # The pre-port single `ms_ssim_decimate` kernel (rows folded in one thread).
        failures = _replaced(HOST, '@"ms_ssim_decimate_h"', '@"ms_ssim_decimate"')
        self._assert_detected(failures, "unfused one-kernel decimation")

    def test_ceiling_extent_is_detected(self) -> None:
        failures = _replaced(MATH, "return (n / 2u) + (n & 1u);", "return n / 2u;")
        self._assert_detected(failures, "n / 2 + (n & 1)")

    def test_unmirrored_period_is_detected(self) -> None:
        failures = _replaced(
            MATH, "const vmaf_mtl_i32 period = 2 * n;", "const vmaf_mtl_i32 period = n;"
        )
        self._assert_detected(failures, "mirror is period 2n")

    def test_simd_sum_reduction_is_detected(self) -> None:
        failures = _appended(KERNEL, "\ninline float r(float t) { return simd_sum(t); }\n")
        self._assert_detected(failures, "reduced on the device")

    def test_fp32_window_quotients_are_detected(self) -> None:
        # The pre-port kernel: `my_l = (2.0f * mu_r * mu_c + c1) / (...)` in fp32.
        failures = _appended(KERNEL, "\ninline float q(float a, float b) { return a / b; }\n")
        self._assert_detected(failures, "formed outside the header")

    def test_unstored_term_is_detected(self) -> None:
        failures = _replaced(
            KERNEL, "luminance[index] = vmaf_mtl_signed_bits(terms.luminance);", "float unused;"
        )
        self._assert_detected(failures, "lv is not stored")

    def test_region_offset_is_detected(self) -> None:
        failures = _replaced(
            KERNEL,
            "const uint index = params.offset + gid.y * params.final_width + gid.x;",
            "const uint index = gid.y * params.final_width + gid.x;",
        )
        self._assert_detected(failures, "raster position of the region")

    def test_float_partials_on_the_host_are_detected(self) -> None:
        failures = _appended(HOST, "\nstatic float p(const float *l_partials) { return l_partials[0]; }\n")
        self._assert_detected(failures, "float partials")

    def test_host_sum_of_a_part_is_detected(self) -> None:
        failures = _replaced(
            HOST,
            "vmaf_mtl_ssim_lcs_sums(lum + offset, con + offset, str + offset, windows)",
            "vmaf_mtl_ssim_lcs_sums(lum + offset, con + offset, str + offset, 1u)",
        )
        self._assert_detected(failures, "raster order")

    def test_unrounded_mean_is_detected(self) -> None:
        failures = _replaced(
            MATH, "return (double)(float)(sum / pixels);", "return sum / pixels;"
        )
        self._assert_detected(failures, "rounded to fp32")

    def test_plain_combine_is_detected(self) -> None:
        # No fabs() on the means: pow() of a negative mean is NaN.
        failures = _replaced(
            MATH,
            "pow(fabs(contrast[scale]), (double)vmaf_mtl_ms_ssim_betas[scale]) *",
            "pow(contrast[scale], (double)vmaf_mtl_ms_ssim_betas[scale]) *",
        )
        self._assert_detected(failures, "three powers left to right")

    def test_private_wang_weights_are_detected(self) -> None:
        failures = _appended(HOST, "\nstatic const float g_alphas[5] = {0};\n")
        self._assert_detected(failures, "Wang weights")

    def test_host_normalisation_is_detected(self) -> None:
        failures = _replaced(
            HOST, "picture_copy((float *)[dst contents]", "convert((float *)[dst contents]"
        )
        self._assert_detected(failures, "picture_copy()")

    def test_changed_option_table_is_detected(self) -> None:
        failures = _replaced(HOST, 'enable_chroma",', 'enable_chroma_x",')
        self._assert_detected(failures, "option table")

    def test_changed_low_pass_is_detected(self) -> None:
        failures = _replaced(MATH, "0.026727f, -0.016828f,", "0.026728f, -0.016828f,")
        self._assert_detected(failures, "low-pass taps")

    def test_changed_wang_weight_is_detected(self) -> None:
        failures = _replaced(MATH, "0.0448f, 0.2856f, 0.3001f,", "0.0449f, 0.2856f, 0.3001f,")
        self._assert_detected(failures, "Wang betas")

    def test_changed_reference_is_detected(self) -> None:
        failures = _replaced(
            DECIMATE,
            "acc = vmaf_fmaf_exact(src_row[xi], ms_ssim_lpf_h[k], acc);",
            "acc += src_row[xi] * ms_ssim_lpf_h[k];",
        )
        self._assert_detected(failures, "the twin mirrors it")

    def test_shared_terms_header_is_pinned_too(self) -> None:
        failures = _replaced(
            TERMS,
            "const float ref_var = ref_var_raw < 0.0f ? 0.0f : ref_var_raw;",
            "const float ref_var = ref_var_raw;",
        )
        self._assert_detected(failures, "the variances are clamped at zero")


if __name__ == "__main__":
    unittest.main()
