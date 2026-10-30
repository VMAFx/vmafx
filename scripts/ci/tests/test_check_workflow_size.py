#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for check-workflow-size.py: positive, negative and boundary cases (HISS-15)."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHECKER = ROOT / "scripts" / "ci" / "check-workflow-size.py"


class CheckWorkflowSizeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.tmp_path = Path(self.tmp.name)

    def run_checker(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603
            [sys.executable, str(CHECKER), *args],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_planted_oversize_file_fails(self) -> None:
        """Negative case: a 65537-byte file exceeds the 65536 bound and fails."""
        oversize = self.tmp_path / "oversize.yml"
        oversize.write_bytes(b"a" * 65537)
        res = self.run_checker(str(oversize))
        self.assertEqual(res.returncode, 1)
        self.assertIn("oversize.yml (65537 bytes) exceeds 65536 byte limit", res.stderr)

    def test_boundary_exact_size_passes(self) -> None:
        """Boundary case: a file of exactly 65536 bytes passes."""
        boundary = self.tmp_path / "boundary.yml"
        boundary.write_bytes(b"b" * 65536)
        res = self.run_checker(str(boundary))
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_boundary_one_byte_under_passes(self) -> None:
        """Boundary case: a file of 65535 bytes passes."""
        under = self.tmp_path / "under.yml"
        under.write_bytes(b"c" * 65535)
        res = self.run_checker(str(under))
        self.assertEqual(res.returncode, 0, res.stderr)

    def test_repository_workflows_positive(self) -> None:
        """Positive case: all workflow files in the repository pass."""
        res = self.run_checker()
        self.assertEqual(res.returncode, 0, res.stderr)


if __name__ == "__main__":
    unittest.main()
