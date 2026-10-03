#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin float_moment_metal's 16-bit second moments to the CPU's float squares (ADR-1498).

``moment.c::compute_2nd_moment()`` forms each square in ``float``: at 16 bits
that is the integer square rounded to 24 bits. ``float_moment_metal`` added
exact integer squares (T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02). The
10/12/16-bit kernel now adds ``vmaf_mtl_moment_float_square()``
(``metal_float_moment_math.h``): one fp32 product of the sample with itself,
converted to an integer, as ``moment_float_square()`` of the CUDA, SYCL and HIP
twins (ADR-1453, ADR-1449, ADR-1447). The host adds the workgroup sums in
``uint64`` and converts each total once.

The reduction itself (uint64 threadgroup sums, lo/hi pairs) is pinned by
``test_metal_float_moment_contract.py``. Device-free: reads the sources only.
``test_metal_float_moment_math`` holds the term against ``moment.c`` on the
host; ``test_metal_float_moment_parity`` compares the moments on an Apple device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

MATH = "metal/metal_float_moment_math.h"
KERNEL = "metal/float_moment.metal"
HOST = "metal/float_moment_metal.mm"
REFERENCE = "moment.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

TERM_SIGNATURE = "VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_moment_float_square(vmaf_mtl_u32 v)"
TERM = (
    "const float sample = (float)v;",
    "return (vmaf_mtl_u32)(sample * sample);",
)
KERNEL_16_SIGNATURE = "kernel void float_moment_kernel_16bpc("
KERNEL_16 = (
    "const uint rv = (uint)ref_row[(int)gid.x];",
    "const uint dv = (uint)dis_row[(int)gid.x];",
    "my_r2 = (ulong)vmaf_mtl_moment_float_square(rv);",
    "my_d2 = (ulong)vmaf_mtl_moment_float_square(dv);",
)
HOST_SUM_SIGNATURE = (
    "static void accumulate_partials(const FloatMomentStateMetal *s, uint64_t sum[4])"
)
HOST_SUM = tuple(
    f"sum[{k}] += reconstruct_partial(parts[{2 * k}], parts[{2 * k + 1}], i);" for k in range(4)
)
HOST_COLLECT = (
    "uint64_t sum[4] = {0u, 0u, 0u, 0u};",
    "const double ref1 = denom1 > 0.0 ? (double)sum[0] / denom1 : 0.0;",
    "const double ref2 = denom2 > 0.0 ? (double)sum[2] / denom2 : 0.0;",
    "const double dis2 = denom2 > 0.0 ? (double)sum[3] / denom2 : 0.0;",
)
REFERENCE_LINES = (
    "const float term = pic_ * pic_;",
    "cum += (double)term;",
    "cum /= ((double)w * h);",
)


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
        failures.append(f"{MATH}: the term is not one fp32 product of the sample with itself")
    if re.search(r"scaler|double|FMA|fma|\bv \* v\b", term):
        failures.append(f"{MATH}: the term is scaled, fused or squared in another type")
    return failures


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    body = _function_body(code, KERNEL_16_SIGNATURE)
    failures: list[str] = []
    if '#include "metal_float_moment_math.h"' not in code:
        failures.append(f"{KERNEL}: the kernel does not include the term's header")
    if any(piece not in body for piece in KERNEL_16):
        failures.append(f"{KERNEL}: the 16-bit kernel does not add the CPU's float squares")
    if re.search(r"\b(?:rv \* rv|dv \* dv)\b", body):
        failures.append(f"{KERNEL}: the 16-bit kernel adds exact integer squares")
    return failures


def _host_failures(host: str) -> list[str]:
    code = _flat(host)
    accumulate = _function_body(code, HOST_SUM_SIGNATURE)
    failures: list[str] = []
    if any(piece not in accumulate for piece in HOST_SUM):
        failures.append(f"{HOST}: the host does not add the workgroup sums in uint64")
    if "(double)reconstruct_partial" in code or "double sum[4]" in code:
        failures.append(f"{HOST}: the host adds the workgroup sums in floating point")
    if any(piece not in code for piece in HOST_COLLECT):
        failures.append(f"{HOST}: the host does not convert each exact total once")
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


class FloatMomentMetalExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_exact_integer_square_is_detected(self) -> None:
        # The pre-port 16-bit term.
        failures = self._edited(
            KERNEL,
            "        my_r2 = (ulong)vmaf_mtl_moment_float_square(rv);",
            "        my_r2 = (ulong)rv * (ulong)rv;",
        )
        self._assert_detected(failures, "does not add the CPU's float squares")

    def test_integer_square_in_the_header_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    return (vmaf_mtl_u32)(sample * sample);",
            "    return v * v;",
        )
        self._assert_detected(failures, "not one fp32 product")
        self._assert_detected(failures, "squared in another type")

    def test_scaled_sample_is_detected(self) -> None:
        failures = self._edited(
            MATH,
            "    const float sample = (float)v;",
            "    const float sample = (float)v / scaler;",
        )
        self._assert_detected(failures, "not one fp32 product")

    def test_double_host_sum_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "        sum[2] += reconstruct_partial(parts[4], parts[5], i);",
            "        sum[2] += (double)reconstruct_partial(parts[4], parts[5], i);",
        )
        self._assert_detected(failures, "in floating point")

    def test_double_host_totals_are_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    uint64_t sum[4] = {0u, 0u, 0u, 0u};",
            "    double sum[4] = {0.0, 0.0, 0.0, 0.0};",
        )
        self._assert_detected(failures, "in floating point")
        self._assert_detected(failures, "convert each exact total once")

    def test_changed_reference_square_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "            const float term = pic_ * pic_;",
            "            const double term = (double)pic_ * pic_;",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
