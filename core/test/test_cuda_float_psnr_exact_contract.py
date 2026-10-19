#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin float_psnr_cuda's sums to exact integers (ADR-1455).

``float_psnr.c`` squares each sample difference in ``float`` and adds the
squares in ``double``; that sum is exact while it is below 2^53 units of
1 / scaler^2, so the CPU's noise is the exact sum of its terms.
``float_psnr_cuda`` returns the same score exactly when its own sums are
exact. It added each 16x16 block in fp32, which is exact at 8 bits and rounds
at 10, 12 and 16 bits once the differences in a block are large (up to 1.2e-7
dB on full-range content).

The kernel now squares the raw sample difference in ``float`` (the CPU's
term times ``scaler``^2), converts it to an integer, and the warps and the
blocks add ``uint64`` values. Each block is a segment of one row, and the
host adds each row's blocks exactly and the rows into a double in the CPU's
order (``feature/float_psnr_rows.h``, ADR-1499), then divides by
``scaler``^2 and by the pixel count.

Device-free: reads the sources only. ``test_cuda_float_psnr_parity`` compares
the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

KERNEL = "cuda/float_psnr/float_psnr_score.cu"
HOST = "cuda/float_psnr_cuda.c"
GEOMETRY = "cuda/float_psnr_cuda.h"
REFERENCE = "float_psnr.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A floating-point accumulator anywhere in the reduction.
FLOAT_SUM = re.compile(r"\b(?:float|double) (?:my_noise|mine|v|w|total|s_warps)\b")

TERM = (
    "const float diff = (float)(ref - dis);",
    "return (unsigned long long)__fmul_rn(diff, diff);",
)
BLOCK_SUM = (
    "__shared__ unsigned long long s_warps[FPSNR_WARPS];",
    "unsigned long long v = mine;",
    "v += __shfl_down_sync(0xffffffffu, v, off);",
    "unsigned long long total = 0ull;",
    "total += warp_sum;",
)
BLOCK = (
    "unsigned long long my_noise = 0ull;",
    "my_noise = fpsnr_square(rv, dv);",
    "const unsigned long long total = fpsnr_block_sum(my_noise);",
    "VMAF_CUDA_DPTR(unsigned long long, partials.data)[block_idx] = total;",
)
HOST_SUM = (
    "const unsigned per_row = (s->frame_w + FPSNR_BX - 1u) / FPSNR_BX;",
    "const double total = vmaf_float_psnr_row_noise(s->rb.host_pinned, s->frame_h, per_row);",
    "const double scaler = (double)(1u << (s->bpc - 8u));",
    "return (total / (scaler * scaler)) / n_pix;",
)
READBACK = "s->partials_bytes = (size_t)s->wg_count * sizeof(uint64_t);"
# The CPU's term and its sum, which the kernel and the host mirror.
REFERENCE_LINES = (
    "float diff = ref[j] - dis[j];",
    "accum += (double)(diff * diff);",
    "noise_ /= (w * h);",
)


def _flat(source: str) -> str:
    """Code without comments, every run of whitespace collapsed."""
    return " ".join(COMMENT.sub(" ", source).split())


def _sources() -> dict[str, str]:
    return {
        name: (FEATURE / name).read_text(encoding="utf-8")
        for name in (KERNEL, HOST, REFERENCE, GEOMETRY)
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


def _kernel_failures(kernel: str) -> list[str]:
    code = _flat(kernel)
    failures: list[str] = []
    term = _function_body(code, "unsigned long long fpsnr_square(int ref, int dis)")
    if any(piece not in term for piece in TERM):
        failures.append(f"{KERNEL}: the term is not the CPU's float product of the raw difference")
    if "scaler" in term or "double" in term:
        failures.append(f"{KERNEL}: the term is scaled or squared in another type")
    block_sum = _function_body(code, "unsigned long long fpsnr_block_sum(unsigned long long mine)")
    block = _function_body(code, "void fpsnr_block(")
    if any(piece not in block_sum for piece in BLOCK_SUM) or any(p not in block for p in BLOCK):
        failures.append(f"{KERNEL}: a block sum is not an integer sum")
    if FLOAT_SUM.search(block_sum) or FLOAT_SUM.search(block):
        failures.append(f"{KERNEL}: a block sum is added in floating point")
    return failures


def _host_failures(host: str) -> list[str]:
    code = _flat(host)
    failures: list[str] = []
    noise = _function_body(code, "static double float_psnr_noise(const FloatPsnrStateCuda *s)")
    if any(piece not in noise for piece in HOST_SUM):
        failures.append(
            f"{HOST}: the host does not add the rows' exact sums as the CPU does (ADR-1499)"
        )
    if READBACK not in code:
        failures.append(f"{HOST}: the readback is not one uint64 per block")
    return failures


def _reference_failures(reference: str) -> list[str]:
    code = _flat(reference)
    return [
        f"{REFERENCE} no longer holds `{line}`; the twin mirrors it"
        for line in REFERENCE_LINES
        if line not in code
    ]


def _geometry_failures(geometry: str, kernel: str) -> list[str]:
    code = _flat(geometry)
    rows = re.search(r"#define FPSNR_BX 256u\b", code) and re.search(r"#define FPSNR_BY 1u\b", code)
    if rows and '#include "cuda/float_psnr_cuda.h"' in kernel:
        return []
    return [f"{GEOMETRY}: a block is not a segment of one row (ADR-1499)"]


def _contract_failures(sources: dict[str, str]) -> list[str]:
    return (
        _geometry_failures(sources[GEOMETRY], sources[KERNEL])
        + _kernel_failures(sources[KERNEL])
        + _host_failures(sources[HOST])
        + _reference_failures(sources[REFERENCE])
    )


class FloatPsnrCudaExactContract(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _contract_failures(sources)

    def _assert_detected(self, failures: list[str], needle: str) -> None:
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_sources_satisfy_the_contract(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_float_warp_sum_is_detected(self) -> None:
        # The pre-ADR-1455 reduction.
        failures = self._edited(
            KERNEL, "    unsigned long long v = mine;", "    float v = (float)mine;"
        )
        self._assert_detected(failures, "not an integer sum")
        self._assert_detected(failures, "added in floating point")

    def test_float_block_total_is_detected(self) -> None:
        failures = self._edited(
            KERNEL, "    unsigned long long total = 0ull;", "    float total = 0.0f;"
        )
        self._assert_detected(failures, "added in floating point")

    def test_frame_total_instead_of_rows_is_detected(self) -> None:
        # The pre-ADR-1499 host: every block added into one exact total.
        failures = self._edited(
            HOST,
            "vmaf_float_psnr_row_noise(s->rb.host_pinned, s->frame_h, per_row);",
            "vmaf_float_psnr_row_noise(s->rb.host_pinned, 1u, s->wg_count);",
        )
        self._assert_detected(failures, "rows' exact sums")

    def test_scaled_difference_is_detected(self) -> None:
        # The square of a scaled difference is the same value; the pin is
        # that the term stays an integer in units of 1 / scaler^2.
        failures = self._edited(
            KERNEL,
            "    const float diff = (float)(ref - dis);",
            "    const float diff = (float)(ref - dis) * inv_scaler;",
        )
        self._assert_detected(failures, "float product of the raw difference")
        self._assert_detected(failures, "scaled or squared in another type")

    def test_integer_square_is_detected(self) -> None:
        # At 16 bits the CPU's float square is the integer square rounded to
        # 24 bits; an exact integer square is another number.
        failures = self._edited(
            KERNEL,
            "    return (unsigned long long)__fmul_rn(diff, diff);",
            "    return (unsigned long long)((long long)(ref - dis) * (ref - dis));",
        )
        self._assert_detected(failures, "float product of the raw difference")

    def test_host_division_in_another_order_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "    return (total / (scaler * scaler)) / n_pix;",
            "    return total / (scaler * scaler * n_pix);",
        )
        self._assert_detected(failures, "rows' exact sums")

    def test_square_blocks_are_detected(self) -> None:
        # The pre-ADR-1499 geometry: 16x16 blocks mix rows.
        failures = self._edited(GEOMETRY, "#define FPSNR_BY 1u", "#define FPSNR_BY 16u")
        self._assert_detected(failures, "segment of one row")

    def test_changed_reference_term_is_detected(self) -> None:
        failures = self._edited(
            REFERENCE,
            "        accum += (double)(diff * diff);",
            "        accum += (double)diff * diff;",
        )
        self._assert_detected(failures, "the twin mirrors it")


if __name__ == "__main__":
    unittest.main()
