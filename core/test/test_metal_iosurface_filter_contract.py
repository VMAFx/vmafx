#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FFmpeg libvmaf_metal filter hands libvmaf whole frames (ADR-1679).

VideoToolbox decodes to bi-planar NV12 and P010. Before ADR-1679 the filter in
`ffmpeg-patches/0013-libvmaf-add-libvmaf-metal-filter.patch` imported plane 0
only, `vmaf_metal_state_build_pictures()` (core/src/metal/picture_import.mm)
requires all three planes and refused every frame with -EINVAL, and the
import copied a plane without looking at the surface's layout: a plane-1
import would have scored the interleaved CbCr bytes as Cb and a P010 sample
64 times too large. An import failure also passed the frame through unscored.

This test reads the sources, without a device or an FFmpeg build:

- the filter imports planes 0, 1 and 2 of both frames, with no literal plane;
- it accepts NV12 and P010 software formats only, names a refused format, and
  checks both inputs;
- an import failure fails the filter, no frame is passed through unscored, and
  a failed pooled score prints no score line;
- a filter that stopped on a frame prints no pooled score: it would cover
  fewer frames than were decoded (ADR-1761);
- the import reads the surface's pixel format through iosurface_layout.h and
  refuses a layout outside its table with -ENOTSUP;
- the planes the filter imports are the planes build_pictures requires.

The device side is test_metal_iosurface_import_parity (macOS tester bundle).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "ffmpeg-patches" / "0013-libvmaf-add-libvmaf-metal-filter.patch"
IMPORT_MM = ROOT / "core" / "src" / "metal" / "picture_import.mm"
HEADER = ROOT / "core" / "include" / "libvmaf" / "libvmaf_metal.h"
LAYOUT = ROOT / "core" / "src" / "metal" / "iosurface_layout.h"

IMPORT_CALL = re.compile(r"vmaf_metal_picture_import\((.*?)\);", re.S)
PLANE_LOOP = re.compile(r"for \(unsigned plane = 0; plane < 3; plane\+\+\)")
PASS_THROUGH = re.compile(r"skipping|non-VideoToolbox frame")
WANT_ALL_PLANES = re.compile(r"const unsigned want = 0x7u;")
SCORE_SKIPPED_ON_FAILURE = re.compile(r'"problem getting pooled vmaf score\.\\n"\);\s*continue;')
STOP_RECORDED = re.compile(r"if \(ret < 0\) \{\s*s->stopped = 1;")
NO_SCORE_AFTER_STOP = re.compile(
    r"if \(s->stopped\) \{[^}]*no pooled score[^}]*goto clean_up;\s*\}", re.S
)


def added_filter_code(patch: str) -> str:
    """The lines patch 0013 adds to libavfilter/vf_libvmaf.c, without the '+'."""
    section = patch.split("diff --git a/libavfilter/vf_libvmaf.c", 1)[1]
    return "\n".join(line[1:] for line in section.splitlines() if line.startswith("+"))


def filter_problems(code: str) -> list[str]:
    """Every way `code` (the filter's added C) breaks the import contract."""
    problems = []
    calls = IMPORT_CALL.findall(code)
    if not calls:
        problems.append("no vmaf_metal_picture_import call")
    if any(re.search(r"/\*\s*plane\s*=\s*\*/\s*\d", call) for call in calls):
        problems.append("a plane is imported by literal index")
    if not PLANE_LOOP.search(code):
        problems.append("no loop over planes 0, 1 and 2")
    if PASS_THROUGH.search(code):
        problems.append("a frame that fails the import is passed through unscored")
    for fmt in ("AV_PIX_FMT_NV12", "AV_PIX_FMT_P010"):
        if fmt not in code:
            problems.append(f"{fmt} is not an accepted sw_format")
    if not re.search(r"sw_format %s .*?is not\s*\"\s*\"supported", code, re.S):
        problems.append("a refused sw_format is not named in the error")
    if "check_metal_input(ctx, 1," not in code:
        problems.append("the reference input is not checked")
    if not SCORE_SKIPPED_ON_FAILURE.search(code):
        problems.append("a score line is printed after the pooled score failed")
    if not (STOP_RECORDED.search(code) and NO_SCORE_AFTER_STOP.search(code)):
        problems.append("a filter that stopped on a frame still prints a pooled score")
    return problems


# The filter as it was before ADR-1679 (abridged): one plane, pass-through.
PRE_ADR_1679 = """
    ret = vmaf_metal_picture_import(s->metal_state, (uintptr_t)ref_surf,
                                    /*plane=*/0,
                                    ref->width, ref->height, s->bpc,
                                    /*is_ref=*/1, s->frame_cnt);
    if (ret) {
        av_log(ctx, AV_LOG_WARNING,
               "vmaf_metal_picture_import (ref) failed: %d — skipping frame\\n", ret);
        return ff_filter_frame(ctx->outputs[0], dist);
    }
        if (err) {
            av_log(ctx, AV_LOG_ERROR,
                   "problem getting pooled vmaf score.\\n");
        }
        av_log(ctx, AV_LOG_INFO, "VMAF score: %f\\n", vmaf_score);
"""


# The filter before ADR-1761 (abridged): the import fails the filter, and
# uninit_metal() then pools the frames before it and prints their score.
PRE_ADR_1761 = """
    ret = import_metal_frame(ctx, s, ref, 1);
    if (ret >= 0)
        ret = import_metal_frame(ctx, s, dist, 0);
    if (ret < 0) {
        av_frame_free(&dist);
        return ret;
    }
    if (!s->frame_cnt)
        goto clean_up;

    err = vmaf_read_pictures(s->vmaf, NULL, NULL, 0);
"""


class MetalIOSurfaceFilterContract(unittest.TestCase):
    def test_filter_imports_whole_frames(self) -> None:
        code = added_filter_code(PATCH.read_text(encoding="utf-8"))
        self.assertEqual(filter_problems(code), [])

    def test_the_pre_adr_1679_filter_is_refused(self) -> None:
        problems = filter_problems(PRE_ADR_1679)
        self.assertIn("a plane is imported by literal index", problems)
        self.assertIn("no loop over planes 0, 1 and 2", problems)
        self.assertIn("a frame that fails the import is passed through unscored", problems)
        self.assertIn("a score line is printed after the pooled score failed", problems)

    def test_the_pre_adr_1761_filter_is_refused(self) -> None:
        self.assertIn(
            "a filter that stopped on a frame still prints a pooled score",
            filter_problems(PRE_ADR_1761),
        )

    def test_import_reads_the_surface_layout(self) -> None:
        source = IMPORT_MM.read_text(encoding="utf-8")
        self.assertIn('#include "iosurface_layout.h"', source)
        self.assertIn("IOSurfaceGetPixelFormat(surf)", source)
        self.assertIn("vmaf_metal_plane_read_plan(", source)
        self.assertIn("vmaf_metal_read_plane(", source)
        self.assertRegex(source, r"if \(fmt == (?:nullptr|NULL)\) \{\s*return -ENOTSUP;")
        # The plane-by-plane memcpy of the surface's own plane index is gone.
        self.assertNotIn("IOSurfaceGetBaseAddressOfPlane(surf, (size_t)plane)", source)

    def test_filter_planes_are_the_planes_build_requires(self) -> None:
        source = IMPORT_MM.read_text(encoding="utf-8")
        self.assertRegex(source, WANT_ALL_PLANES)
        code = added_filter_code(PATCH.read_text(encoding="utf-8"))
        self.assertRegex(code, PLANE_LOOP)

    def test_header_documents_the_layouts(self) -> None:
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("-ENOTSUP", header)
        self.assertIn("kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange", header)
        layout = LAYOUT.read_text(encoding="utf-8")
        for fourcc in (
            "'4', '2', '0', 'v'",
            "'4', '2', '0', 'f'",
            "'x', '4', '2', '0'",
            "'x', 'f', '2', '0'",
        ):
            self.assertIn(fourcc, layout)


if __name__ == "__main__":
    unittest.main()
