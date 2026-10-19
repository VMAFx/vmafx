#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FFmpeg libvmaf filters hand the AVFrame colour to libvmaf (ADR-2093, patch 0022).

A model with a `conversion_target` needs the colorimetry of both inputs, and
libvmaf takes it from vmaf_set_input_colorimetry(). Patch 0022 maps the
AVFrame range, primaries, transfer and matrix to libvmaf's enums and calls the
function once on the first frame pair, in do_vmaf() and in the software path of
do_vmaf_sycl(). Device-free: reads the series. The run with an FFmpeg is
ffmpeg-patches/test/check-libvmaf-input-colorimetry.sh.
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = (
    ROOT
    / "ffmpeg-patches"
    / "0022-libvmaf-declare-the-input-colorimetry-from-the-AVFrame-properties.patch"
)

MAPPINGS = (
    "case AVCOL_RANGE_MPEG: c.range = VMAF_COLOR_RANGE_LIMITED",
    "case AVCOL_RANGE_JPEG: c.range = VMAF_COLOR_RANGE_FULL",
    "case AVCOL_PRI_BT709:    c.primaries = VMAF_COLOR_PRIMARIES_BT709",
    "case AVCOL_PRI_BT2020:   c.primaries = VMAF_COLOR_PRIMARIES_BT2020",
    "case AVCOL_PRI_SMPTE432: c.primaries = VMAF_COLOR_PRIMARIES_SMPTE432",
    "case AVCOL_TRC_BT709:     c.trc = VMAF_COLOR_TRC_BT709",
    "case AVCOL_TRC_SMPTE2084: c.trc = VMAF_COLOR_TRC_SMPTE2084",
    "case AVCOL_SPC_BT709:      c.matrix = VMAF_COLOR_MATRIX_BT709",
    "case AVCOL_SPC_BT2020_NCL: c.matrix = VMAF_COLOR_MATRIX_BT2020_NCL",
    "case AVCOL_SPC_ICTCP:      c.matrix = VMAF_COLOR_MATRIX_ICTCP",
)
UNSPECIFIED = "if (!c.range || !c.primaries || !c.trc || !c.matrix)\n+        return 0;"
CALL = "vmaf_set_input_colorimetry(s->vmaf,"
SITES = (
    "+    if ((ret = vmaf_declare_input_color(ctx, s, ref, dist)) < 0) {\n+        av_frame_free(&dist);",
    "+        if ((ret = vmaf_declare_input_color(ctx, s, ref, dist)) < 0)\n+            return ret;",
)


class InputColorimetryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = PATCH.read_text(encoding="utf-8")

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
        self.assertIn("if (s->color_set)\n+        return 0;", self.text)
        for site in SITES:
            with self.subTest(site=site):
                self.assertEqual(self.text.count(site), 1)

    def test_the_check_refuses_a_missing_call(self) -> None:
        self.assertNotIn(CALL, self.text.replace(CALL, ""))


if __name__ == "__main__":
    unittest.main()
