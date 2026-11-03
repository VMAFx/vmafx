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
CHANGE_BRANCH = (
    "if (!color_tags_equal(&ref_tags, &s->ref_color) || !color_tags_equal(&dist_tags, &s->dist_color)) {"
    " const VmafxStatus st = vmafx_context_set_default_color(s->context, r, d, &error);"
    " if (st != VMAFX_OK) {"
    ' av_log(ctx, AV_LOG_ERROR, "vmafx: input colour metadata changed between frames\\n");'
    ' return vmafx_fail(ctx, st, error, "vmafx_context_set_default_color");'
    " }"
    " s->ref_color = ref_tags;"
    " s->dist_color = dist_tags;"
    " }"
)


def flat(text: str) -> str:
    """The text with every whitespace run as one space: line breaks are layout, not mapping."""
    return " ".join(text.split())


class InputColorimetryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = SOURCE.read_text(encoding="utf-8")
        self.flat_text = flat(self.text)

    def test_every_attribute_is_mapped(self) -> None:
        for line in MAPPINGS:
            with self.subTest(line=line):
                self.assertIn(flat(line), self.flat_text)

    def test_a_missing_mapping_is_refused(self) -> None:
        """Negative: the source without one mapping no longer holds it."""
        dropped = self.flat_text.replace(flat(MAPPINGS[0]), "")
        self.assertNotIn(flat(MAPPINGS[0]), dropped)
        self.assertIn(flat(MAPPINGS[1]), dropped)

    def test_an_input_with_an_unnamed_attribute_stays_unspecified(self) -> None:
        self.assertIn(flat(UNSPECIFIED), self.flat_text)
        self.assertIn(flat("? &ref_tags.color : NULL"), self.flat_text)
        self.assertIn(flat("? &dist_tags.color : NULL"), self.flat_text)

    def test_declared_once_before_the_first_read(self) -> None:
        self.assertEqual(self.flat_text.count(flat(CALL)), 2)
        self.assertIn(flat("if (s->color_set)"), self.flat_text)
        self.assertEqual(self.flat_text.count(flat(SITE)), 1)

    def test_the_check_refuses_a_missing_call(self) -> None:
        self.assertNotIn(flat(CALL), self.flat_text.replace(flat(CALL), ""))

    def test_midstream_color_change_rejected(self) -> None:
        """Negative: mid-stream change in colour metadata calls set_default_color and rejects on non-OK status."""
        self.assertIn(flat(CHANGE_BRANCH), self.flat_text)

    def test_unchanged_color_passes(self) -> None:
        """Positive: matching colour tags between frames pass with 0."""
        self.assertIn(flat("if (s->color_set) {"), self.flat_text)
        self.assertIn(flat("return 0;"), self.flat_text)

    def test_planted_missing_midstream_color_check_fails(self) -> None:
        """Planted defect: omitting the midstream colour change check fails."""
        mutated = self.flat_text.replace(
            flat(
                "if (!color_tags_equal(&ref_tags, &s->ref_color) || !color_tags_equal(&dist_tags, &s->dist_color))"
            ),
            flat("if (0)"),
        )
        self.assertNotIn(
            flat(
                "if (!color_tags_equal(&ref_tags, &s->ref_color) || !color_tags_equal(&dist_tags, &s->dist_color))"
            ),
            mutated,
        )

    def test_planted_unconditional_einval_on_tag_change_fails(self) -> None:
        """Planted defect: returning AVERROR(EINVAL) on a tag change without calling set_default_color fails."""
        round1_branch = (
            "if (!color_tags_equal(&ref_tags, &s->ref_color) || !color_tags_equal(&dist_tags, &s->dist_color)) {"
            ' av_log(ctx, AV_LOG_ERROR, "vmafx: input colour metadata changed between frames\\n");'
            " return AVERROR(EINVAL);"
            "}"
        )
        mutated = self.flat_text.replace(flat(CHANGE_BRANCH), flat(round1_branch))
        self.assertNotEqual(mutated, self.flat_text)
        self.assertNotIn(flat(CHANGE_BRANCH), mutated)
        self.assertEqual(mutated.count(flat(CALL)), 1)


class MidstreamColorChangeSimulationTests(unittest.TestCase):
    """Behavioral unit test verifying the colour-tag equality and change-detection logic."""

    @staticmethod
    def map_range(r: int) -> int:
        return {1: 1, 2: 2}.get(r, 0)

    @staticmethod
    def map_pri(p: int) -> int:
        return {1: 1, 9: 2, 12: 3}.get(p, 0)

    @staticmethod
    def map_trc(t: int) -> int:
        return {1: 1, 16: 2}.get(t, 0)

    @staticmethod
    def map_matrix(m: int) -> int:
        return {1: 1, 9: 2, 14: 3}.get(m, 0)

    def frame_color_tags(self, r: int, p: int, t: int, m: int) -> dict:
        mr, mp, mt, mm = self.map_range(r), self.map_pri(p), self.map_trc(t), self.map_matrix(m)
        supported = bool(mr and mp and mt and mm)
        return {
            "supported": supported,
            "color": (mr, mp, mt, mm) if supported else None,
            "range": r,
            "primaries": p,
            "trc": t,
            "space": m,
        }

    def tags_equal(self, a: dict, b: dict) -> bool:
        if a["supported"] != b["supported"]:
            return False
        if a["supported"]:
            return a["color"] == b["color"]
        return (
            a["range"] == b["range"]
            and a["primaries"] == b["primaries"]
            and a["trc"] == b["trc"]
            and a["space"] == b["space"]
        )

    def test_unchanged_supported_tags_pass(self) -> None:
        # BT.2020 / PQ / Limited
        f0 = self.frame_color_tags(1, 9, 16, 9)
        f1 = self.frame_color_tags(1, 9, 16, 9)
        self.assertTrue(self.tags_equal(f0, f1))

    def test_unchanged_unsupported_tags_pass(self) -> None:
        # Untagged (all 0)
        f0 = self.frame_color_tags(0, 0, 0, 0)
        f1 = self.frame_color_tags(0, 0, 0, 0)
        self.assertTrue(self.tags_equal(f0, f1))

    def test_midstream_change_supported_to_different_supported_fails(self) -> None:
        # BT.2020 changed to BT.709
        f0 = self.frame_color_tags(1, 9, 16, 9)
        f1 = self.frame_color_tags(1, 1, 1, 1)
        self.assertFalse(self.tags_equal(f0, f1))

    def test_midstream_change_supported_to_unsupported_fails(self) -> None:
        # BT.2020 changed to untagged
        f0 = self.frame_color_tags(1, 9, 16, 9)
        f1 = self.frame_color_tags(0, 0, 0, 0)
        self.assertFalse(self.tags_equal(f0, f1))

    def test_midstream_change_unsupported_to_supported_fails(self) -> None:
        # Untagged changed to BT.2020
        f0 = self.frame_color_tags(0, 0, 0, 0)
        f1 = self.frame_color_tags(1, 9, 16, 9)
        self.assertFalse(self.tags_equal(f0, f1))

    def test_midstream_change_unsupported_to_different_unsupported_fails(self) -> None:
        # Unsupported tag 100 changed to unsupported tag 101
        f0 = self.frame_color_tags(0, 100, 0, 0)
        f1 = self.frame_color_tags(0, 101, 0, 0)
        self.assertFalse(self.tags_equal(f0, f1))


if __name__ == "__main__":
    unittest.main()
