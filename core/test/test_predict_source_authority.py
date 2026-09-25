#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Keep one compiled predictor authority and tests on headers/linked objects."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PREDICT_SOURCE = REPO_ROOT / "core" / "src" / "predict.c"
PREDICT_INTERNAL = REPO_ROOT / "core" / "src" / "predict_internal.h"
PREDICT_TEST = REPO_ROOT / "core" / "test" / "test_predict.c"
SOURCE_MESON = REPO_ROOT / "core" / "src" / "meson.build"
TEST_MESON = REPO_ROOT / "core" / "test" / "meson.build"
IMPLEMENTATION_INCLUDE = re.compile(
    r'^[ \t]*#[ \t]*include[ \t]*["<][^">]*predict\.c[">]', re.MULTILINE
)


class PredictSourceAuthorityTest(unittest.TestCase):
    def test_test_does_not_text_include_predict_implementation(self) -> None:
        source = PREDICT_TEST.read_text(encoding="utf-8")
        self.assertIsNone(
            IMPLEMENTATION_INCLUDE.search(source),
            "test_predict.c must link predictor code instead of text-including predict.c",
        )

    def test_shared_math_helpers_have_one_header_authority(self) -> None:
        self.assertTrue(PREDICT_INTERNAL.is_file(), "predict_internal.h must own shared helpers")
        include = '#include "predict_internal.h"'
        self.assertIn(include, PREDICT_SOURCE.read_text(encoding="utf-8"))
        self.assertIn(include, PREDICT_TEST.read_text(encoding="utf-8"))

    def test_predictor_has_one_compiled_source_authority(self) -> None:
        source_meson = SOURCE_MESON.read_text(encoding="utf-8")
        test_meson = TEST_MESON.read_text(encoding="utf-8")
        self.assertEqual(source_meson.count("src_dir + 'predict.c'"), 1)
        self.assertIn("predict_c_lib = static_library(", source_meson)
        self.assertIn("predict_c_lib.extract_all_objects(recursive: true)", source_meson)
        self.assertNotIn("'../src/predict.c'", test_meson)
        self.assertIn(
            "predict_test_dependencies = gpu_all_deps + [predict_c_dependency]", test_meson
        )


if __name__ == "__main__":
    unittest.main()
