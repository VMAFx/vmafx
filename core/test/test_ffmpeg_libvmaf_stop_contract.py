#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FFmpeg libvmaf and libvmaf_cuda filters print no score after an error (ADR-1768).

Upstream FFmpeg's shared `uninit()` pools the frames read before a mid-run
error and prints `VMAF score:` for them, a score over fewer frames than were
decoded, and prints an uninitialised value when the pooled score fails. The
fork's series (patch 0021) diverges on purpose. This test reads the series,
without a device or an FFmpeg build:

- a failed picture copy or vmaf_read_pictures() in do_vmaf() and
  do_vmaf_cuda() goes through stop_on_frame(), which logs the frame and the
  error once, records the stop and frees the frame;
- frame_cnt advances only after a successful read;
- uninit() prints no pooled score after a stop or a failed flush, and no
  score line for a model whose pooled score failed.

The device run is ffmpeg-patches/test/check-libvmaf-no-score-after-error.sh.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCHES = ROOT / "ffmpeg-patches"

STOP_HELPER = re.compile(
    r"static int stop_on_frame\(AVFilterContext \*ctx, LIBVMAFContext \*s, AVFrame \*\*dist,"
    r"[^{]*\{[^}]*of frame %u failed \(%s\)[^}]*s->frame_cnt, av_err2str\(err\)\);"
    r"\s*s->stopped = 1;\s*av_frame_free\(dist\);\s*return ret;\s*\}",
    re.S,
)
READ_STOPS = re.compile(
    r"err = vmaf_read_pictures\(s->vmaf, &pic_ref, &pic_dist, s->frame_cnt\);\s*"
    r"if \(err\)\s*return stop_on_frame\(ctx, s, &dist, \"vmaf_read_pictures\", err, "
    r"AVERROR\(EINVAL\)\);\s*s->frame_cnt\+\+;"
)
COPY_STOPS = re.compile(r"return stop_on_frame\(ctx, s, &dist, \"copy")
# do_vmaf() and do_vmaf_cuda() read once and copy twice each.
READ_SITES = 2
COPY_SITES = 4
UPSTREAM_READ_ERROR = '-        av_log(ctx, AV_LOG_ERROR, "problem during vmaf_read_pictures.\\n");'
NO_SCORE_AFTER_STOP = re.compile(
    r"if \(s->stopped\) \{\s*av_log\(ctx, AV_LOG_ERROR,\s*"
    r"\"%s: no pooled score: the filter stopped on the error above\\n\",\s*"
    r"ctx->filter->name\);\s*goto clean_up;\s*\}"
)
NO_SCORE_AFTER_FLUSH = re.compile(
    r"flushing libvmaf after frame %u failed \(%s\); no pooled score\\n\",[^;]*;\s*goto clean_up;"
)
NO_LINE_AFTER_POOL_FAILURE = re.compile(
    r"if \(pool_err\) \{[^}]*err = pool_err;\s*continue;\s*\}", re.S
)


def series_patches() -> list[Path]:
    """The patches of ffmpeg-patches/series.txt, in order."""
    names = (PATCHES / "series.txt").read_text(encoding="utf-8").splitlines()
    return [PATCHES / n.strip() for n in names if n.strip() and not n.startswith("#")]


def filter_changes(patches: list[str]) -> tuple[str, str]:
    """What the patches make of libavfilter/vf_libvmaf.c.

    The first string is every hunk's post-image (context and added lines,
    without the prefix, hunks separated by "@@"); the second is every removed
    line, with its '-'.
    """
    post, removed = [], []
    for patch in patches:
        for section in patch.split("diff --git ")[1:]:
            if not section.startswith("a/libavfilter/vf_libvmaf.c "):
                continue
            for line in section.splitlines():
                if line.startswith(("+++", "---")):
                    continue
                if line.startswith("@@"):
                    post.append("@@")
                elif line.startswith(("+", " ")):
                    post.append(line[1:])
                elif line.startswith("-"):
                    removed.append(line)
    return "\n".join(post), "\n".join(removed)


def problems(added: str, removed: str) -> list[str]:
    """Every way the series' changes to vf_libvmaf.c break the contract.

    `added` is the post-image text of filter_changes().
    """
    found = []
    if not STOP_HELPER.search(added):
        found.append("no stop_on_frame() that logs the frame, records the stop and frees it")
    if len(READ_STOPS.findall(added)) < READ_SITES:
        found.append("a failed vmaf_read_pictures() in libvmaf or libvmaf_cuda does not stop")
    if len(COPY_STOPS.findall(added)) < COPY_SITES:
        found.append("a failed picture copy in libvmaf or libvmaf_cuda does not stop")
    if removed.count(UPSTREAM_READ_ERROR) < READ_SITES:
        found.append("upstream's unnamed vmaf_read_pictures error is still logged")
    if not NO_SCORE_AFTER_STOP.search(added):
        found.append("uninit() prints a pooled score after a stop")
    if not NO_SCORE_AFTER_FLUSH.search(added):
        found.append("uninit() prints a pooled score after a failed flush")
    if not NO_LINE_AFTER_POOL_FAILURE.search(added):
        found.append("uninit() prints a score line after a failed pooled score")
    return found


class LibvmafStopContract(unittest.TestCase):
    def setUp(self) -> None:
        self.patches = [p.read_text(encoding="utf-8") for p in series_patches()]

    def test_series_holds_the_contract(self) -> None:
        self.assertEqual(problems(*filter_changes(self.patches)), [])

    def test_the_series_without_patch_0021_is_refused(self) -> None:
        without = [p for p in self.patches if "VMAFx ADR-1768" not in p]
        self.assertLess(len(without), len(self.patches))
        found = problems(*filter_changes(without))
        for expected in (
            "no stop_on_frame() that logs the frame, records the stop and frees it",
            "a failed vmaf_read_pictures() in libvmaf or libvmaf_cuda does not stop",
            "uninit() prints a pooled score after a stop",
            "uninit() prints a pooled score after a failed flush",
            "uninit() prints a score line after a failed pooled score",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, found)

    def test_a_stop_that_is_not_recorded_is_refused(self) -> None:
        added, removed = filter_changes(self.patches)
        mutated = added.replace(
            "    s->stopped = 1;\n    av_frame_free(dist);", "    av_frame_free(dist);"
        )
        self.assertNotEqual(mutated, added)
        self.assertIn(
            "no stop_on_frame() that logs the frame, records the stop and frees it",
            problems(mutated, removed),
        )

    def test_frame_cnt_advanced_before_the_read_is_refused(self) -> None:
        added, removed = filter_changes(self.patches)
        mutated = added.replace("&pic_dist, s->frame_cnt);", "&pic_dist, s->frame_cnt++);")
        self.assertNotEqual(mutated, added)
        self.assertIn(
            "a failed vmaf_read_pictures() in libvmaf or libvmaf_cuda does not stop",
            problems(mutated, removed),
        )


if __name__ == "__main__":
    unittest.main()
