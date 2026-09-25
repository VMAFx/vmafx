#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the predictor test binary and verify its production diagnostic path."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

TEST_BINARY = Path(sys.argv[1]).resolve()
del sys.argv[1]


class PredictNonfiniteLogOutputTest(unittest.TestCase):
    def test_nonfinite_prediction_emits_one_structured_warning(self) -> None:
        completed = subprocess.run(  # noqa: S603 -- Meson supplies the resolved test target.
            [str(TEST_BINARY)], capture_output=True, text=True, timeout=30, check=False
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

        marker = "libvmaf WARNING predict: non-finite model score at frame 37 (value="
        matching_lines = [line for line in completed.stderr.splitlines() if marker in line]
        self.assertEqual(len(matching_lines), 1, completed.stderr)
        self.assertTrue(matching_lines[0].endswith(", failing frame"), matching_lines[0])
        self.assertIn("nan", matching_lines[0].lower())


if __name__ == "__main__":
    unittest.main()
