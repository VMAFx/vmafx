# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Contract tests for Netflix golden gate makefile target and build directory isolation."""

import re
import unittest
from pathlib import Path
from typing import ClassVar

ROOT = Path(__file__).resolve().parent.parent.parent.parent
MAKEFILE = ROOT / "Makefile"


class GoldenGateMakefileContractTest(unittest.TestCase):
    """Pin Makefile target and isolation variables for test-netflix-golden."""

    makefile_text: ClassVar[str]

    @classmethod
    def setUpClass(cls) -> None:
        cls.makefile_text = MAKEFILE.read_text(encoding="utf-8")

    def test_golden_build_dir_variable_defined(self) -> None:
        """Makefile must define GOLDEN_BUILD_DIR with default under core/ (or LIBVMAF_DIR)."""
        pattern = r"^GOLDEN_BUILD_DIR\s*(\?=|:=|=)\s*(.+)$"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Makefile must define GOLDEN_BUILD_DIR")
        assert match is not None
        val = match.group(2).strip()
        self.assertTrue(
            "build-golden" in val,
            f"GOLDEN_BUILD_DIR must point to a dedicated build-golden dir, got: {val}",
        )

    def test_build_golden_target_defined(self) -> None:
        """Makefile must define a build-golden target."""
        pattern = r"^build-golden\s*:"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Makefile must define a build-golden target")

    def test_test_netflix_golden_depends_on_build_golden_not_build(self) -> None:
        """test-netflix-golden must depend on build-golden, never on generic developer 'build'."""
        pattern = r"^test-netflix-golden\s*:\s*(.+)$"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Makefile must define test-netflix-golden target")
        assert match is not None
        prereqs = match.group(1).split()
        self.assertIn(
            "build-golden",
            prereqs,
            f"test-netflix-golden must depend on build-golden, got: {prereqs}",
        )
        self.assertNotIn(
            "build",
            prereqs,
            f"test-netflix-golden must NOT depend on generic developer 'build', got: {prereqs}",
        )

    def test_test_netflix_golden_exports_vmaf_build_dir(self) -> None:
        """test-netflix-golden recipe must export VMAF_BUILD_DIR to point pytest at isolated build."""
        # Locate recipe for test-netflix-golden
        pattern = r"^test-netflix-golden\s*:.*?\n((?:\t.*\n)+)"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Could not extract test-netflix-golden recipe")
        assert match is not None
        recipe = match.group(1)
        self.assertIn(
            "VMAF_BUILD_DIR=",
            recipe,
            "test-netflix-golden recipe must pass VMAF_BUILD_DIR to isolate python runner from core/build",
        )
        self.assertIn(
            "GOLDEN_BUILD_DIR",
            recipe,
            "test-netflix-golden recipe must reference GOLDEN_BUILD_DIR",
        )

    def test_clean_target_removes_golden_build_dir(self) -> None:
        """clean target in Makefile must clean GOLDEN_BUILD_DIR."""
        pattern = r"^clean\s*:.*?\n((?:\t.*\n)+)"
        match = re.search(pattern, self.makefile_text, re.MULTILINE)
        self.assertIsNotNone(match, "Could not extract clean recipe")
        assert match is not None
        recipe = match.group(1)
        self.assertTrue(
            "GOLDEN_BUILD_DIR" in recipe or "build-golden" in recipe,
            f"clean target recipe must clean GOLDEN_BUILD_DIR, got: {recipe}",
        )


if __name__ == "__main__":
    unittest.main()
