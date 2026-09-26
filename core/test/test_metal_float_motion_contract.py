#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin Metal float_motion lifecycle, force-zero, debug emission, and flush idempotency contracts."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

FEATURE_DIR = Path(__file__).resolve().parents[1] / "src" / "feature" / "metal"
HOST_PATH = FEATURE_DIR / "float_motion_metal.mm"


def _extract_options_block(source: str) -> str:
    match = re.search(r"static\s+const\s+VmafOption\s+options\[\]\s*=\s*\{([\s\S]*?)\n\};", source)
    if not match:
        raise AssertionError("could not find options[] block in float_motion_metal.mm")
    return match.group(1)


def _extract_struct_block(source: str) -> str:
    match = re.search(
        r"typedef\s+struct\s+FloatMotionStateMetal\s*\{([\s\S]*?)\}\s*FloatMotionStateMetal;",
        source,
    )
    if not match:
        raise AssertionError("could not find FloatMotionStateMetal struct in float_motion_metal.mm")
    return match.group(1)


class MetalFloatMotionContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = HOST_PATH.read_text(encoding="utf-8")
        cls.options = _extract_options_block(cls.source)
        cls.struct = _extract_struct_block(cls.source)

    def test_state_struct_contains_debug_and_force_zero(self) -> None:
        self.assertRegex(self.struct, r"\bbool\s+debug\b")
        self.assertRegex(self.struct, r"\bbool\s+motion_force_zero\b")

    def test_options_table_registers_debug_and_force_zero(self) -> None:
        self.assertRegex(self.options, r'\.name\s*=\s*"debug"')
        self.assertRegex(self.options, r'\.name\s*=\s*"motion_force_zero"')
        self.assertRegex(self.options, r'\.alias\s*=\s*"force_0"')
        self.assertRegex(self.options, r'\.name\s*=\s*"motion_fps_weight"')
        self.assertRegex(self.options, r'\.alias\s*=\s*"mfw"')

    def test_force_zero_init_retains_close_callback(self) -> None:
        self.assertIn("fex->close = close_fex_metal;", self.source)
        self.assertIn("fex->flush = NULL;", self.source)
        self.assertIn("fex->submit = NULL;", self.source)
        self.assertIn("fex->collect = NULL;", self.source)
        self.assertIn("fex->extract = extract_force_zero_metal;", self.source)

    def test_force_zero_extract_gated_by_debug(self) -> None:
        match = re.search(
            r"extract_force_zero_metal\s*\([\s\S]*?\)\s*\{([\s\S]*?)\n\}", self.source
        )
        self.assertIsNotNone(match, "extract_force_zero_metal function not found")
        body = match.group(1)
        self.assertIn("VMAF_feature_motion2_score", body)
        self.assertRegex(body, r"if\s*\(\s*s->debug[\s\S]*?VMAF_feature_motion_score")

    def test_collect_motion_score_gated_by_debug(self) -> None:
        match = re.search(r"collect_fex_metal\s*\([\s\S]*?\)\s*\{([\s\S]*?)\n\}", self.source)
        self.assertIsNotNone(match, "collect_fex_metal function not found")
        body = match.group(1)
        self.assertRegex(body, r"if\s*\(\s*s->debug\s*\)[\s\S]*?VMAF_feature_motion_score")

    def test_flush_is_idempotent_with_option_derived_name(self) -> None:
        match = re.search(r"flush_fex_metal\s*\([\s\S]*?\)\s*\{([\s\S]*?)\n\}", self.source)
        self.assertIsNotNone(match, "flush_fex_metal function not found")
        body = match.group(1)
        self.assertIn("vmaf_dictionary_get", body)
        self.assertIn("vmaf_feature_collector_get_score", body)
        self.assertRegex(
            body, r"vmaf_feature_collector_get_score[\s\S]*?==\s*0\s*\)\s*\{\s*return\s+1\s*;"
        )

    def test_zero_goto_in_translation_unit(self) -> None:
        self.assertNotRegex(self.source, r"\bgoto\b")


if __name__ == "__main__":
    unittest.main()
