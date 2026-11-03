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

import os
import re
import tempfile
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
REPORT_PATH_CREATE_CHECK = re.compile(
    r"avpriv_open\(\s*s->output\s*,\s*O_WRONLY\s*\|\s*O_CREAT\s*\|\s*O_EXCL\s*,\s*0666\s*\)"
)
REPORT_PROBE_CREATED_ONLY_ON_CREATE = re.compile(
    r"if\s*\(fd\s*>=\s*0\)\s*\{\s*close\(fd\);\s*s->output_created_by_probe\s*=\s*1;\s*return 0;\s*\}"
)
REPORT_PROBE_RB_CHECK = re.compile(r'avpriv_fopen_utf8\(\s*s->output\s*,\s*"rb"\s*\)')


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
    if not REPORT_PATH_CREATE_CHECK.search(clean):
        found.append("report_path_check does not use create-excl open semantics")
    if not REPORT_PROBE_CREATED_ONLY_ON_CREATE.search(clean):
        found.append("report_path_check sets output_created_by_probe when not created by probe")
    if REPORT_PROBE_RB_CHECK.search(clean):
        found.append("report_path_check derives existence from rb open")
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

    def test_unreadable_existing_log_path_survives(self) -> None:
        """Positive: a pre-existing unreadable (mode 0200) log path survives uninit."""
        self.assertEqual(problems(self.code), [])
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "report.json"
            log.write_text("PREEXISTING_UNREADABLE", encoding="utf-8")
            log.chmod(0o200)
            try:
                fd = os.open(str(log), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
                os.close(fd)
                created_by_probe = True
            except FileExistsError:
                created_by_probe = False
            with log.open("ab"):
                pass
            if created_by_probe:
                log.unlink()
            self.assertTrue(log.exists())
            log.chmod(0o600)
            self.assertEqual(log.read_text(encoding="utf-8"), "PREEXISTING_UNREADABLE")

    def test_planted_rb_probe_fails(self) -> None:
        """Negative/planted: deriving existence from rb open fails the contract."""
        mutated = self.code.replace(
            "const int fd = avpriv_open(s->output, O_WRONLY | O_CREAT | O_EXCL, 0666);",
            'FILE *probe = avpriv_fopen_utf8(s->output, "rb");',
        )
        self.assertNotEqual(mutated, self.code)
        self.assertIn(
            "report_path_check derives existence from rb open",
            problems(mutated),
        )


if __name__ == "__main__":
    unittest.main()
