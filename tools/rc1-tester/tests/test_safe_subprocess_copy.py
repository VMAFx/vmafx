# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The wheel's runner copy stays byte-identical to scripts/lib/safe_subprocess.py."""

from __future__ import annotations

import unittest
from pathlib import Path

TESTS = Path(__file__).resolve().parent
COPY = TESTS.parent / "src" / "vmaf_rc1_tester" / "_safe_subprocess.py"
SOURCE = TESTS.parents[2] / "scripts" / "lib" / "safe_subprocess.py"


def drift(source: bytes, copy: bytes) -> str | None:
    """Return why the copy differs from the source, or None when equal."""
    if source == copy:
        return None
    return "copy differs from scripts/lib/safe_subprocess.py; run: cp {} {}".format(
        "scripts/lib/safe_subprocess.py",
        "tools/rc1-tester/src/vmaf_rc1_tester/_safe_subprocess.py",
    )


class SafeSubprocessCopyTest(unittest.TestCase):
    def test_copy_matches_source(self) -> None:
        self.assertTrue(SOURCE.is_file(), f"missing {SOURCE}")
        self.assertIsNone(drift(SOURCE.read_bytes(), COPY.read_bytes()))

    def test_planted_edit_is_refused(self) -> None:
        source = SOURCE.read_bytes()
        self.assertIsNotNone(drift(source, source + b"\n"))
        self.assertIsNotNone(drift(source, source.replace(b"4096", b"4097", 1)))

    def test_empty_copy_is_refused(self) -> None:
        self.assertIsNotNone(drift(SOURCE.read_bytes(), b""))


if __name__ == "__main__":
    unittest.main()
