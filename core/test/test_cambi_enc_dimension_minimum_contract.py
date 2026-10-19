#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the CAMBI `enc_width` / `enc_height` minimum of every extractor to 144.

Netflix/vmaf 4f3f71b68 lowers the minimum of both options on the CPU extractor
from 180 / 150 to 144, so a native 144p encode is a valid CAMBI option. The
CUDA, HIP, SYCL and Metal twins keep their own option tables (a device-free
comparison with the CPU's would need each backend built); one that kept 180 /
150 would refuse a size the CPU extractor accepts. ADR-2093.

Device-free: reads the sources only.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MINIMUM = 144
TABLES = (
    "core/src/feature/cambi.c",
    "core/src/feature/cuda/integer_cambi_cuda.c",
    "core/src/feature/hip/integer_cambi_hip.c",
    "core/src/feature/sycl/integer_cambi_sycl.cpp",
    "core/src/feature/metal/integer_cambi_metal.mm",
)
# The first number after the option's name that is the lower bound: either
# `.min = N` or the `0, N` of "default, min" in the CPU / SYCL helper forms.
LOWER_BOUND = re.compile(r"(?:\.min\s*=\s*|\b0,\s*)(\d+)\b")


def lower_bound(text: str, option: str) -> int:
    start = text.index(f'"{option}"')
    found = LOWER_BOUND.search(text[start : start + 500])
    if found is None:
        raise AssertionError(f"no lower bound found after {option!r}")
    return int(found.group(1))


class CambiEncDimensionMinimumTests(unittest.TestCase):
    def test_every_table_has_the_minimum(self) -> None:
        for path in TABLES:
            text = (REPO / path).read_text(encoding="utf-8")
            for option in ("enc_width", "enc_height"):
                with self.subTest(path=path, option=option):
                    self.assertEqual(lower_bound(text, option), MINIMUM)

    def test_the_check_refuses_the_old_minimum(self) -> None:
        old = '{ .name = "enc_width", .default_val.i = 0, .min = 180, .max = 7680 }'
        self.assertEqual(lower_bound(old, "enc_width"), 180)
        old_cpu = 'CAMBI_OPTION("enc_height", "h", enc_height, VMAF_OPT_TYPE_INT, i, 0, 150, 7680,'
        self.assertEqual(lower_bound(old_cpu, "enc_height"), 150)


if __name__ == "__main__":
    sys.exit(unittest.main())
