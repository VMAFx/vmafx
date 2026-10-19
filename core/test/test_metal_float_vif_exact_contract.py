#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the bit-exact float_vif_metal design (ADR-1498, after ADR-1422).

The CPU extractor (``float_vif.c`` / ``vif.c`` / ``vif_tools.c``) fixes four
things a twin has to copy to return its bits, and Metal adds one limit:

- the Gaussian taps are ``vif_get_filter()``'s, derived at run time in fp32
  and handed to the kernels as an argument;
- ``log2f`` is the polynomial ``log2f_approx()`` (``VIF_OPT_FAST_LOG2``), not
  a library call, and its coefficients are fp32 bit patterns because Metal has
  no fp64 type to round a decimal literal through;
- ``vif_sigma_nsq`` is a ``double``, so the two log arguments are fp64
  quotients and sums rounded to fp32 once. A Metal kernel has no fp64 type
  (Metal Shading Language Specification 4.1, section 2.1):
  ``metal_float_vif_math.h`` evaluates them as exact fp32 pairs and replays
  the reference's fp64 operations in integers next to a rounding boundary
  (``core/src/feature/sycl/sycl_float_vif_math.h`` is the same arithmetic in
  SYCL, ADR-1422);
- the per-pixel terms are added row by row into one fp32 accumulator, and the
  rows into another;
- the option table is the CPU's, and every option runs on the device.

Device-free: reads the sources only. Every planted regression below is a
construct the pre-ADR-1498 twin had (a decimal tap table, a device ``log2``,
fp32 ``vif_sigma_nsq``, a per-threadgroup sum, a refused kernelscale) or a
shortcut that loses a bit, so the contract fails on the old design and passes
on the new one. ``test_metal_float_vif_math`` checks the header's arithmetic
against the CPU routines on the host and ``test_metal_float_vif_parity``
checks the scores on an Apple device; this contract keeps the design from
eroding on hosts without one.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

KERNEL = "metal/float_vif.metal"
HOST = "metal/float_vif_metal.mm"
MATH = "metal/metal_float_vif_math.h"
SYCL_MATH = "sycl/sycl_float_vif_math.h"
CPU_OPTIONS = "vif_options.h"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A decimal fp32 literal with at least seven fraction digits: a filter tap
# written into the source instead of taken from vif_get_filter().
TAP_LITERAL = re.compile(r"\b0\.\d{7,}f\b")
# A decimal literal long enough to be a polynomial coefficient (with or
# without an `f` suffix): Metal has no fp64 type to round it through.
DECIMAL_COEFFICIENT = re.compile(r"(?<![\w.])-?\d+\.\d{10,}f?\b")
LIBRARY_LOG2 = re.compile(r"\b(?:metal|fast|precise|std)::log2f?\s*\(|(?<![\w:])log2f?\s*\(")
OTHER_REDUCTION = re.compile(
    r"\bsimd_\w+\s*\(|\batomic_\w+|\bthreadgroup\b|\bthreadgroup_barrier\b|\bquad_\w+\s*\("
)
FP64 = re.compile(r"\b(?:long\s+)?double\b")
LONG_LONG = re.compile(r"\blong\s+long\b|\d[uU]?[lL][lL]\b")
STATIC = re.compile(r"\bstatic\b")
OPTION_NAMES = (
    ("debug", None),
    ("vif_enhn_gain_limit", "egl"),
    ("vif_kernelscale", "ks"),
    ("vif_prescale", "ps"),
    ("vif_scale1_min_val", "s1miv"),
    ("vif_scale2_min_val", "s2miv"),
    ("vif_scale3_min_val", "s3miv"),
    ("vif_prescale_method", "pm"),
    ("vif_sigma_nsq", "snsq"),
    ("vif_skip_scale0", "ssclz"),
)

ROW_SUM = (
    "float numerator = 0.0f;",
    "float denominator = 0.0f;",
    "for (uint x = 0u; x < row.width; ++x) {",
    "const uint at = vmaf_mtl_fvif_term_index(x, y, row.height);",
    "numerator += terms[at];",
    "denominator += terms[at + 1u];",
    "uint y [[thread_position_in_grid]]",
)
HOST_FOLD = (
    "float num = 0.0f;",
    "float den = 0.0f;",
    "for (unsigned y = 0; y < s->scale_h[scale]; ++y) {",
    "num += num_rows[y];",
    "den += den_rows[y];",
    "scores[2 * scale + 0] = (double)num;",
)
STATISTIC = (
    "const float gain_den = sigma1_sq + eps;",
    "float g = sigma12 / gain_den;",
    "const float product = gain_sq * sigma1_sq;",
    "vmaf_mtl_fvif_one_plus_ratio(product, vmaf_mtl_fvif_noise_plus(sv_sq, p.noise)));",
    "vmaf_mtl_fvif_one_plus_ratio(sigma1_sq, vmaf_mtl_fvif_noise_denominator(p.noise)));",
    "if (sigma1_sq < p.noise.above) {",
)
# The same statements in the SYCL header (ADR-1422): the two copies are one
# behaviour until T-METAL-SYCL-FP64-FREE-ARITHMETIC-COPIES-2026-10-03.
SYCL_STATISTIC = (
    "const float gain_den = sigma1_sq + eps;",
    "float g = sigma12 / gain_den;",
    "const float product = gain_sq * sigma1_sq;",
    ".num = log2_approx(one_plus_ratio(product, noise_plus(sv_sq, p.noise))),",
    ".den = log2_approx(one_plus_ratio(sigma1_sq, noise_denominator(p.noise))),",
    "if (sigma1_sq < p.noise.above) {",
)
REPLAY = (
    "return vmaf_mtl_fvif_needs_replay(numerator, sum, denominator) ?",
    "vmaf_mtl_fvif_one_plus_ratio_replayed(numerator, denominator.exact) :",
)
LOG2_STEPS = 9
TERM_PIECES = (
    "terms[out] = term.num;",
    "terms[out + 1u] = term.den;",
    "vmaf_mtl_fvif_pixel_term(mu1, mu2, xx, yy, xy, params)",
)
SIGMA_PIECES = ("const float mu1_sq = mu1 * mu1;", "const float mu1_mu2 = mu1 * mu2;")
KERNEL_NAMES = (
    "float_vif_vertical",
    "float_vif_compute",
    "float_vif_row_sums",
    "float_vif_decimate",
)
# The functions of the math header that are host code and use fp64.
HOST_MARKER = "#if !defined(__METAL_VERSION__)\n\n"


TAP_BINDINGS = 3  # scales whose taps the host binds and the kernel declares


def _code(source: str) -> str:
    """The source with its comments blanked, so prose cannot trip a check."""
    return COMMENT.sub(" ", source)


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (KERNEL, HOST, MATH, SYCL_MATH, CPU_OPTIONS)
    }


def _function_body(source: str, name: str) -> str:
    """Text of the first definition of `name` (brace-matched), or empty."""
    match = re.search(rf"\b{name}\([^;{{]*\)\s*\{{", source)
    if not match:
        return ""
    depth = 0
    for index in range(match.end() - 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : index + 1]
    return ""


def _require(body: str, pieces: tuple[str, ...], message: str) -> list[str]:
    return [f"{message} ({piece})" for piece in pieces if piece not in body]


def _device_math(sources: dict[str, str]) -> str:
    """The math header without its host-only block (comments blanked)."""
    text = sources[MATH]
    index = text.rfind(HOST_MARKER)
    return _code(text if index < 0 else text[:index])


def _tap_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name in (KERNEL, HOST, MATH):
        if TAP_LITERAL.search(_code(sources[name])):
            failures.append(f"{name}: a filter tap is a source literal, not vif_get_filter()'s")
    host = _code(sources[HOST])
    init = _function_body(host, "init_filters")
    if "vif_get_filter(s->taps[scale], scale, (float)s->vif_kernelscale);" not in init:
        failures.append(f"{HOST}: the taps no longer come from vif_get_filter()")
    if host.count("[enc setBytes:s->taps[scale]") != TAP_BINDINGS:
        failures.append(f"{HOST}: the vertical, compute and decimate launches must take the taps")
    kernel = _code(sources[KERNEL])
    if kernel.count("constant float *taps [[buffer(") != TAP_BINDINGS:
        failures.append(f"{KERNEL}: every filtering kernel must read its taps from an argument")
    for name in ("float_vif_vertical", "float_vif_compute", "float_vif_decimate"):
        if "taps[" not in _function_body(kernel, name):
            failures.append(f"{KERNEL}: {name}() does not read its taps argument")
    return failures


def _log2_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    if "#define VIF_OPT_FAST_LOG2" not in _code(sources[CPU_OPTIONS]):
        failures.append(
            f"{CPU_OPTIONS}: VIF_OPT_FAST_LOG2 is gone; the CPU's log2f is libm's now and "
            "the twin's polynomial no longer mirrors it"
        )
    for name in (KERNEL, HOST, MATH):
        code = _code(sources[name])
        if LIBRARY_LOG2.search(code):
            failures.append(f"{name}: a library log2 instead of log2f_approx()'s polynomial")
    for name in (KERNEL, MATH):
        if DECIMAL_COEFFICIENT.search(_code(sources[name])):
            failures.append(
                f"{name}: a decimal coefficient literal; Metal has no fp64 type to round it "
                "through, the polynomial is fp32 bit patterns"
            )
    log2 = _function_body(_code(sources[MATH]), "vmaf_mtl_fvif_log2")
    if log2.count("vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C") != LOG2_STEPS:
        failures.append(f"{MATH}: log2 is not nine horner_s() steps over log2_poly_s")
    return failures


def _statistic_failures(sources: dict[str, str]) -> list[str]:
    math = _code(sources[MATH])
    failures = _require(
        _function_body(math, "vmaf_mtl_fvif_pixel_statistic"),
        STATISTIC,
        f"{MATH}: vmaf_mtl_fvif_pixel_statistic() is not vif_pixel_statistic_s()",
    )
    failures += _require(
        _function_body(math, "vmaf_mtl_fvif_pixel_sigmas"),
        SIGMA_PIECES,
        f"{MATH}: vmaf_mtl_fvif_pixel_sigmas() is not vif_pixel_statistic_s()'s first half",
    )
    failures += _require(
        _function_body(math, "vmaf_mtl_fvif_one_plus_ratio"),
        REPLAY,
        f"{MATH}: one_plus_ratio() must replay the fp64 operations next to a rounding boundary",
    )
    device = _device_math(sources)
    if FP64.search(device) or LONG_LONG.search(device):
        failures.append(f"{MATH}: an fp64 or long long type outside the host-only block")
    if STATIC.search(device):
        failures.append(f"{MATH}: `static` outside the host-only block (MSL reserves it)")
    sycl = _code(sources[SYCL_MATH])
    for piece in SYCL_STATISTIC:
        if piece not in sycl:
            failures.append(f"{SYCL_MATH}: the statement the Metal header mirrors changed: {piece}")
    return failures


def _sum_failures(sources: dict[str, str]) -> list[str]:
    kernel = _code(sources[KERNEL])
    failures = _require(
        _function_body(kernel, "float_vif_row_sums"),
        ROW_SUM,
        f"{KERNEL}: float_vif_row_sums() is not the CPU's row sum, one thread per row",
    )
    if OTHER_REDUCTION.search(kernel):
        failures.append(
            f"{KERNEL}: a SIMD-group, threadgroup or atomic reduction adds the terms in an "
            "order the CPU does not use"
        )
    host = _code(sources[HOST])
    failures += _require(
        _function_body(host, "sum_rows"),
        HOST_FOLD,
        f"{HOST}: sum_rows() is not the CPU's fp32 sum over the rows",
    )
    return failures


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = _code(sources[KERNEL])
    failures: list[str] = []
    if FP64.search(kernel) or LONG_LONG.search(kernel):
        failures.append(f"{KERNEL}: an fp64 or long long type (Metal has none)")
    if "std::" in kernel:
        failures.append(f"{KERNEL}: the C++ standard library does not exist in MSL")
    for name in KERNEL_NAMES:
        if f"kernel void {name}(" not in kernel:
            failures.append(f"{KERNEL}: kernel {name} is missing")
    failures += _require(
        _function_body(kernel, "float_vif_compute"),
        TERM_PIECES,
        f"{KERNEL}: float_vif_compute() does not store the statistic's terms",
    )
    if "vmaf_mtl_fvif_raw_to_float(" not in _function_body(kernel, "fvif_read_raw"):
        failures.append(f"{KERNEL}: raw samples are not read as picture_copy() reads them")
    host = _code(sources[HOST])
    for name in KERNEL_NAMES:
        if f'@"{name}"' not in host:
            failures.append(f"{HOST}: the host does not load kernel {name}")
    return failures


def _option_failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    failures: list[str] = []
    for name, alias in OPTION_NAMES:
        if f'.name        = "{name}",' not in host:
            failures.append(f"{HOST}: the CPU's {name} option is missing")
        if alias is not None and f'.alias       = "{alias}",' not in host:
            failures.append(f"{HOST}: the CPU's {name} alias {alias} is missing")
    if "VMAF_OPT_FLAG_DEFAULT_ONLY" in host:
        failures.append(f"{HOST}: an option is default-only; the CPU's table has none")
    if re.search(r"vif_kernelscale\s*!=\s*1\.0", host):
        failures.append(f"{HOST}: a kernelscale other than 1.0 is refused; every one runs")
    if ".use_minimums = true," not in _function_body(host, "collect_fex_metal"):
        failures.append(f"{HOST}: the per-scale floors are not applied")
    prescale = _function_body(host, "prescale_plane")
    for piece in ("picture_copy(host,", "vif_scale_frame_s(s->scaling_method,"):
        if piece not in prescale:
            failures.append(f"{HOST}: the prescale is not the CPU's own ({piece})")
    return failures


def _signature_failures(sources: dict[str, str]) -> list[str]:
    """No device-function parameter is a pointer or a reference (MSL wants an
    address space on each, and the header is values in, values out)."""
    failures: list[str] = []
    for match in re.finditer(r"VMAF_MTL_FUNC\s+[\w\s]+?\s+(\w+)\(([^)]*)\)", _device_math(sources)):
        if "*" in match.group(2) or "&" in match.group(2):
            failures.append(f"{MATH}: {match.group(1)}() takes a pointer or a reference")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    return (
        _tap_failures(sources)
        + _log2_failures(sources)
        + _statistic_failures(sources)
        + _sum_failures(sources)
        + _kernel_failures(sources)
        + _option_failures(sources)
        + _signature_failures(sources)
    )


class MetalFloatVifExactContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def _any(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in failure for failure in failures), failures)

    def test_live_sources_keep_the_exact_design(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_tap_literal_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "        const float c = taps[k];\n        const float r = fvif_sample(",
            "        const float c = k == 0u ? 0.00745626912f : taps[k];\n"
            "        const float r = fvif_sample(",
        )
        self._any(failures, "source literal")

    def test_decimal_tap_table_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "#define FVIF_TG 16",
            "#define FVIF_TG 16\nconstant float FVIF_COEFF_S3[3] = {0.166378498f, 0.667243004f, "
            "0.166378498f};",
        )
        self._any(failures, "source literal")

    def test_taps_not_from_vif_get_filter_are_detected(self) -> None:
        failures = self._edited(
            HOST,
            "vif_get_filter(s->taps[scale], scale, (float)s->vif_kernelscale);",
            "fill_table(s->taps[scale], scale);",
        )
        self._any(failures, "vif_get_filter()")

    def test_taps_not_passed_to_a_launch_are_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    [enc setBytes:s->taps[scale] length:sizeof(s->taps[scale]) atIndex:6];",
            "    [enc setBytes:&dims length:sizeof(dims) atIndex:6];",
        )
        self._any(failures, "must take the taps")

    def test_device_log2_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "vmaf_mtl_fvif_log2(\n        vmaf_mtl_fvif_one_plus_ratio(product, vmaf_mtl_fvif_noise_plus(sv_sq, p.noise)));",
            "metal::log2(\n        vmaf_mtl_fvif_one_plus_ratio(product, vmaf_mtl_fvif_noise_plus(sv_sq, p.noise)));",
        )
        self._any(failures, "library log2")

    def test_decimal_polynomial_coefficient_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const float scaled = var * x;\n    return scaled + VMAF_MTL_U2F(coeff_bits);",
            "    const float scaled = var * x;\n    return scaled + 1.442694803896991f;",
        )
        self._any(failures, "decimal coefficient")

    def test_short_polynomial_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C8);\n",
            "",
        )
        self._any(failures, "nine horner_s() steps")

    def test_fp32_noise_variance_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "vmaf_mtl_fvif_one_plus_ratio(sigma1_sq, vmaf_mtl_fvif_noise_denominator(p.noise)));",
            "1.0f + sigma1_sq / p.noise.hi);",
        )
        self._any(failures, "vif_pixel_statistic_s()")

    def test_dropped_replay_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    return vmaf_mtl_fvif_needs_replay(numerator, sum, denominator) ?\n"
            "               vmaf_mtl_fvif_one_plus_ratio_replayed(numerator, denominator.exact) :\n"
            "               sum.hi;",
            "    return sum.hi;",
        )
        self._any(failures, "rounding boundary")

    def test_fp64_in_the_device_statistic_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const float gain_sq = g * g;",
            "    const float gain_sq = (float)((double)g * (double)g);",
        )
        self._any(failures, "fp64 or long long")

    def test_fp64_in_a_kernel_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    float acc_ref = 0.0f;",
            "    double acc_ref = 0.0;",
        )
        self._any(failures, "fp64 or long long")

    def test_reference_taken_by_pointer_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "VMAF_MTL_FUNC bool vmaf_mtl_fvif_finite(float x)",
            "VMAF_MTL_FUNC bool vmaf_mtl_fvif_finite(float &x)",
        )
        self._any(failures, "pointer or a reference")

    def test_simd_group_reduction_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    terms[out + 1u] = term.den;",
            "    terms[out + 1u] = simd_sum(term.den);",
        )
        self._any(failures, "order the CPU does not use")

    def test_threadgroup_sum_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    float numerator = 0.0f;\n    float denominator = 0.0f;\n    for (uint x",
            "    threadgroup float partial[32];\n    float numerator = 0.0f;\n"
            "    float denominator = 0.0f;\n    for (uint x",
        )
        self._any(failures, "order the CPU does not use")

    def test_strided_row_sum_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    for (uint x = 0u; x < row.width; ++x) {",
            "    for (uint x = 0u; x < row.width; x += 2u) {",
        )
        self._any(failures, "row sum")

    def test_blocked_row_launch_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "                               uint y [[thread_position_in_grid]])\n{\n    if (y >= row.height)",
            "                               uint2 y [[thread_position_in_grid]])\n{\n    if (y >= row.height)",
        )
        self._any(failures, "one thread per row")

    def test_fp64_host_fold_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    float num = 0.0f;\n        float den = 0.0f;",
            "    double num = 0.0;\n        double den = 0.0;",
        )
        self._any(failures, "sum over the rows")

    def test_missing_scale_floor_is_detected(self) -> None:
        failures = self._edited(
            HOST, '.name        = "vif_scale2_min_val",', '.name        = "vif_s2",'
        )
        self._any(failures, "vif_scale2_min_val")

    def test_default_only_kernelscale_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "        .max         = 4.0,\n        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,\n    },\n"
            '    {\n        .name        = "vif_prescale",',
            "        .max         = 4.0,\n        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM | "
            'VMAF_OPT_FLAG_DEFAULT_ONLY,\n    },\n    {\n        .name        = "vif_prescale",',
        )
        self._any(failures, "default-only")

    def test_refused_kernelscale_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    int err = init_geometry(s, w, h);",
            "    if (s->vif_kernelscale != 1.0) { return -EINVAL; }\n    int err = init_geometry(s, w, h);",
        )
        self._any(failures, "kernelscale other than 1.0")

    def test_dropped_floors_are_detected(self) -> None:
        failures = self._edited(HOST, ".use_minimums = true,", ".use_minimums = false,")
        self._any(failures, "floors")

    def test_hand_made_prescale_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    vif_scale_frame_s(s->scaling_method, host,",
            "    scale_nearest(s->scaling_method, host,",
        )
        self._any(failures, "CPU's own")

    def test_mirrored_statement_drift_is_detected(self) -> None:
        failures = self._edited(
            SYCL_MATH,
            "    const float gain_den = sigma1_sq + eps;",
            "    const float gain_den = eps + sigma1_sq;",
        )
        self._any(failures, "the Metal header mirrors")

    def test_missing_kernel_is_detected(self) -> None:
        failures = self._edited(KERNEL, "kernel void float_vif_row_sums(", "kernel void rows(")
        self._any(failures, "float_vif_row_sums is missing")


if __name__ == "__main__":
    unittest.main()
