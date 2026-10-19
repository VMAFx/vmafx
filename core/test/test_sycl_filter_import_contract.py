#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FFmpeg libvmaf_sycl filter never scores fewer frames than it decoded (ADR-1761).

`ffmpeg-patches/0005-libvmaf-add-libvmaf-sycl-filter.patch` used to log a
warning and pass the frame through unscored when vmaf_sycl_import_va_surface()
failed, so the pooled score covered fewer frames than the input without saying
so; and it imported the reference input's surfaces with the distorted input's
VA display, which with two VA devices names other surfaces (a wrong score) or
none (the import failure the skip hid). This test reads the patch, without a
device or an FFmpeg build:

- a failed import is retried in a bounded loop, then stops the filter with an
  error naming the frame; no import failure passes the frame through;
- each input's surfaces are imported with that input's VA display;
- the software path copies every chroma row of an odd-height frame.

The device run is ffmpeg-patches/test/check-sycl-import-retry.sh.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "ffmpeg-patches" / "0005-libvmaf-add-libvmaf-sycl-filter.patch"

MIN_IMPORT_TRIES = 2
MAX_IMPORT_TRIES = 5
TRIES = re.compile(r"#define LIBVMAF_SYCL_IMPORT_TRIES (\d+)\b")
BOUNDED_LOOP = re.compile(
    r"for \(int attempt = 1; attempt <= LIBVMAF_SYCL_IMPORT_TRIES; attempt\+\+\)"
)
NAMED_ERROR = re.compile(r'"libvmaf_sycl: cannot import the %s VA surface %u of frame %u')
PASS_THROUGH = re.compile(
    r"vmaf_sycl_import_va_surface[^;]*;\s*if \(ret\) \{[^}]*return ff_filter_frame", re.S
)
REF_DISPLAY = re.compile(
    r"import_va_surface_retry\(ctx, s, s->va_display_ref, ref_surface, 1, ref\)"
)
DIST_DISPLAY = re.compile(
    r"import_va_surface_retry\(ctx, s, s->va_display, dist_surface, 0, dist\)"
)
REF_LINK_DISPLAY = re.compile(r"qsv_link_va_display\(ctx, ctx->inputs\[1\], \"reference\"")
CHROMA_ROWS = re.compile(r"const int h = p == 0 \? ref->height : \(ref->height \+ 1\) / 2;")


def added_filter_code(patch: str) -> str:
    """The lines patch 0005 adds to libavfilter/vf_libvmaf.c, without the '+'."""
    section = patch.split("diff --git a/libavfilter/vf_libvmaf.c", 1)[1]
    return "\n".join(line[1:] for line in section.splitlines() if line.startswith("+"))


def problems(code: str) -> list[str]:
    """Every way `code` (the filter's added C) breaks the contract."""
    found = []
    tries = TRIES.search(code)
    if not tries or not MIN_IMPORT_TRIES <= int(tries.group(1)) <= MAX_IMPORT_TRIES:
        found.append("no bounded number of import tries")
    if not BOUNDED_LOOP.search(code):
        found.append("the import is not retried in a bounded loop")
    if not NAMED_ERROR.search(code):
        found.append("a failed import is not reported with its frame")
    if PASS_THROUGH.search(code) or "skipping frame" in code:
        found.append("a frame whose import failed is passed through unscored")
    if not (REF_DISPLAY.search(code) and DIST_DISPLAY.search(code)):
        found.append("the inputs are not imported with their own VA displays")
    if not REF_LINK_DISPLAY.search(code):
        found.append("the reference input's VA display is not read")
    if not CHROMA_ROWS.search(code):
        found.append("the software path drops the last chroma row of an odd height")
    return found


# The filter before ADR-1761 (abridged).
PRE_ADR_1761 = """
        ret = vmaf_sycl_import_va_surface(s->sycl_state, s->va_display,
                                           ref_surface, 1,
                                           ref->width, ref->height, s->bpc);
        if (ret) {
            av_log(ctx, AV_LOG_WARNING,
                   "vmaf_sycl_import_va_surface (ref) failed: %d "
                   "(VA surface may have been freed by decoder) — skipping frame\\n", ret);
            return ff_filter_frame(ctx->outputs[0], dist);
        }
            const int h = p == 0 ? ref->height : ref->height / 2;
"""


class SyclFilterImportContract(unittest.TestCase):
    def test_filter_holds_the_contract(self) -> None:
        code = added_filter_code(PATCH.read_text(encoding="utf-8"))
        self.assertEqual(problems(code), [])

    def test_the_pre_adr_1761_filter_is_refused(self) -> None:
        found = problems(PRE_ADR_1761)
        for expected in (
            "the import is not retried in a bounded loop",
            "a frame whose import failed is passed through unscored",
            "the inputs are not imported with their own VA displays",
            "the software path drops the last chroma row of an odd height",
        ):
            with self.subTest(expected=expected):
                self.assertIn(expected, found)

    def test_an_unbounded_retry_is_refused(self) -> None:
        code = added_filter_code(PATCH.read_text(encoding="utf-8"))
        unbounded = TRIES.sub("#define LIBVMAF_SYCL_IMPORT_TRIES 1000", code)
        self.assertIn("no bounded number of import tries", problems(unbounded))


if __name__ == "__main__":
    unittest.main()
