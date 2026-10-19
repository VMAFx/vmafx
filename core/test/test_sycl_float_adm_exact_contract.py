#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CPU arithmetic of float_adm_sycl (ADR-1434).

``float_adm_sycl`` returns the CPU ``float_adm`` outputs bit for bit because

* the decouple divides as ``adm_tools.c::DIVS()`` does: the IEEE fp32
  quotient (ADR-1442), not a product with a reciprocal;
* the angle threshold is ``(cos^2 * |o|^2) * |t|^2`` in that association;
* the three expressions the reference evaluates in fp64 (the enhancement
  gain, ``FLOAT_ONE_BY_30 * fabsf(x)`` and the centre tap's
  ``FLOAT_ONE_BY_15 * fabsf(x)``) are evaluated without an fp64 type
  (ADR-0220): as exact fp32 pairs, with a replay of the fp64 operations in
  integers where a pair does not decide the rounding;
* the masking threshold is one sum per band with the centre tap fifth;
* every reduction adds a row left to right in fp32 and then the rows, and
* the CSF weights, the reduced region, the pooling and the frame floor are the
  reference's own routines and expression;
* the term kernel keeps the large register file with the sub-group size left
  to the compiler (ADR-1501), in the twin and in the probe, which spills on no
  default AOT target, and the probe's queue is in order, as libvmaf's is: its
  three kernels each read what the one before wrote.

Device-free: reads the sources only. ``test_sycl_float_adm_math`` checks the
arithmetic against ``adm_tools.c`` on the host and on a device, and
``test_sycl_float_adm_parity`` checks the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"
TEST_ROOT = ROOT / "core" / "test"

TWIN = "sycl/float_adm_sycl.cpp"
MATH = "sycl/sycl_float_adm_math.h"
CPU = "adm_tools.c"
CPU_FRAME = "adm.c"
MATH_TEST = "test_sycl_float_adm_math.c"
PROBE = "test_sycl_float_adm_math_probe.cpp"
TERMS_SHAPE = "VmafSyclKernelShape<vmaf_sycl_fadm::kTermsSubGroup, vmaf_sycl_fadm::kTermsGrf>"
TERMS_SHAPE_CONSTANTS = (
    "inline constexpr int kTermsSubGroup = 0;",
    "inline constexpr int kTermsGrf = 256;",
)

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")
FP64 = re.compile(r"\b(?:long\s+)?double\b")
# A quotient formed through a reciprocal: 1 / d, rcp, recip.
RECIPROCAL = re.compile(r"1\.0f\s*/|rcp|recip")
ONE_BY_30 = "0.0333333351"
ONE_BY_15 = "0.0666666701"
# The reference's lines the twin mirrors.
REFERENCE_LINES = (
    # The trailing space ends the literal: no `f` suffix, no further digit.
    f"#define FLOAT_ONE_BY_30 {ONE_BY_30} ",
    f"#define FLOAT_ONE_BY_15 {ONE_BY_15} ",
    "#define DIVS(n, d) ((n) / (d))",
    "return (ot_dp >= 0.0f) && (ot_dp * ot_dp >= cos_1deg_sq * o_mag_sq * t_mag_sq);",
    "float k = DIVS(t, o + eps);",
    "rst = (float)(MIN(rst * adm_enhn_gain_limit, t));",
    "rst = (float)(MAX(rst * adm_enhn_gain_limit, t));",
    "flt_ptr[dst_offset + j] = (float)(FLOAT_ONE_BY_30 * fabsf(dst_val));",
    "sum = (float)(sum + (FLOAT_ONE_BY_15 * fabsf(src_ptr[j])));",
)
FRAME_FLOOR = "1e-10 * (w * h) / (1920.0 * 1080.0)"
# Host code in the math header that may name the fp64 type.
HOST_ONLY = ("make_gain_limit",)
CONSTANT = re.compile(
    r"(kOneBy\d+) = \{ \.hi = (\S+)f, \.lo = (\S+)f, \.mant = (0x[0-9a-fA-F]+)ULL, \.exp = (-?\d+) ?\}"
)


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    sources = {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (TWIN, MATH, CPU, CPU_FRAME)
    }
    for name in (MATH_TEST, PROBE):
        sources[name] = (TEST_ROOT / name).read_text(encoding="utf-8")
    return sources


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


def _reference_failures(sources: dict[str, str]) -> list[str]:
    """The reference still holds the lines the twin and its test mirror."""
    failures: list[str] = []
    cpu = SPACE.sub(" ", sources[CPU])
    for line in REFERENCE_LINES:
        if line not in cpu:
            failures.append(f"{CPU}: no longer holds `{line}`")
    if FRAME_FLOOR not in _code(sources[CPU_FRAME]):
        failures.append(f"{CPU_FRAME}: compute_adm() no longer floors at `{FRAME_FLOOR}`")
    test = SPACE.sub(" ", sources[MATH_TEST])
    for name, value in (("REFERENCE_ONE_BY_30", ONE_BY_30), ("REFERENCE_ONE_BY_15", ONE_BY_15)):
        if f"#define {name} {value} " not in test:
            failures.append(f"{MATH_TEST}: {name} is not the reference's literal {value}")
    return failures


def _constant_failures(math: str) -> list[str]:
    """kOneBy30 / kOneBy15 are the reference's double literals."""
    failures: list[str] = []
    found = {m.group(1): m.groups()[1:] for m in CONSTANT.finditer(math)}
    for name, literal in (("kOneBy30", ONE_BY_30), ("kOneBy15", ONE_BY_15)):
        if name not in found:
            failures.append(f"{MATH}: {name} is not defined as hi, lo, mant, exp")
            continue
        hi, lo, mant, exp = found[name]
        value = float(literal)
        if int(mant, 16) * 2.0 ** int(exp) != value:
            failures.append(f"{MATH}: {name}.mant / .exp are not the double literal {literal}")
        pair = float.fromhex(hi) + float.fromhex(lo)
        if abs(pair - value) > value * 2.0**-48:
            failures.append(f"{MATH}: {name}.hi + .lo is not the double literal {literal}")
    return failures


def _division_failures(math: str) -> list[str]:
    divs = _function_body(math, "divs")
    failures: list[str] = []
    if "return n / d;" not in divs or RECIPROCAL.search(divs):
        failures.append(
            f"{MATH}: divs() must be DIVS(): the fp32 quotient n / d, with no reciprocal "
            "(ADR-1442)"
        )
    return failures


def _decouple_failures(math: str) -> list[str]:
    failures: list[str] = []
    angle = _function_body(math, "angle_flag")
    if (
        "const float scaled = cos_1deg_sq * o_mag_sq;" not in angle
        or "const float rhs = scaled * t_mag_sq;" not in angle
    ):
        failures.append(f"{MATH}: angle_flag() must compare with (cos^2 * |o|^2) * |t|^2")
    gain = _function_body(math, "gain_limited")
    if "if (limit.is_float != 0) {" not in gain or "soft_mul(" not in gain:
        failures.append(
            f"{MATH}: gain_limited() must replay the fp64 product for a limit that is not "
            "an fp32 value"
        )
    if "times_constant(sycl::fabs(csf), kOneBy30)" not in _function_body(math, "csf_flt"):
        failures.append(f"{MATH}: csf_flt() must multiply by the double FLOAT_ONE_BY_30")
    for name, replay in (
        ("times_constant", "times_constant_replayed(a, constant)"),
        ("add_scaled", "add_scaled_replayed(sum, a, constant)"),
    ):
        body = _function_body(math, name)
        if "undecided(pair)" not in body or replay not in body:
            failures.append(f"{MATH}: {name}() must replay what the pair does not decide")
    return failures


def _threshold_failures(math: str) -> list[str]:
    body = _function_body(math, "thresh_band")
    order = (
        "sum += n.above_left;",
        "sum += n.above;",
        "sum += n.above_right;",
        "sum += n.left;",
        "sum = add_scaled(sum, sycl::fabs(centre), kOneBy15);",
        "sum += n.right;",
        "sum += n.below_left;",
        "sum += n.below;",
        "sum += n.below_right;",
    )
    position = 0
    for piece in order:
        found = body.find(piece, position)
        if found < 0:
            return [
                f"{MATH}: thresh_band() must add the nine taps in the reference's order, "
                "the centre fifth as an fp64 addend"
            ]
        position = found + len(piece)
    return []


def _reduction_failures(math: str, twin: str) -> list[str]:
    failures: list[str] = []
    row = _function_body(math, "row_sum")
    if "float inner = 0.0f;" not in row or "inner += terms[(size_t)x * stride];" not in row:
        failures.append(f"{MATH}: row_sum() must add a row left to right in one fp32 accumulator")
    fold = _function_body(math, "fold_rows")
    if "float accum = 0.0f;" not in fold or "accum += rows[y];" not in fold:
        failures.append(f"{MATH}: fold_rows() must add the rows in one fp32 accumulator")
    if re.search(r"reduce_over_group|sycl::reduction|sycl::plus", twin + math):
        failures.append("a group reduction adds in another order than the reference's rows")
    launch = _function_body(twin, "launch_row_sums")
    if (
        "VMAF_SYCL_REQD_SG_SIZE(16)" not in launch
        or "vmaf_sycl_fadm::row_item(args, id[0]);" not in launch
    ):
        failures.append(f"{TWIN}: the row kernel must run row_item() per (slot, row)")
    if "vmaf_sycl_fadm::fold_rows(" not in _function_body(twin, "fadm_pool_scale"):
        failures.append(f"{TWIN}: the host must fold the rows with fold_rows()")
    return failures


def _device_type_failures(math: str) -> list[str]:
    device = math
    for name in HOST_ONLY:
        device = device.replace(_function_body(math, name), " ")
    failures: list[str] = []
    if FP64.search(device):
        failures.append(f"{MATH}: fp64 type outside the host-only helper (ADR-0220)")
    if "mul_hi" in math or "sycl::fma" in math:
        failures.append(f"{MATH}: sycl::mul_hi() / sycl::fma() are not the reference's operations")
    return failures


def _class_body(source: str, name: str) -> str:
    """Text of class `name` up to its closing `};`, or empty."""
    match = re.search(rf"\bclass {name}\b(.*?)\}};", source, re.S)
    return match.group(1) if match else ""


def _terms_kernel_failures(file: str, source: str, kernel: str, call: str) -> list[str]:
    """The term kernel's shape and work-item in the twin or the probe."""
    body = _class_body(source, kernel)
    if TERMS_SHAPE not in body or "vmaf_sycl_fadm::terms_sample(args_," not in body:
        return [f"{file}: {kernel} must run terms_sample() in the term kernel's shape (ADR-1501)"]
    if call not in source:
        return [f"{file}: the term kernel must be launched as `{call}`"]
    return []


def _terms_shape_failures(sources: dict[str, str], math: str, twin: str) -> list[str]:
    failures = [
        f"{MATH}: the term kernel's shape must be `{piece}` (ADR-1501)"
        for piece in TERMS_SHAPE_CONSTANTS
        if piece not in math
    ]
    probe = _code(sources[PROBE])
    failures += _terms_kernel_failures(TWIN, twin, "FadmTermsKernel", "FadmTermsKernel(args));")
    failures += _terms_kernel_failures(PROBE, probe, "TermsKernel", "TermsKernel(terms_of));")
    if "sycl::queue q(*device, sycl::property::queue::in_order{});" not in _function_body(
        probe, "on_default_gpu"
    ):
        failures.append(f"{PROBE}: the probe's queue must be in order, as libvmaf's is")
    return failures


def _twin_failures(twin: str) -> list[str]:
    failures: list[str] = []
    if "vmaf_sycl_fadm::decouple_sample(args, y, x);" not in _function_body(
        twin, "launch_decouple_csf"
    ):
        failures.append(f"{TWIN}: launch_decouple_csf() must run sycl_float_adm_math.h's work-item")
    for routine in (
        "adm_csf_rfactor_s(",
        "adm_border_s(",
        "adm_pool_bands_s(",
        "adm_decouple_cos_1deg_sq_s()",
        "vmaf_sycl_fadm::make_gain_limit(s->adm_enhn_gain_limit)",
    ):
        if routine not in twin:
            failures.append(f"{TWIN}: must take `{routine}` from the reference, not derive it")
    if re.search(r"\bpow\(|\bpowf\(|cbrt", twin):
        failures.append(f"{TWIN}: a pooling root of its own instead of adm_pool_bands_s()")
    if f"const double floor = {FRAME_FLOOR};" not in _function_body(twin, "fadm_finalize_scores"):
        failures.append(f"{TWIN}: the frame floor must be compute_adm()'s `{FRAME_FLOOR}`")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    math = _code(sources[MATH])
    twin = _code(sources[TWIN])
    return (
        _reference_failures(sources)
        + _constant_failures(math)
        + _division_failures(math)
        + _decouple_failures(math)
        + _threshold_failures(math)
        + _reduction_failures(math, twin)
        + _device_type_failures(math)
        + _twin_failures(twin)
        + _terms_shape_failures(sources, math, twin)
    )


class SyclFloatAdmExactContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def _detects(self, failures: list[str], text: str) -> None:
        self.assertTrue(any(text in failure for failure in failures), failures)

    def test_live_sources_keep_the_cpu_arithmetic(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_out_of_order_probe_queue_is_detected(self) -> None:
        # The B580 ran the probe's three kernels concurrently on this queue.
        failures = self._edited(
            PROBE,
            "sycl::queue q(*device, sycl::property::queue::in_order{});",
            "sycl::queue q(*device);",
        )
        self._detects(failures, "must be in order")

    def test_terms_kernel_default_shape_is_detected(self) -> None:
        # The lambda that spilled 128 bytes on an Arc B580.
        failures = self._edited(
            TWIN,
            "FadmTermsKernel(args));",
            "[=](sycl::id<2> r) { vmaf_sycl_fadm::terms_sample(args, (unsigned)r[0], (unsigned)r[1]); });",
        )
        self._detects(failures, "must be launched as")

    def test_terms_kernel_without_large_grf_is_detected(self) -> None:
        failures = self._edited(
            MATH, "inline constexpr int kTermsGrf = 256;", "inline constexpr int kTermsGrf = 0;"
        )
        self._detects(failures, "term kernel's shape")

    def test_probe_terms_kernel_shape_is_detected(self) -> None:
        failures = self._edited(PROBE, TERMS_SHAPE, "VmafSyclKernelShape<16, 0>")
        self._detects(failures, "TermsKernel must run terms_sample()")

    def test_reciprocal_division_is_detected(self) -> None:
        # Upstream's ADM_OPT_RECIP_DIVISION, and the twin before ADR-1442.
        failures = self._edited(MATH, "    return n / d;", "    return n * (1.0f / d);")
        self._detects(failures, "divs() must be DIVS()")

    def test_device_reciprocal_is_detected(self) -> None:
        failures = self._edited(
            MATH, "    return n / d;", "    return n / d + 0.0f * sycl::native::recip(d);"
        )
        self._detects(failures, "divs() must be DIVS()")

    def test_angle_threshold_association_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const float scaled = cos_1deg_sq * o_mag_sq;\n    const float rhs = scaled * t_mag_sq;",
            "    const float mags = o_mag_sq * t_mag_sq;\n    const float rhs = cos_1deg_sq * mags;",
        )
        self._detects(failures, "angle_flag()")

    def test_fp32_gain_is_detected(self) -> None:
        failures = self._edited(MATH, "    if (limit.is_float != 0) {", "    {")
        self._detects(failures, "gain_limited()")

    def test_fp32_csf_constant_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    return times_constant(sycl::fabs(csf), kOneBy30);",
            "    return 0.0333333351f * sycl::fabs(csf);",
        )
        self._detects(failures, "csf_flt()")

    def test_dropped_replay_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    return times_constant_replayed(a, constant);\n}\n\n/* (float)((double)sum",
            "    return pair.hi;\n}\n\n/* (float)((double)sum",
        )
        self._detects(failures, "times_constant() must replay")

    def test_centre_tap_last_is_detected(self) -> None:
        sources = _sources()
        centre = "    sum = add_scaled(sum, sycl::fabs(centre), kOneBy15);\n"
        last = "    sum += n.below_right;\n"
        self.assertIn(centre, sources[MATH])
        self.assertIn(last, sources[MATH])
        sources[MATH] = sources[MATH].replace(centre, "", 1).replace(last, last + centre, 1)
        self._detects(_failures(sources), "thresh_band()")

    def test_group_reduction_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "            vmaf_sycl_fadm::row_item(args, id[0]);",
            "            args.rows[id[0]] = sycl::reduce_over_group(sg, term, sycl::plus<float>());",
        )
        self._detects(failures, "group reduction")
        self._detects(failures, "row kernel")

    def test_host_double_fold_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    float accum = 0.0f;\n    for (unsigned y = 0u; y < count; y++) {",
            "    double accum = 0.0;\n    for (unsigned y = 0u; y < count; y++) {",
        )
        self._detects(failures, "fold_rows()")
        self._detects(failures, "fp64 type outside")

    def test_copied_csf_weights_are_detected(self) -> None:
        failures = self._edited(TWIN, "        adm_csf_rfactor_s(", "        fadm_own_rfactor(")
        self._detects(failures, "adm_csf_rfactor_s(")

    def test_own_pooling_root_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "    return {.numerator = adm_pool_bands_s(",
            "    const float root = std::pow(accum[3], 1.0f / 3.0f);\n"
            "    return {.numerator = root + adm_pool_bands_s(",
        )
        self._detects(failures, "pooling root of its own")

    def test_old_floor_is_detected(self) -> None:
        failures = self._edited(
            TWIN,
            "    const double floor = 1e-10 * (w * h) / (1920.0 * 1080.0);",
            "    const double floor = 1e-2 * (w * h) / (1920.0 * 1080.0);",
        )
        self._detects(failures, "frame floor")

    def test_changed_constant_is_detected(self) -> None:
        failures = self._edited(MATH, "0x111111203e01fcULL", "0x111111203e01fdULL")
        self._detects(failures, "kOneBy30.mant")

    def test_changed_reference_constant_is_detected(self) -> None:
        failures = self._edited(
            CPU, f"#define FLOAT_ONE_BY_30 {ONE_BY_30}", f"#define FLOAT_ONE_BY_30 {ONE_BY_30}f"
        )
        self._detects(failures, CPU)

    def test_changed_reference_division_is_detected(self) -> None:
        failures = self._edited(
            CPU, "#define DIVS(n, d) ((n) / (d))", "#define DIVS(n, d) ((n) * rcp_s(d))"
        )
        self._detects(failures, CPU)


if __name__ == "__main__":
    unittest.main()
