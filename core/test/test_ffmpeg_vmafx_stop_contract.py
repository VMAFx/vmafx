#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FFmpeg vmafx filter prints no score after an error (ADR-1768).

The vmafx filter (ffmpeg-patches/src/vf_vmafx.c) adheres to ADR-1768:
- a failed submit_pair() or filter operation logs the frame and the
  error once, records the stop in s->failed and frees the frame;
- frame_cnt advances only after a successful submit;
- uninit() prints no pooled score after a stop or a failed flush;
- a failed flush logs the frame count and the error.

Device run: ffmpeg-patches/test/check-vmafx-no-score-after-error.sh.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ffmpeg-patches" / "src" / "vf_vmafx.c"

STOP_ON_ERROR = re.compile(
    r"if\s*\(ret\s*<\s*0\)\s*\{\s*s->failed\s*=\s*1;\s*av_frame_free\(&dist\);\s*return ret;\s*\}"
)
ADVANCE_AFTER_SUBMIT = re.compile(
    r"const\s+uint64_t\s+index\s*=\s*s->frame_cnt;.*?submit_pair\(ctx,\s*ref,\s*dist,\s*index\);.*?s->frame_cnt\+\+;",
    re.DOTALL,
)
NO_SCORE_AFTER_STOP = re.compile(
    r"if\s*\(s->context\s*&&\s*!s->failed\)\s*\{.*?log_pooled\(ctx\);.*?write_report\(ctx\);",
    re.DOTALL,
)
SUBMIT_ERROR_LOG = re.compile(r'vmafx_submit of frame %"\s*PRIu64\s*" failed \(%s\)')
FLUSH_ERROR_LOG = re.compile(r'flushing vmafx after frame %"\s*PRIu64\s*" failed \(%s\)')


def problems(code: str) -> list[str]:
    """Every way `code` breaks the stop contract."""
    clean = re.sub(r"/\*.*?\*/", "", code, flags=re.DOTALL)
    found = []
    if not STOP_ON_ERROR.search(clean):
        found.append("a failed submit or filter step does not record stop and free frame")
    if not ADVANCE_AFTER_SUBMIT.search(clean):
        found.append("frame_cnt is advanced before submit succeeds")
    if not NO_SCORE_AFTER_STOP.search(clean):
        found.append("uninit() prints score or writes report after failure")
    if not SUBMIT_ERROR_LOG.search(clean):
        found.append("submit failure error does not name frame and error")
    if not FLUSH_ERROR_LOG.search(clean):
        found.append("flush failure error does not name frame and error")
    return found


class VmafxStopContract(unittest.TestCase):
    def setUp(self) -> None:
        self.code = SOURCE.read_text(encoding="utf-8")

    def test_filter_holds_the_contract(self) -> None:
        self.assertEqual(problems(self.code), [])

    def test_a_stop_that_is_not_recorded_is_refused(self) -> None:
        mutated = self.code.replace("s->failed = 1;", "")
        self.assertNotEqual(mutated, self.code)
        self.assertIn(
            "a failed submit or filter step does not record stop and free frame",
            problems(mutated),
        )

    def test_frame_cnt_advanced_before_the_read_is_refused(self) -> None:
        mutated = self.code.replace(
            "const uint64_t index = s->frame_cnt;",
            "const uint64_t index = s->frame_cnt++;",
        )
        self.assertNotEqual(mutated, self.code)
        self.assertIn("frame_cnt is advanced before submit succeeds", problems(mutated))

    def test_score_printed_after_failure_is_refused(self) -> None:
        mutated = self.code.replace(
            "if (s->context && !s->failed) {",
            "if (s->context) {",
        )
        self.assertNotEqual(mutated, self.code)
        self.assertIn(
            "uninit() prints score or writes report after failure",
            problems(mutated),
        )


if __name__ == "__main__":
    unittest.main()
