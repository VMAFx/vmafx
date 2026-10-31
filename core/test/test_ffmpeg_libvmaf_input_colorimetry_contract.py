#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FFmpeg vmafx filter hands the AVFrame colour to VMAFx (ADR-2093, ADR-2094).

A model with a `conversion_target` needs the colorimetry of both inputs, and
VMAFx takes it from vmafx_context_set_default_color(). vf_vmafx.c maps the
AVFrame range, primaries, transfer and matrix to VMAFx's enums and calls the
function once on the first frame pair, in submit_pair(). Device-free: reads the
filter source.
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ffmpeg-patches" / "src" / "vf_vmafx.c"

MAPPINGS = (
    "case AVCOL_RANGE_MPEG: return VMAFX_COLOR_RANGE_LIMITED;",
    "case AVCOL_RANGE_JPEG: return VMAFX_COLOR_RANGE_FULL;",
    "case AVCOL_PRI_BT709:    return VMAFX_COLOR_PRIMARIES_BT709;",
    "case AVCOL_PRI_BT2020:   return VMAFX_COLOR_PRIMARIES_BT2020;",
    "case AVCOL_PRI_SMPTE432: return VMAFX_COLOR_PRIMARIES_SMPTE432;",
    "case AVCOL_TRC_BT709:     return VMAFX_COLOR_TRC_BT709;",
    "case AVCOL_TRC_SMPTE2084: return VMAFX_COLOR_TRC_SMPTE2084;",
    "case AVCOL_SPC_BT709:      return VMAFX_COLOR_MATRIX_BT709;",
    "case AVCOL_SPC_BT2020_NCL: return VMAFX_COLOR_MATRIX_BT2020_NCL;",
    "case AVCOL_SPC_ICTCP:      return VMAFX_COLOR_MATRIX_ICTCP;",
)
UNSPECIFIED = "if (!c.range || !c.primaries || !c.trc || !c.matrix)\n        return 0;"
CALL = "vmafx_context_set_default_color(s->context,"
SITE = "int ret = vmafx_declare_input_color(ctx, s, ref, dist);"


class InputColorimetryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = SOURCE.read_text(encoding="utf-8")

    def test_every_attribute_is_mapped(self) -> None:
        for line in MAPPINGS:
            with self.subTest(line=line):
                self.assertIn(line, self.text)

    def test_an_input_with_an_unnamed_attribute_stays_unspecified(self) -> None:
        self.assertIn(UNSPECIFIED, self.text)
        self.assertIn("? &ref_color : NULL", self.text)
        self.assertIn("? &dist_color : NULL", self.text)

    def test_declared_once_before_the_first_read(self) -> None:
        self.assertEqual(self.text.count(CALL), 1)
        self.assertIn("if (s->color_set)\n        return 0;", self.text)
        self.assertEqual(self.text.count(SITE), 1)

    def test_the_check_refuses_a_missing_call(self) -> None:
        self.assertNotIn(CALL, self.text.replace(CALL, ""))


if __name__ == "__main__":
    unittest.main()
