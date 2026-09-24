#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Lock the user-visible formatting of restored libvmaf diagnostics."""

from __future__ import annotations

import unittest
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parents[1] / "src"


class VmafLogCallsiteFormatTests(unittest.TestCase):
    def source(self, relative: str) -> str:
        return (SOURCE_ROOT / relative).read_text(encoding="utf-8")

    def test_cuda_error_level_does_not_repeat_severity(self) -> None:
        source = self.source("cuda/common.c")
        self.assertNotIn('vmaf_log(VMAF_LOG_LEVEL_ERROR, "Error: ', source)
        self.assertEqual(source.count('"failed to initialize CUDA\\n"'), 2)
        self.assertEqual(source.count('"device_id %d is out of range\\n"'), 1)

    def test_restored_diagnostics_are_line_terminated(self) -> None:
        expected = {
            "feature/luminance_tools.cpp": (
                '"unknown pixel range received\\n"',
                '"unknown EOTF received\\n"',
            ),
            "feature/speed.c": (
                '"SpEED: image too small, operating width or height is 0\\n"',
                '"invalid speed_kernelscale\\n"',
            ),
            "feature/vif.c": ('"invalid vif_kernelscale: %f\\n"',),
        }

        for relative, messages in expected.items():
            with self.subTest(source=relative):
                source = self.source(relative)
                for message in messages:
                    self.assertIn(message, source)


if __name__ == "__main__":
    unittest.main()
