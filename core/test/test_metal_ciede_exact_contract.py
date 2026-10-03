#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the ciede_metal design: the CPU's arithmetic in fp32 pairs, the CPU's sum (ADR-1498).

``ciede.c`` computes in double and stores in float, and adds every pixel's
value into one double in raster order. ``ciede_metal`` follows it with the
arithmetic of the SYCL and HIP twins (ADR-1436, ADR-1448):
``feature/ciede_ff_math.h``, the reference statement for statement with every
fp64 value as an fp32 pair and every fp64 math-library call as a pair function
(``feature/ff_math.h``). ``test_sycl_ciede_exact_contract`` pins that header;
``test_metal_ciede_math`` holds it, in its Metal subset and on the Metal
primitives, to the reference's fp64 statements. This contract pins what the
Metal twin adds:

- the primitives (``metal/metal_ciede_math.h``): Metal's fp32 operators under
  the strict kernel flags, ``fma()``, ``exp(log(x) / 3)`` and
  ``exp(0.2 log(x))`` as root estimates (Metal has no ``cbrt()``), and the
  Metal subset of the shared headers;
- the Metal subset of the shared headers: positional pair initializers (the
  error-free product keeps its ``fma``), the C++ library and the fp64 host
  helpers out of a Metal kernel, an address space on every table pointer and
  reference;
- the kernel calls the shared ``pixel()`` with the host's constants and the
  two tables of ``ff_math.h``, uses no fp64 and no math function of its own,
  and stores each value at its raster position with no reduction;
- the host evaluates ``make_constants()`` for the bit depth, reads the whole
  plane back, adds it with ``ciede_frame_sum()`` and forms the score with the
  reference's expression; the twin's option table is the CPU's (none);
- every kernel is built with ``-fno-fast-math -ffp-contract=off``.

Device-free: reads the sources only. The planted regressions are constructs
the twin had before ADR-1498 (an fp32 formula with ``pow()``, threadgroup sums
of the values) and the ways the port could drift. ``test_metal_integer_ciede_parity``
checks the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = ROOT / "core" / "src"

HOST = "feature/metal/integer_ciede_metal.mm"
KERNEL = "feature/metal/integer_ciede.metal"
PRIMITIVES = "feature/metal/metal_ciede_math.h"
PAIR = "feature/ff_pair.h"
FF = "feature/ff_math.h"
MATH = "feature/ciede_ff_math.h"
CPU = "feature/ciede.c"
BUILD = "metal/meson.build"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
FP64 = re.compile(r"\b(?:long\s+)?double\b")
# A math function in the kernel file: the arithmetic is the shared header's.
MATH_CALL = re.compile(
    r"\b(?:pow|powr|cbrt|atan2|sin|cos|exp|exp2|log|log2|sqrt|fabs|fma|rint|ldexp)f?\s*\("
)
DEVICE_REDUCTION = re.compile(
    r"\bsimd_(?:sum|prefix_\w+|shuffle\w*)\s*\(|\bthreadgroup\b|\batomic_\w+|"
    r"\bthreadgroup_barrier\s*\(|\bthreadgroup_position_in_grid\b"
)
FUNCTION_DEFINITION = re.compile(r"\b(?:Ff|float|bool|int|void)\s+\w+\([^;{]*\)\s*\{")
PRIMITIVE_LINES = (
    "#define VMAF_FF_MSL_SUBSET 1",
    '#include "metal_portable.h"',
    "#define VMAF_FF_INLINE VMAF_MTL_FUNC",
    "#define VMAF_FF_PROGRAM_CONST VMAF_MTL_CONSTANT",
    "#define VMAF_FF_TABLE_SPACE constant",
    "#define VMAF_FF_REF_SPACE thread",
    "#define VMAF_FF_FMA(a, b, c) VMAF_MTL_FMA(a, b, c)",
    "#define VMAF_FF_FABS(x) VMAF_MTL_FABS(x)",
    "#define VMAF_FF_SQRT(x) VMAF_MTL_SQRT(x)",
    "#define VMAF_FF_RINT(x) metal::rint(x)",
    "#define VMAF_FF_CBRT(x) metal::exp(metal::log(x) / 3.0f)",
    "#define VMAF_FF_ROOT5(x) metal::exp(0.2f * metal::log(x))",
    "#define VMAF_FF_LDEXP(x, k) metal::ldexp((x), (k))",
    # The host stand-ins: the same functions in C++.
    "#define VMAF_FF_FMA(a, b, c) std::fma((a), (b), (c))",
    "#define VMAF_FF_SQRT(x) std::sqrt(x)",
    "#define VMAF_FF_CBRT(x) std::exp(std::log(x) / 3.0f)",
    "#define VMAF_FF_ROOT5(x) std::exp(0.2f * std::log(x))",
    '#include "../ff_pair.h"',
    "namespace vmaf_ffm_base = vmaf_ff_pair;",
    '#include "../ciede_ff_math.h"',
    "namespace vmaf_metal_ciede = vmaf_ciede_ff;",
    "static_assert(sizeof(vmaf_metal_ciede::Constants) == 79u * sizeof(float),",
)
# The Metal subset of the shared headers, in the source as written.
SUBSET_PIECES = (
    (PAIR, "    return {product, VMAF_FF_FMA(a, b, -product)};"),
    (PAIR, "    return {sum, a_error + b_error};"),
    (FF, "#if !defined(__METAL_VERSION__)\n#include <cstddef>\n#include <cstdint>\n#endif"),
    (FF, "#define VMAF_FF_CONSTANT VMAF_FF_PROGRAM_CONST"),
    (FF, "#define VMAF_FF_PAIR_INIT(high, low) {high, low}"),
    (FF, "    const VMAF_FF_TABLE_SPACE float *sin_cos; /* kSinCosTable */"),
    (FF, "VMAF_FF_INLINE SinCos sin_cos(Ff x, const VMAF_FF_TABLE_SPACE float *table)"),
    (FF, "VMAF_FF_INLINE Ff atan2(float y, float x, const VMAF_FF_TABLE_SPACE float *table)"),
    (FF, "    const Ff sin_k = {table[index + 0u], table[index + 1u]};"),
    (FF, "VMAF_FF_CONSTANT float kAtanTable[2 * 17] = {"),
    (
        MATH,
        "#if !defined(__METAL_VERSION__)\n#include <cstddef>\n#include <cstdint>\n"
        "#include <numbers>\n#endif",
    ),
    (
        MATH,
        "#define Constants VMAF_FF_REF_SPACE Constants\n#define Tables VMAF_FF_REF_SPACE Tables",
    ),
    (MATH, "#undef Constants\n#undef Tables"),
    (MATH, "    const VMAF_FF_TABLE_SPACE float *table = tables.sin_cos;"),
    (MATH, "#if !defined(__METAL_VERSION__)\n\n/* An fp64 value as a pair"),
)
KERNEL_INCLUDE = '#include "metal_ciede_math.h"'
KERNEL_PIXEL = "return vmaf_metal_ciede::pixel(ref, dis, k, tables);"
KERNEL_TABLES = (
    "const vmaf_metal_ffm::Tables tables = {vmaf_metal_ffm::kAtanTable, "
    "vmaf_metal_ffm::kSinCosTable};"
)
KERNEL_CONSTANTS = "constant vmaf_metal_ciede::Constants &constants [[buffer(8)]],"
TERM_STORE = (
    "const uint i = gid.y * dim.x + gid.x; terms[i] = ciede_pixel(ciede_samples(ref_y, ref_u, "
    "ref_v, i), ciede_samples(dis_y, dis_u, dis_v, i), constants);"
)
HOST_INCLUDES = ('#include "ciede_frame_sum.h"', '#include "metal_ciede_math.h"')
HOST_CONSTANTS = "s->constants = vmaf_metal_ciede::make_constants(bpc);"
HOST_SET_CONSTANTS = "[enc setBytes:&s->constants length:sizeof(s->constants) atIndex:8];"
HOST_READBACK = "vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx, (size_t)w * h * sizeof(float));"
HOST_CALL = "const double de00_sum = ciede_frame_sum(terms, (size_t)s->frame_w * s->frame_h);"
HOST_SCORE = "const double score = 45. - 20. * log10(de00_sum / (s->frame_w * s->frame_h));"
HOST_OPTIONS = "static const VmafOption options[] = {{0}};"
CPU_SCORE = "const double score = 45. - 20. * log10(de00_sum / (ref_pic->w[0] * ref_pic->h[0]));"
CPU_UPSCALE = (
    "out_buf[j] = in_buf[(j / ((p && ss_hor) ? 2 : 1))];",
    "in_buf += ((p && ss_ver) ? i % 2 : 1) * in->stride[p];",
)
HOST_UPSCALE = (
    "unsigned in_x = ss_hor ? (j >> 1) : j;",
    "unsigned in_row_step = ss_ver ? (i & 1u) : 1u;",
)
STRICT_FP = "metal_shader_strict_fp_args = ['-fno-fast-math', '-ffp-contract=off']"


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    return {
        name: (SRC_ROOT / name).read_text(encoding="utf-8")
        for name in (HOST, KERNEL, PRIMITIVES, PAIR, FF, MATH, CPU, BUILD)
    }


def _primitive_failures(sources: dict[str, str]) -> list[str]:
    primitives = sources[PRIMITIVES]
    failures = [
        f"{PRIMITIVES}: the Metal primitive is not `{line}`"
        for line in PRIMITIVE_LINES
        if line not in primitives
    ]
    code = _flat(primitives)
    if FP64.search(code):
        failures.append(f"{PRIMITIVES}: an fp64 type among the primitives")
    if FUNCTION_DEFINITION.search(code):
        failures.append(f"{PRIMITIVES}: a function definition next to the shared arithmetic")
    if re.search(r"\bmetal::(?:pow|powr|cbrt)\s*\(", code):
        failures.append(f"{PRIMITIVES}: a power function as a root estimate")
    return failures


def _subset_failures(sources: dict[str, str]) -> list[str]:
    failures = [
        f"{name}: the Metal subset no longer holds `{piece.splitlines()[-1].strip()}`"
        for name, piece in SUBSET_PIECES
        if piece not in sources[name]
    ]
    # Inside a Metal kernel no fp64 is left: make_pair() and make_constants()
    # sit in the !__METAL_VERSION__ block, which runs to the end of the file.
    math = sources[MATH]
    host_block = math.find("#if !defined(__METAL_VERSION__)\n\n/* An fp64 value")
    device = _flat(math[:host_block]) if host_block >= 0 else _flat(math)
    if FP64.search(device):
        failures.append(f"{MATH}: fp64 type outside the host-only block")
    return failures


def _has(text: str, pieces: tuple[str, ...]) -> bool:
    return all(piece in text for piece in pieces)


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = sources[KERNEL]
    code = _flat(kernel)
    checks = (
        (KERNEL_INCLUDE in kernel, "the kernels no longer compile the shared arithmetic"),
        (not FP64.search(code), "fp64 in the kernels (Metal has none)"),
        (
            not MATH_CALL.search(code),
            "a math function of the kernel's own replaces the shared one",
        ),
        (
            _has(code, (KERNEL_PIXEL, KERNEL_TABLES)),
            "the pixel is not the shared pixel() on ff_math.h's tables",
        ),
        (
            _has(code, (KERNEL_CONSTANTS, "const vmaf_metal_ciede::Constants k = constants;")),
            "the constants are not the host's make_constants()",
        ),
        (TERM_STORE in code, "a pixel's value is not stored at its raster position"),
        (not DEVICE_REDUCTION.search(code), "the per-pixel values are reduced on the device"),
    )
    return [f"{KERNEL}: {message}" for ok, message in checks if not ok]


def _host_failures(sources: dict[str, str]) -> list[str]:
    host = sources[HOST]
    code = _flat(host)
    cpu = _flat(sources[CPU])
    checks = (
        (
            _has(host, HOST_INCLUDES),
            "the host no longer takes ciede_frame_sum.h and metal_ciede_math.h",
        ),
        (
            _has(code, (HOST_CONSTANTS, HOST_SET_CONSTANTS)),
            "the kernel's constants are not make_constants() of the bit depth",
        ),
        (HOST_READBACK in code, "the readback is no longer one float per pixel"),
        (HOST_CALL in code, "collect no longer adds the whole plane with ciede_frame_sum()"),
        (
            CPU_SCORE in cpu and HOST_SCORE in code,
            "the score is not the reference's expression",
        ),
        (not re.search(r"\+=\s*\(double\)", code), "the host adds device partials of its own"),
        (
            _has(cpu, CPU_UPSCALE) and _has(code, HOST_UPSCALE),
            "the chroma upscale is not ciede.c's scale_chroma_planes()",
        ),
        (
            HOST_OPTIONS in code and not re.search(r"\.options\s*=", cpu),
            "the option table is not the CPU extractor's (none)",
        ),
    )
    return [f"{HOST}: {message}" for ok, message in checks if not ok]


def _build_failures(sources: dict[str, str]) -> list[str]:
    build = sources[BUILD]
    failures: list[str] = []
    if STRICT_FP not in build:
        failures.append(f"{BUILD}: the Metal kernels are no longer built without fast math")
    if "'integer_ciede'," not in build:
        failures.append(f"{BUILD}: integer_ciede.metal is no longer built")
    return failures


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _primitive_failures(sources)
        + _subset_failures(sources)
        + _kernel_failures(sources)
        + _host_failures(sources)
        + _build_failures(sources)
    )


class CiedeMetalExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _detects(self, failures: list[str], text: str) -> None:
        self.assertTrue(any(text in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_fp32_formula_in_the_kernel_is_detected(self) -> None:
        # The pre-ADR-1498 kernel: ciede2000 in fp32 with pow().
        sources = _sources()
        sources[KERNEL] += "\ninline float ciede_pow_pos_2_4(float x) { return pow(x, 2.4f); }\n"
        self._detects(_contract_failures(sources), "math function of the kernel's own")

    def test_pixel_of_its_own_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    return vmaf_metal_ciede::pixel(ref, dis, k, tables);",
            "    return ciede2000_dev(ref, dis);",
        )
        self._detects(failures, "shared pixel()")

    def test_fp64_in_the_kernel_is_detected(self) -> None:
        sources = _sources()
        sources[KERNEL] += "\ninline float lab_map(double t) { return (float)t; }\n"
        self._detects(_contract_failures(sources), "fp64 in the kernels")

    def test_block_sum_store_is_detected(self) -> None:
        # The pre-ADR-1498 partials: one float per threadgroup.
        failures = self._edited(
            KERNEL,
            "    terms[i] = ciede_pixel(",
            "    terms[bid.y * grid_groups.x + bid.x] += ciede_pixel(",
        )
        self._detects(failures, "raster position")

    def test_device_reduction_is_detected(self) -> None:
        # The pre-ADR-1498 ciede_reduce_and_store().
        sources = _sources()
        reduction = (
            "inline float r(float v, threadgroup float *p) { p[0] = simd_sum(v); return p[0]; }"
        )
        sources[KERNEL] += "\n" + reduction + "\n"
        self._detects(_contract_failures(sources), "reduced on the device")

    def test_powf_as_root_estimate_is_detected(self) -> None:
        failures = self._edited(
            PRIMITIVES,
            "#define VMAF_FF_ROOT5(x) metal::exp(0.2f * metal::log(x))",
            "#define VMAF_FF_ROOT5(x) metal::powr((x), 0.2f)",
        )
        self._detects(failures, "Metal primitive is not")
        self._detects(failures, "power function as a root estimate")

    def test_two_prod_without_fma_is_detected(self) -> None:
        failures = self._edited(
            PAIR,
            "    return {product, VMAF_FF_FMA(a, b, -product)};",
            "    return {product, a * b - product};",
        )
        self._detects(failures, "Metal subset no longer holds")

    def test_designated_initializer_in_the_subset_is_detected(self) -> None:
        failures = self._edited(
            FF,
            "    const Ff sin_k = {table[index + 0u], table[index + 1u]};",
            "    const Ff sin_k = {.hi = table[index + 0u], .lo = table[index + 1u]};",
        )
        self._detects(failures, "Metal subset no longer holds")

    def test_table_pointer_without_address_space_is_detected(self) -> None:
        failures = self._edited(
            FF,
            "VMAF_FF_INLINE SinCos sin_cos(Ff x, const VMAF_FF_TABLE_SPACE float *table)",
            "VMAF_FF_INLINE SinCos sin_cos(Ff x, const float *table)",
        )
        self._detects(failures, "Metal subset no longer holds")

    def test_host_helpers_in_a_metal_kernel_are_detected(self) -> None:
        failures = self._edited(
            MATH, "#if !defined(__METAL_VERSION__)\n\n/* An fp64 value", "\n/* An fp64 value"
        )
        self._detects(failures, "fp64 type outside the host-only block")

    def test_constants_of_the_kernels_own_are_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    s->constants   = vmaf_metal_ciede::make_constants(bpc);",
            "    s->constants   = vmaf_metal_ciede::make_constants(8u);",
        )
        self._detects(failures, "make_constants() of the bit depth")

    def test_host_sum_of_its_own_is_detected(self) -> None:
        # The pre-ADR-1498 collect_fex_metal().
        failures = self._edited(
            HOST,
            HOST_CALL,
            "double de00_sum = 0.0;\n    for (size_t i = 0; i < s->frame_w; ++i)\n"
            "        de00_sum += (double)terms[i];",
        )
        self._detects(failures, "ciede_frame_sum()")
        self._detects(failures, "partials of its own")

    def test_other_score_expression_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            HOST_SCORE,
            "const double score = 45.0 - 20.0 * log10(de00_sum / "
            "((double)s->frame_w * s->frame_h));",
        )
        self._detects(failures, "reference's expression")

    def test_other_chroma_upscale_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "unsigned in_row_step = ss_ver ? (i & 1u) : 1u;",
            "unsigned in_row_step = ss_hor ? (i & 1u) : 1u;",
        )
        self._detects(failures, "scale_chroma_planes()")

    def test_option_of_the_twins_own_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            HOST_OPTIONS,
            'static const VmafOption options[] = {{.name = "enable_lcs"}, {0}};',
        )
        self._detects(failures, "option table")

    def test_fast_math_in_the_kernel_build_is_detected(self) -> None:
        failures = self._edited(
            BUILD, STRICT_FP, "metal_shader_strict_fp_args = ['-ffp-contract=off']"
        )
        self._detects(failures, "without fast math")


if __name__ == "__main__":
    unittest.main()
