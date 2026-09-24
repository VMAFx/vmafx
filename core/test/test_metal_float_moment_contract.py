#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the exact-integer Metal float_moment reduction without Apple hardware."""

from __future__ import annotations

import struct
import unittest
from pathlib import Path

FEATURE_DIR = Path(__file__).resolve().parents[1] / "src" / "feature" / "metal"
KERNEL_PATH = FEATURE_DIR / "float_moment.metal"
HOST_PATH = FEATURE_DIR / "float_moment_metal.mm"
UINT32_MASK = (1 << 32) - 1
WORKGROUP_THREADS = 256
FIXTURE_WIDTH = 256
FIXTURE_HEIGHT = 144
FIXTURE_SCALER = 4


def _float32(value: float) -> float:
    """Round one operation result to IEEE-754 binary32."""
    return struct.unpack("!f", struct.pack("!f", value))[0]


def _legacy_float_workgroup_sum(lane_values: list[float]) -> float:
    """Model the former eight-SIMD-group binary32 reduction."""
    if len(lane_values) != WORKGROUP_THREADS:
        raise ValueError("a float_moment workgroup has exactly 256 lanes")
    simd_totals: list[float] = []
    for group in range(8):
        total = 0.0
        for value in lane_values[group * 32 : (group + 1) * 32]:
            total = _float32(total + value)
        simd_totals.append(total)
    workgroup = 0.0
    for total in simd_totals:
        workgroup = _float32(workgroup + total)
    return workgroup


def _fixture_raw_value(row: int, col: int, variant: int) -> int:
    value = (row + col) & 0xFF
    delta = 0 if variant == 0 else ((row * 9 + col) & 0x7) - 4
    value = max(0, min(255, value + delta))
    low_bits = (row * 7 + col * 5 + variant) & (FIXTURE_SCALER - 1)
    return value * FIXTURE_SCALER + low_bits


def _legacy_fixture_second_moment(variant: int) -> tuple[float, float]:
    """Return CPU and former-Metal second moments for the 10-bit fixture."""
    cpu_sum = 0.0
    metal_sum = 0.0
    for group_y in range((FIXTURE_HEIGHT + 15) // 16):
        for group_x in range((FIXTURE_WIDTH + 15) // 16):
            lanes: list[float] = []
            for local_y in range(16):
                for local_x in range(16):
                    row = group_y * 16 + local_y
                    col = group_x * 16 + local_x
                    if row >= FIXTURE_HEIGHT or col >= FIXTURE_WIDTH:
                        lanes.append(0.0)
                        continue
                    raw = _fixture_raw_value(row, col, variant)
                    normalized = _float32(raw / FIXTURE_SCALER)
                    square = _float32(normalized * normalized)
                    lanes.append(square)
                    cpu_sum += square
            metal_sum += _legacy_float_workgroup_sum(lanes)
    pixels = FIXTURE_WIDTH * FIXTURE_HEIGHT
    return cpu_sum / pixels, metal_sum / pixels


def _split_without_carry_workgroup_sum(value: int) -> int:
    """Model #1029's separate uint32 simd_sum calls without carry."""
    lane_lo = (32 * (value & UINT32_MASK)) & UINT32_MASK
    lane_hi = (32 * (value >> 32)) & UINT32_MASK
    return 8 * ((lane_hi << 32) | lane_lo)


class MetalFloatMomentContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.kernel = KERNEL_PATH.read_text(encoding="utf-8")
        cls.host = HOST_PATH.read_text(encoding="utf-8")

    def test_ten_bit_fixture_exposes_float_workgroup_rounding(self) -> None:
        for variant in (0, 1):
            with self.subTest(variant=variant):
                cpu, legacy_metal = _legacy_fixture_second_moment(variant)
                self.assertGreater(abs(cpu - legacy_metal), 1.0e-4)

    def test_original_split_reduction_loses_low_half_carry(self) -> None:
        second_moment = 65535 * 65535
        exact = WORKGROUP_THREADS * second_moment
        self.assertNotEqual(_split_without_carry_workgroup_sum(second_moment), exact)

    def test_kernel_reduces_raw_moments_as_uint64(self) -> None:
        self.assertNotIn("threadgroup float", self.kernel)
        self.assertNotIn("simd_sum(", self.kernel)
        for name in ("r1", "d1", "r2", "d2"):
            with self.subTest(moment=name):
                self.assertRegex(
                    self.kernel,
                    rf"threadgroup\s+ulong\s+tg_{name}\[FM_THREADS_PER_GROUP\]",
                )
                self.assertIn(f"tg_{name}[lid] = my_{name};", self.kernel)
                self.assertIn(f"wg_{name} += tg_{name}[i];", self.kernel)

    def test_kernel_emits_four_lo_hi_uint32_pairs(self) -> None:
        expected_bindings = {
            "r1_lo": 2,
            "r1_hi": 3,
            "d1_lo": 4,
            "d1_hi": 5,
            "r2_lo": 6,
            "r2_hi": 7,
            "d2_lo": 8,
            "d2_hi": 9,
        }
        for name, binding in expected_bindings.items():
            with self.subTest(buffer=name):
                pattern = rf"device\s+uint\s+\*{name}\s+\[\[buffer\({binding}\)\]\]"
                self.assertRegex(self.kernel, pattern)
        for name in ("r1", "d1", "r2", "d2"):
            self.assertIn(f"write_u64({name}_lo, {name}_hi, idx, wg_{name});", self.kernel)

    def test_host_reconstructs_and_normalizes_exact_partials(self) -> None:
        self.assertIn("#define FM_PARTIAL_BUFFER_COUNT 8u", self.host)
        self.assertIn("VmafMetalKernelBuffer rb[FM_PARTIAL_BUFFER_COUNT]", self.host)
        self.assertRegex(self.host, r"\(uint64_t\)hi\[i\]\s*<<\s*32u")
        self.assertIn("const double denom1 = n_pix * scaler;", self.host)
        self.assertIn("const double denom2 = denom1 * scaler;", self.host)
        self.assertRegex(self.host, r"atIndex:\(NSUInteger\)\(b \+ 2u\)")
        self.assertRegex(self.host, r"atIndex:10\]")
        self.assertRegex(self.host, r"atIndex:11\]")


if __name__ == "__main__":
    unittest.main()
