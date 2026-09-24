#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep the production feature collector on one authoritative C++ source."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
FEATURE_DIR = REPO_ROOT / "core" / "src" / "feature"
SOURCE_MESON = REPO_ROOT / "core" / "src" / "meson.build"


class FeatureCollectorSourceAuthorityTest(unittest.TestCase):
    def test_only_cpp_implementation_exists(self) -> None:
        self.assertTrue((FEATURE_DIR / "feature_collector.cpp").is_file())
        self.assertFalse(
            (FEATURE_DIR / "feature_collector.c").exists(),
            "feature_collector.c must not be resurrected beside the C++ authority",
        )

    def test_production_build_uses_cpp_authority(self) -> None:
        source = SOURCE_MESON.read_text(encoding="utf-8")
        cpp_reference = "feature_src_dir + 'feature_collector.cpp'"
        c_reference = "feature_src_dir + 'feature_collector.c'"
        self.assertEqual(source.count(cpp_reference), 1, "production must compile C++ exactly once")
        self.assertEqual(source.count(c_reference), 0, "production still compiles the C twin")

    def test_no_build_file_references_c_twin(self) -> None:
        references = []
        for build_file in (REPO_ROOT / "core").rglob("meson.build"):
            if any(part.startswith((".", "build")) for part in build_file.parts):
                continue
            if re.search(
                r"(?<![A-Za-z0-9_])feature_collector\.c(?!pp)",
                build_file.read_text(encoding="utf-8"),
            ):
                references.append(str(build_file.relative_to(REPO_ROOT)))
        self.assertEqual(references, [], f"stale C source references: {references}")

    def test_no_source_or_test_file_text_includes_collector_implementation(self) -> None:
        include_pattern = re.compile(
            r'^[ \t]*#[ \t]*include[ \t]*["<][^">]*feature_collector\.(?:c|cpp)[">]',
            re.MULTILINE,
        )
        violations = []
        for path in (REPO_ROOT / "core").rglob("*"):
            if path.suffix not in (".c", ".cpp", ".h"):
                continue
            if any(part.startswith((".", "build")) for part in path.parts):
                continue
            content = path.read_text(encoding="utf-8", errors="replace")
            if include_pattern.search(content):
                violations.append(str(path.relative_to(REPO_ROOT)))
        self.assertEqual(
            violations,
            [],
            f"source or test files must not text-include collector implementation: {violations}",
        )


if __name__ == "__main__":
    unittest.main()
