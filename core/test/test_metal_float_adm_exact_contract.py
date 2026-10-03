#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CPU behaviour of float_adm_metal (ADR-1498).

The Metal twin of ``float_adm`` takes the designs of the CUDA (ADR-1420),
SYCL (ADR-1434) and HIP (ADR-1458) twins. This contract holds the source to
them:

* ``init()`` refuses frames below 17x17 with the CPU's
  ``adm_frame_size_check()``, before it reads its state or claims a device
  resource (T-GPU-FLOAT-ADM-TINY-FRAME-FLOOR-2026-10-01);
* the frame numerator and denominator are floored at ``compute_adm()``'s
  ``1e-10 * (w * h) / (1920.0 * 1080.0)``, not at the ``1e-2`` of a branch no
  build defines (T-GPU-FLOAT-ADM-FRAME-SUM-FLOOR-2026-10-01);
* the host takes the CSF weights, the reduced region, the pooling and the
  angle threshold from the CPU's own routines (``adm_float_reference.h``) and
  holds no copy of their constants;
* the kernels run ``metal_float_adm_math.h``, the Metal spelling of the SYCL
  twin's arithmetic: the IEEE fp32 quotient (ADR-1442), the angle threshold
  ``(cos^2 * |o|^2) * |t|^2``, the three fp64 expressions of the reference as
  exact fp32 pairs with an integer replay, the masking threshold as one sum
  per band with the centre tap fifth, and
* every reduction is one fp32 sum of a row per work-item, the rows added on
  the host in fp32, with no per-block or per-simd sum of the terms
  (T-GPU-FLOAT-ADM-CPU-ARITHMETIC-2026-10-01).

No Apple device runs anything here: the contract reads the sources, and each
check has a planted regression below. ``test_metal_float_adm_parity`` measures
the twin on a device.
"""

from __future__ import annotations

import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

HOST = "metal/float_adm_metal.mm"
KERNEL = "metal/float_adm.metal"
MATH = "metal/metal_float_adm_math.h"
CPU = "adm_tools.c"
CPU_FRAME = "adm.c"
MATH_TEST = "../../test/test_metal_float_adm_math.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")

FP64 = re.compile(r"\b(?:long\s+)?double\b|\blong long\b|\d(?:ULL|LL|ull|ll)\b")
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
    "rst = MIN(rst * adm_enhn_gain_limit, t);",
    "rst = MAX(rst * adm_enhn_gain_limit, t);",
    "flt_ptr[dst_offset + j] = FLOAT_ONE_BY_30 * fabsf(dst_val);",
    "sum += FLOAT_ONE_BY_15 * fabsf(src_ptr[j]);",
)
# Where the host's copies of the reference's constants were.
COPIED_HOST = (
    "fadm_dwt_quant_step",
    "fadm_dwt_basis_amp",
    "M_PI",
    "0.0333333351",
    "0.0666666701",
    "0.99969541789740297",
)
# What the host takes from the reference.
HOST_REFERENCE_CALLS = (
    "adm_csf_rfactor_s(",
    "adm_border_s(",
    "adm_pool_bands_s(",
    "adm_decouple_cos_1deg_sq_s(",
)
# The kernels' dispatch and what they replaced.
KERNEL_NAMES = ("float_adm_decouple", "float_adm_terms", "float_adm_rows")
OLD_KERNEL_NAMES = (
    "float_adm_decouple_csf",
    "float_adm_csf_cm",
    "float_adm_csf_r",
    "float_adm_aim_cm",
)
# Constructs a per-block or per-simd reduction needs.
BLOCK_REDUCTION = re.compile(r"simd_sum|threadgroup\s+float|threadgroup_barrier|atomic")
CONSTANT = re.compile(
    r"vmaf_mtl_fadm_const\((0x[0-9a-fA-F]+)u, (0x[0-9a-fA-F]+)u, (0x[0-9a-fA-F]+)u, "
    r"(0x[0-9a-fA-F]+)u, (-?\d+)\)"
)

SIZE_CHECK = 'adm_frame_size_check("float_adm_metal", w, h)'
FRAME_FLOOR = "1e-10 * (w * h) / (1920.0 * 1080.0)"


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE_ROOT / name).read_text(encoding="utf-8")
        for name in (HOST, KERNEL, MATH, CPU, CPU_FRAME, MATH_TEST)
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


def _size_check_failures(host: str) -> list[str]:
    """init() calls the CPU's size check first."""
    init = _function_body(host, "init_fex_metal")
    check = init.find(SIZE_CHECK)
    if check < 0:
        return [f"{HOST}: init_fex_metal() must call `{SIZE_CHECK}`"]
    failures: list[str] = []
    for later in ("fex->priv", "vmaf_metal_context_new(", "newBufferWithLength"):
        found = init.find(later)
        if 0 <= found < check:
            failures.append(f"{HOST}: init_fex_metal() must call the size check before `{later}`")
    return failures


def _floor_failures(sources: dict[str, str], host: str) -> list[str]:
    """The frame sums are floored where and as compute_adm() floors them."""
    failures: list[str] = []
    if FRAME_FLOOR not in _code(sources[CPU_FRAME]):
        failures.append(f"{CPU_FRAME}: compute_adm() no longer floors at `{FRAME_FLOOR}`")
    collect = _function_body(host, "collect_fex_metal")
    if f"const double numden_limit = {FRAME_FLOOR};" not in collect:
        failures.append(f"{HOST}: the frame floor must be compute_adm()'s `{FRAME_FLOOR}`")
    if re.search(r"\b1e-2\b", host):
        failures.append(f"{HOST}: the 1e-2 floor of ADM_OPT_SINGLE_PRECISION is back")
    return failures


def _reference_failures(sources: dict[str, str]) -> list[str]:
    """The reference still holds the lines the twin and its test mirror."""
    failures: list[str] = []
    cpu = SPACE.sub(" ", sources[CPU])
    for line in REFERENCE_LINES:
        if line not in cpu:
            failures.append(f"{CPU}: no longer holds `{line}`")
    test = SPACE.sub(" ", sources[MATH_TEST])
    for name, value in (("REFERENCE_ONE_BY_30", ONE_BY_30), ("REFERENCE_ONE_BY_15", ONE_BY_15)):
        if f"#define {name} {value} " not in test:
            failures.append(f"test_metal_float_adm_math.c: {name} is not the literal {value}")
    return failures


def _host_reference_failures(host: str) -> list[str]:
    """The host calls the CPU's routines and holds no copy of their constants."""
    failures: list[str] = []
    for call in HOST_REFERENCE_CALLS:
        # The pooling is called once per slot family (denominator, adm2, AIM).
        wanted = 3 if call == "adm_pool_bands_s(" else 1
        if host.count(call) < wanted:
            failures.append(f"{HOST}: must call the reference's `{call}` ({wanted}x)")
    for copied in COPIED_HOST:
        if copied in host:
            failures.append(f"{HOST}: holds a copy of the reference (`{copied}`)")
    if "powf(" in host or "accum_out" in host or "wg_count" in host:
        failures.append(f"{HOST}: pools in the host's own way (powf / per-threadgroup sums)")
    collect = _function_body(host, "pool_scale")
    if "vmaf_mtl_fadm_fold_rows(" not in collect or "(float)1e-10" not in collect:
        failures.append(f"{HOST}: pool_scale() must fold the rows in fp32 and keep the 1e-10 floor")
    for name in KERNEL_NAMES:
        if f'@"{name}"' not in host:
            failures.append(f"{HOST}: must build the kernel {name}")
    for name in OLD_KERNEL_NAMES:
        if name in host:
            failures.append(f"{HOST}: still names the old kernel {name}")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    """The kernels take their arithmetic from the header and sum nothing per block."""
    failures: list[str] = []
    if '#include "metal_float_adm_math.h"' not in kernel:
        failures.append(f"{KERNEL}: must include metal_float_adm_math.h")
    if BLOCK_REDUCTION.search(kernel):
        failures.append(f"{KERNEL}: a per-block or per-simd reduction is back")
    for old in ("FADM_ONE_BY_30", "FADM_ONE_BY_15", "FADM_COS_1DEG_SQ", "FADM_EPS", "pow("):
        if old in kernel:
            failures.append(f"{KERNEL}: holds its own arithmetic (`{old}`)")
    if re.search(r"\bgain_limit\b|\*\s*gain\b", kernel):
        failures.append(f"{KERNEL}: the enhancement gain is an fp32 product again")
    for name in KERNEL_NAMES:
        if f"kernel void {name}(" not in kernel:
            failures.append(f"{KERNEL}: no kernel {name}")
    for name in OLD_KERNEL_NAMES:
        if name in kernel:
            failures.append(f"{KERNEL}: still has the old kernel {name}")
    rows = _function_body(kernel, "float_adm_rows")
    if "float inner = 0.0f;" not in rows or "inner += terms[" not in rows:
        failures.append(f"{KERNEL}: float_adm_rows must add a row left to right in one fp32 sum")
    if "for (uint x = 0u; x < a.region_w; ++x)" not in rows:
        failures.append(f"{KERNEL}: float_adm_rows must walk the row's columns in order")
    return failures


def _constant_failures(math: str) -> list[str]:
    """one_by_30 / one_by_15 are the reference's double literals."""
    failures: list[str] = []
    for name, literal in (("one_by_30", ONE_BY_30), ("one_by_15", ONE_BY_15)):
        body = _function_body(math, f"vmaf_mtl_fadm_{name}")
        match = CONSTANT.search(body)
        if not match:
            failures.append(f"{MATH}: {name}() is not vmaf_mtl_fadm_const(hi, lo, mant_hi, ...)")
            continue
        hi, lo, mant_hi, mant_lo, exp = match.groups()
        value = float(literal)
        mant = (int(mant_hi, 16) << 32) | int(mant_lo, 16)
        if mant * 2.0 ** int(exp) != value:
            failures.append(f"{MATH}: {name}() mant / exp are not the double literal {literal}")
        pair = _float_of_bits(int(hi, 16)) + _float_of_bits(int(lo, 16))
        if abs(pair - value) > value * 2.0**-48:
            failures.append(f"{MATH}: {name}() hi + lo is not the double literal {literal}")
    return failures


def _float_of_bits(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def _division_and_angle_failures(math: str) -> list[str]:
    failures: list[str] = []
    divs = _function_body(math, "vmaf_mtl_fadm_divs")
    if "return n / d;" not in divs or RECIPROCAL.search(divs):
        failures.append(f"{MATH}: divs() must be DIVS(): the fp32 quotient n / d (ADR-1442)")
    angle = _function_body(math, "vmaf_mtl_fadm_angle_flag")
    if (
        "const float scaled = cos_1deg_sq * o_mag_sq;" not in angle
        or "const float rhs = scaled * t_mag_sq;" not in angle
    ):
        failures.append(f"{MATH}: angle_flag() must compare with (cos^2 * |o|^2) * |t|^2")
    return failures


def _fp64_failures(math: str) -> list[str]:
    """The three fp64 expressions are pairs with an integer replay, not fp32 constants."""
    failures: list[str] = []
    device = (
        math.split("#if !defined(__METAL_VERSION__)", maxsplit=1)[0]
        if "#if !defined(__METAL_VERSION__)" in math
        else math
    )
    if FP64.search(device):
        failures.append(f"{MATH}: the kernel-safe part must not name an fp64 type or literal")
    gain = _function_body(math, "vmaf_mtl_fadm_gain_limited")
    if "limit.is_float != 0" not in gain or "vmaf_mtl_soft_mul(" not in gain:
        failures.append(f"{MATH}: gain_limited() must replay the fp64 product for other limits")
    flt = _function_body(math, "vmaf_mtl_fadm_csf_flt")
    if "vmaf_mtl_fadm_times_constant(VMAF_MTL_FABS(csf), vmaf_mtl_fadm_one_by_30())" not in flt:
        failures.append(f"{MATH}: csf_flt() must multiply by the double FLOAT_ONE_BY_30")
    for name, replay in (
        ("times_constant", "vmaf_mtl_fadm_times_constant_replayed(a, c)"),
        ("add_scaled", "vmaf_mtl_fadm_add_scaled_replayed(sum, a, c)"),
    ):
        body = _function_body(math, f"vmaf_mtl_fadm_{name}")
        if "vmaf_mtl_fadm_undecided(pair)" not in body or replay not in body:
            failures.append(f"{MATH}: {name}() must replay what the pair does not decide")
    return failures


def _threshold_failures(math: str) -> list[str]:
    body = _function_body(math, "vmaf_mtl_fadm_thresh_band")
    order = (
        "sum += n.above_left;",
        "sum += n.above;",
        "sum += n.above_right;",
        "sum += n.left;",
        "sum = vmaf_mtl_fadm_add_scaled(sum, VMAF_MTL_FABS(centre), vmaf_mtl_fadm_one_by_15());",
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
    threshold = _function_body(math, "vmaf_mtl_fadm_threshold")
    if not re.search(r"accum \+= .*b0.*accum \+= .*b1.*accum \+= .*b2", threshold):
        return [f"{MATH}: threshold() must add the three bands in band order"]
    return []


def _math_failures(sources: dict[str, str]) -> list[str]:
    math = _code(sources[MATH])
    return (
        _constant_failures(math)
        + _division_and_angle_failures(math)
        + _fp64_failures(math)
        + _threshold_failures(math)
    )


def _failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    return (
        _size_check_failures(host)
        + _floor_failures(sources, host)
        + _reference_failures(sources)
        + _host_reference_failures(host)
        + _kernel_failures(_code(sources[KERNEL]))
        + _math_failures(sources)
    )


class MetalFloatAdmExactContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def _detects(self, failures: list[str], text: str) -> None:
        self.assertTrue(any(text in failure for failure in failures), failures)

    def test_live_sources_keep_the_cpu_behaviour(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_missing_size_check_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            f"    const int size_err = {SIZE_CHECK};",
            "    const int size_err = 0;",
        )
        self._detects(failures, "must call `adm_frame_size_check")

    def test_size_check_after_the_device_is_detected(self) -> None:
        sources = _sources()
        check = f"    const int size_err = {SIZE_CHECK};\n    if (size_err != 0) {{ return size_err; }}\n"
        context = "    int err = vmaf_metal_context_new(&s->ctx, 0);\n"
        self.assertIn(check, sources[HOST])
        self.assertIn(context, sources[HOST])
        sources[HOST] = sources[HOST].replace(check, "", 1).replace(context, context + check, 1)
        self._detects(_failures(sources), "before `vmaf_metal_context_new(`")

    def test_old_floor_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            f"    const double numden_limit = {FRAME_FLOOR};",
            "    const double numden_limit = 1e-2 * (double)(w * h) / (1920.0 * 1080.0);",
        )
        self._detects(failures, "the frame floor must be")
        self._detects(failures, "1e-2 floor")

    def test_changed_reference_floor_is_detected(self) -> None:
        failures = self._edited(
            CPU_FRAME,
            f"double numden_limit = {FRAME_FLOOR};",
            "double numden_limit = 1e-12 * (w * h) / (1920.0 * 1080.0);",
        )
        self._detects(failures, CPU_FRAME)

    def test_copied_quant_step_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "static void compute_per_scale_dims",
            "static float fadm_dwt_quant_step(int l, int t, double v, int d) { return 1.0f; }\n"
            "static void compute_per_scale_dims",
        )
        self._detects(failures, "holds a copy of the reference")

    def test_host_without_reference_pooling_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "adm_pool_bands_s(accum + VMAF_MTL_FADM_SLOT_AIM",
            "pool_aim_ourselves(accum + VMAF_MTL_FADM_SLOT_AIM",
        )
        self._detects(failures, "must call the reference's `adm_pool_bands_s(`")

    def test_per_block_sum_in_kernel_is_detected(self) -> None:
        failures = self._edited(
            KERNEL, "    float inner = 0.0f;", "    float inner = simd_sum(0.0f);"
        )
        self._detects(failures, "per-block or per-simd reduction")

    def test_fp32_gain_in_kernel_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    if (id >= VMAF_MTL_FADM_TERM_SLOTS",
            "    float gain_limit = 100.0f;\n    if (id >= VMAF_MTL_FADM_TERM_SLOTS",
        )
        self._detects(failures, "fp32 product again")

    def test_kernel_constant_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "constant float FADM_LO0",
            "constant float FADM_ONE_BY_30 = 0.0333333351f;\nconstant float FADM_LO0",
        )
        self._detects(failures, "holds its own arithmetic")

    def test_reciprocal_division_is_detected(self) -> None:
        failures = self._edited(MATH, "    return n / d;", "    return n * (1.0f / d);")
        self._detects(failures, "must be DIVS()")

    def test_angle_regrouping_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const float rhs = scaled * t_mag_sq;",
            "    const float rhs = cos_1deg_sq * (o_mag_sq * t_mag_sq);",
        )
        self._detects(failures, "(cos^2 * |o|^2) * |t|^2")

    def test_fp32_one_by_30_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "return vmaf_mtl_fadm_times_constant(VMAF_MTL_FABS(csf), vmaf_mtl_fadm_one_by_30());",
            "return 0.0333333351f * VMAF_MTL_FABS(csf);",
        )
        self._detects(failures, "csf_flt() must multiply by the double")

    def test_fp32_one_by_15_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "sum = vmaf_mtl_fadm_add_scaled(sum, VMAF_MTL_FABS(centre), vmaf_mtl_fadm_one_by_15());",
            "sum += 0.0666666701f * VMAF_MTL_FABS(centre);",
        )
        self._detects(failures, "must add the nine taps")

    def test_centre_tap_order_is_detected(self) -> None:
        sources = _sources()
        centre = (
            "    sum = vmaf_mtl_fadm_add_scaled(sum, VMAF_MTL_FABS(centre), "
            "vmaf_mtl_fadm_one_by_15());\n"
        )
        self.assertIn(centre, sources[MATH])
        text = sources[MATH].replace(centre, "", 1)
        sources[MATH] = text.replace(
            "    sum += n.below_right;\n", "    sum += n.below_right;\n" + centre, 1
        )
        self._detects(_failures(sources), "must add the nine taps")

    def test_changed_constant_is_detected(self) -> None:
        failures = self._edited(MATH, "0x203e01fcu, -57", "0x203e01fdu, -57")
        self._detects(failures, "mant / exp are not the double literal")

    def test_fp64_type_in_the_kernel_part_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "VMAF_MTL_FUNC float vmaf_mtl_fadm_divs(float n, float d)",
            "VMAF_MTL_FUNC double vmaf_mtl_fadm_unused(float n) { return (double)n; }\n"
            "VMAF_MTL_FUNC float vmaf_mtl_fadm_divs(float n, float d)",
        )
        self._detects(failures, "must not name an fp64 type")

    def test_changed_reference_constant_is_detected(self) -> None:
        failures = self._edited(
            CPU, "#define FLOAT_ONE_BY_30 0.0333333351", "#define FLOAT_ONE_BY_30 0.0333333333"
        )
        self._detects(failures, "no longer holds")


if __name__ == "__main__":
    unittest.main()
