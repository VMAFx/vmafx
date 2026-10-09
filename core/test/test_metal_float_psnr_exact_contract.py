#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin float_psnr_metal's sums to exact integers in the CPU's row order (ADR-1498, ADR-1499).

``float_psnr.c`` squares each sample difference in ``float`` and adds the
squares in ``double``; that sum is exact while it is below 2^53 units of
1 / scaler^2, so the CPU's noise is the exact sum of its terms.
``float_psnr_metal`` added each 16x16 threadgroup with ``simd_sum()`` in fp32,
which is exact at 8 bits only (T-METAL-FLOAT-PSNR-FP32-BLOCK-SUMS-2026-10-02).

The kernel now forms the raw sample difference's float square as an integer
(``vmaf_mtl_fpsnr_term()`` in ``metal_float_psnr_math.h``, the CPU's term
times scaler^2), each threadgroup adds its terms as ``ulong`` in threadgroup
memory and stores one ``ulong``, and the host divides by scaler^2 and the
pixel count, as ``float_psnr_cuda.c::float_psnr_noise()`` does.

Since ADR-1499 a threadgroup covers 256 pixels of one row, and the host adds
each row's segments exactly and the rows into a double in order
(``vmaf_float_psnr_row_noise()`` in ``float_psnr_rows.h``), as ``float_psnr.c``
adds its rows: past 2^53 units those adds round, and a frame total rounded
once is another number.

Device-free: reads the sources only. ``test_metal_float_psnr_math`` holds the
term against the CPU on the host; ``test_metal_float_psnr_parity`` compares the
scores on an Apple device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

MATH = "metal/metal_float_psnr_math.h"
KERNEL = "metal/float_psnr.metal"
HOST = "metal/float_psnr_metal.mm"
REFERENCE = "float_psnr.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A floating-point accumulator or fp32 SIMD reduction in the kernel.
FLOAT_SUM = re.compile(r"\b(?:threadgroup float|float my_noise|float group_sum|float lane_sum)\b")

TERM_SIGNATURE = (
    "VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_fpsnr_term(vmaf_mtl_i32 ref, vmaf_mtl_i32 dis)"
)
TERM = (
    "const float diff = (float)(ref - dis);",
    "return (vmaf_mtl_u32)(diff * diff);",
)
GROUP_SUM_SIGNATURE = "inline void fpsnr_store_group_sum("
GROUP_SUM = (
    "ulong mine, uint lid, uint group, threadgroup ulong *scratch, device ulong *partials",
    "scratch[lid] = mine;",
    "threadgroup_barrier(mem_flags::mem_threadgroup);",
    "ulong total = 0ul;",
    "total += scratch[i];",
    "partials[group] = total;",
)
KERNEL_PIECES = (
    '#include "metal_float_psnr_math.h"',
    "device ulong *partials [[buffer(2)]]",
    "ulong my_noise = 0ul;",
    "my_noise = (ulong)vmaf_mtl_fpsnr_term(r, d);",
    "threadgroup ulong scratch[FPSNR_THREADS_PER_GROUP];",
    "fpsnr_store_group_sum(my_noise, lid, bid.y * grid_groups.x + bid.x, scratch, partials);",
)
HOST_SUM = (
    "const uint64_t *partials = (const uint64_t *)s->rb.host_view;",
    "const double total = vmaf_float_psnr_row_noise(partials, s->frame_h, s->per_row);",
    "const double scaler = (double)(1u << (s->bpc - 8u));",
    "return (total / (scaler * scaler)) / n_pix;",
)
HOST_PIECES = (
    "s->partials_count * sizeof(uint64_t));",
    "const double mse = float_psnr_noise(s);",
    '#include "float_psnr_rows.h"',
)
# ADR-1499: one threadgroup per 256-pixel segment of one row.
HOST_ROWS = (
    "#define FPSNR_SEGMENT 256u",
    "s->per_row = (w + FPSNR_SEGMENT - 1u) / FPSNR_SEGMENT;",
    "s->partials_count = (size_t)s->per_row * h;",
    "MTLSize tg = MTLSizeMake(FPSNR_SEGMENT, 1, 1);",
    "MTLSize grid = MTLSizeMake(s->per_row, s->frame_h, 1);",
)
# The CPU's term and its sum, which the header and the host mirror.
REFERENCE_LINES = (
    "float diff = ref[j] - dis[j];",
    "accum += (double)(diff * diff);",
    "noise_ /= (w * h);",
)


PIECE_COUNT = 2  # times each kernel piece appears (the 8 and the 16 bit kernel)


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE / name).read_text(encoding="utf-8")
        for name in (MATH, KERNEL, HOST, REFERENCE)
    }


def _function_body(code: str, signature: str) -> str:
    """Flattened text of the definition that starts with `signature` (brace-matched), or empty."""
    start = code.find(signature)
    if start < 0:
        return ""
    depth = 0
    for index in range(code.index("{", start), len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return code[start : index + 1]
    return ""


def _math_failures(math: str) -> list[str]:
    term = _function_body(_flat(math), TERM_SIGNATURE)
    failures: list[str] = []
    if any(piece not in term for piece in TERM):
        failures.append(f"{MATH}: the term is not the CPU's float product of the raw difference")
    if re.search(r"scaler|double|FMA|fma", term):
        failures.append(f"{MATH}: the term is scaled, fused or squared in another type")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    failures: list[str] = []
    group_sum = _function_body(code, GROUP_SUM_SIGNATURE)
    if any(piece not in group_sum for piece in GROUP_SUM):
        failures.append(f"{KERNEL}: a threadgroup sum is not an integer sum")
    if any(code.count(piece) != PIECE_COUNT for piece in KERNEL_PIECES[1:]):
        failures.append(f"{KERNEL}: a kernel does not add the header's integer terms")
    if KERNEL_PIECES[0] not in code:
        failures.append(f"{KERNEL}: the kernel does not include the term's header")
    if FLOAT_SUM.search(code) or "simd_sum" in code:
        failures.append(f"{KERNEL}: a threadgroup sum is added in floating point")
    if "scaler" in code or "float r" in code or "float d" in code:
        failures.append(f"{KERNEL}: a sample is converted to float in the kernel")
    return failures


def _host_failures(host: str) -> list[str]:
    code = _flat(host)
    failures: list[str] = []
    noise = _function_body(code, "static double float_psnr_noise(const FloatPsnrStateMetal *s)")
    if any(piece not in noise for piece in HOST_SUM):
        failures.append(
            f"{HOST}: the host does not add the integer sums and divide the exact total"
        )
    if re.search(r"\bdouble mse_sum\b|\buint64_t total\b", noise) or "const float *parts" in code:
        failures.append(f"{HOST}: the host does not add the rows in the CPU's order")
    if any(piece not in code for piece in HOST_ROWS):
        failures.append(f"{HOST}: a threadgroup is not one 256-pixel segment of one row")
    if any(piece not in code for piece in HOST_PIECES):
        failures.append(f"{HOST}: the readback is not one uint64 per threadgroup")
    return failures


def _reference_failures(reference: str) -> list[str]:
    code = _flat(reference)
    return [
        f"{REFERENCE} no longer holds `{line}`; the twin mirrors it"
        for line in REFERENCE_LINES
        if line not in code
    ]


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _math_failures(sources[MATH])
        + _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST])
        + _reference_failures(sources[REFERENCE])
    )


class FloatPsnrMetalExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_fp32_simd_sum_is_detected(self) -> None:
        # The pre-port reduction: per-thread float, simd_sum, float group sum.
        failures = self._edited(
            KERNEL,
            "        partials[group] = total;",
            "        partials[group] = (ulong)simd_sum((float)mine);",
        )
        self._assert_detected(failures, "added in floating point")

    def test_float_group_total_is_detected(self) -> None:
        failures = self._edited(
            KERNEL, "        ulong total = 0ul;", "        float group_sum = 0.0f;"
        )
        self._assert_detected(failures, "not an integer sum")
        self._assert_detected(failures, "added in floating point")

    def test_float_partials_buffer_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "device ulong *partials [[buffer(2)]]",
            "device float *partials [[buffer(2)]]",
        )
        self._assert_detected(failures, "does not add the header's integer terms")

    def test_frame_total_rounded_once_is_detected(self) -> None:
        # The ADR-1455 form: one exact frame total, rounded once.
        failures = self._edited(
            HOST,
            "    const double total = vmaf_float_psnr_row_noise(partials, s->frame_h, s->per_row);",
            "    uint64_t total = 0u;\n    for (size_t i = 0; i < s->partials_count; i++) {\n"
            "        total += partials[i];\n    }",
        )
        self._assert_detected(failures, "not add the rows in the CPU's order")

    def test_square_threadgroup_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    const MTLSize tg   = MTLSizeMake(FPSNR_SEGMENT, 1, 1);",
            "    const MTLSize tg   = MTLSizeMake(16, 16, 1);",
        )
        self._assert_detected(failures, "not one 256-pixel segment of one row")

    def test_float_readback_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "s->partials_count * sizeof(uint64_t));",
            "s->partials_count * sizeof(float));",
        )
        self._assert_detected(failures, "not one uint64 per threadgroup")

    def test_scaled_difference_is_detected(self) -> None:
        # The square of a scaled difference is the same value; the pin is that
        # the term stays an integer in units of 1 / scaler^2.
        failures = self._edited(
            MATH,
            "    const float diff = (float)(ref - dis);",
            "    const float diff = (float)(ref - dis) / scaler;",
        )
        self._assert_detected(failures, "float product of the raw difference")
        self._assert_detected(failures, "scaled, fused or squared in another type")

    def test_integer_square_is_detected(self) -> None:
        # At 16 bits the CPU's float square is the integer square rounded to
        # 24 bits; an exact integer square is another number.
        failures = self._edited(
            MATH,
            "    return (vmaf_mtl_u32)(diff * diff);",
            "    return (vmaf_mtl_u32)((ref - dis) * (ref - dis));",
        )
        self._assert_detected(failures, "float product of the raw difference")

    def test_kernel_side_conversion_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "        const int r = (int)ref_row[gid.x];",
            "        const float r = (float)ref_row[gid.x] / scaler;",
        )
        self._assert_detected(failures, "converted to float in the kernel")

    def test_host_division_in_another_order_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    return (total / (scaler * scaler)) / n_pix;",
            "    return total / (scaler * scaler * n_pix);",
        )
        self._assert_detected(failures, "divide the exact total")

    def test_changed_reference_term_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "        accum += (double)(diff * diff);",
            "        accum += (double)diff * diff;",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
