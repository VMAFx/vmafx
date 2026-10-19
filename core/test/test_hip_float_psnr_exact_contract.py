#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin float_psnr_hip's sums to exact integers (ADR-1440).

``float_psnr.c`` squares each sample difference in ``float`` and adds the
squares in ``double``; that sum never rounds, so the CPU's noise is the exact
sum of its terms. ``float_psnr_hip`` returns the same score exactly when its
own sums are exact. It added each 16x16 block in fp32, which is exact at 8
bits and rounds at 10, 12 and 16 bits once the differences in a block are
large (up to 7.6e-8 dB on full-range noise).

The kernel now squares the sample difference in ``float`` (the CPU's term
times ``scaler``^2), converts it to ``uint32`` and reduces integers: one sum
per block up to 12 bits, the low and the high 16 bits of each square
separately at 16 bits, put together into one uint64 per block. A block is a
segment of one row, and the host adds each row's blocks exactly and the rows
into a double in the CPU's order (``feature/float_psnr_rows.h``, ADR-1499),
then divides by ``scaler``^2.

Device-free: reads the sources only. ``test_hip_float_psnr_parity`` compares
the scores on a device.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HIP_FEATURE = ROOT / "core" / "src" / "feature" / "hip"

KERNEL = "float_psnr/float_psnr_score.hip"
HOST = "float_psnr_hip.c"

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
SPACE = re.compile(r"\s+")
# A floating-point accumulator anywhere in the reduction.
FLOAT_SUM = re.compile(r"\b(?:float|double)\s+(?:my_noise|my_lo|my_hi|total\w*|w)\b")
# Both kernels square through the one helper.
SQUARE_CALLS = 2


def _code(source: str) -> str:
    """The source without comments and with whitespace collapsed."""
    return SPACE.sub(" ", COMMENT.sub(" ", source))


def _sources() -> dict[str, str]:
    return {name: (HIP_FEATURE / name).read_text(encoding="utf-8") for name in (KERNEL, HOST)}


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


def _kernel_failures(sources: dict[str, str]) -> list[str]:
    kernel = _code(sources[KERNEL])
    failures: list[str] = []
    square = _function_body(kernel, "fpsnr_square")
    if (
        "const float diff = (float)(ref - dis); return (uint32_t)(diff * diff);" not in square
        or kernel.count("fpsnr_square(") != SQUARE_CALLS + 1
    ):
        failures.append(f"{KERNEL}: the square is not the CPU's float product of the difference")
    if FLOAT_SUM.search(kernel) or "__shared__ uint32_t s_warps[" not in kernel:
        failures.append(f"{KERNEL}: a block sum is added in floating point")
    wide = _function_body(kernel, "float_psnr_kernel_16bpc")
    for piece in (
        "const bool split = bpc > 12u;",
        "my_lo = split ? (square & 0xFFFFu) : square;",
        "my_hi = split ? (square >> 16u) : 0u;",
        "total_hi = fpsnr_block_sum(s_warps, my_hi);",
    ):
        if piece not in wide:
            failures.append(f"{KERNEL}: a 16-bit block sum can overflow uint32 ({piece})")
    return failures


def _host_failures(sources: dict[str, str]) -> list[str]:
    host = _code(sources[HOST])
    kernel = _code(sources[KERNEL])
    failures: list[str] = []
    collect = _function_body(host, "collect_fex_hip")
    rows = (
        "const unsigned per_row = (s->frame_w + FPSNR_BX - 1u) / FPSNR_BX; "
        "const double total = vmaf_float_psnr_row_noise((const uint64_t *)s->rb.host_pinned, "
        "s->frame_h, per_row);"
    )
    if rows not in collect:
        failures.append(f"{HOST}: collect() does not add the rows' exact sums as the CPU does")
    if "const double noise = (total / (scaler * scaler)) / n_pix;" not in collect:
        failures.append(f"{HOST}: the sum is not divided by the scaler squared")
    size = _function_body(host, "float_psnr_hip_partials_bytes")
    if "s->wg_count * sizeof(uint64_t)" not in size:
        failures.append(f"{HOST}: the read-back is not one uint64 per block")
    halves = "partials[block_idx] = (unsigned long long)total_lo + ((unsigned long long)total_hi << 16u);"
    if halves not in _function_body(kernel, "float_psnr_kernel_16bpc"):
        failures.append(f"{KERNEL}: a block's two halves are not put together into one uint64")
    geometry = (
        re.search(r"#define FPSNR_BX 256u\b", host),
        re.search(r"#define FPSNR_BY 1u\b", host),
        re.search(r"#define FPSNR_BX 256\b", kernel),
        re.search(r"#define FPSNR_BY 1\b", kernel),
    )
    if not all(geometry):
        failures.append(f"{HOST}: a block is not a segment of one row (ADR-1499)")
    return failures


def _failures(sources: dict[str, str]) -> list[str]:
    return _kernel_failures(sources) + _host_failures(sources)


class HipFloatPsnrExactContractTest(unittest.TestCase):
    def _edited(self, name: str, old: str, new: str) -> list[str]:
        sources = _sources()
        self.assertIn(old, sources[name])
        sources[name] = sources[name].replace(old, new, 1)
        return _failures(sources)

    def test_live_sources_add_integers(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_fp32_block_sum_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    uint32_t my_noise = 0u;",
            "    float my_noise = 0.0f;",
        )
        self.assertTrue(any("added in floating point" in failure for failure in failures), failures)

    def test_integer_square_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "    const float diff = (float)(ref - dis);\n    return (uint32_t)(diff * diff);",
            "    const int diff = ref - dis;\n    return (uint32_t)(diff * diff);",
        )
        self.assertTrue(
            any("float product of the difference" in failure for failure in failures), failures
        )

    def test_unsplit_16_bit_sum_is_detected(self) -> None:
        failures = self._edited(
            KERNEL, "const bool split = bpc > 12u;", "const bool split = false;"
        )
        self.assertTrue(any("can overflow uint32" in failure for failure in failures), failures)

    def test_dropped_high_half_is_detected(self) -> None:
        failures = self._edited(
            KERNEL,
            "(unsigned long long)total_lo + ((unsigned long long)total_hi << 16u);",
            "(unsigned long long)total_lo;",
        )
        self.assertTrue(any("two halves" in failure for failure in failures), failures)

    def test_square_blocks_are_detected(self) -> None:
        # The pre-ADR-1499 geometry: 16x16 blocks mix rows.
        failures = self._edited(KERNEL, "#define FPSNR_BY 1\n", "#define FPSNR_BY 16\n")
        self.assertTrue(any("segment of one row" in failure for failure in failures), failures)

    def test_missing_scaler_is_detected(self) -> None:
        failures = self._edited(
            HOST,
            "const double noise = (total / (scaler * scaler)) / n_pix;",
            "const double noise = total / n_pix;",
        )
        self.assertTrue(any("scaler squared" in failure for failure in failures), failures)


if __name__ == "__main__":
    unittest.main()
