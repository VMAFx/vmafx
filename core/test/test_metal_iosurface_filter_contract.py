#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""VideoToolbox frames reach VMAFx as whole frames or are refused (ADR-1679, ADR-2125).

VideoToolbox decodes to bi-planar NV12 and P010. ADR-1679 fixed the retired
libvmaf_metal filter, which imported plane 0 only and passed a frame it could
not import through unscored. The vmafx filter (`ffmpeg-patches/src/vf_vmafx.c`)
replaced it; its Metal zero-copy import is a follow-up of ADR-2125 ("Metal
through the tester bundle"). Until it lands this test holds the filter to:

- the metal backend slot takes VideoToolbox frames and opens no device import:
  such frames are refused with ENOSYS, and the message names import=host;
- import=host downloads the whole frame in the frame's own software format,
  so every plane reaches VMAFx;
- NV12 and P010 are mapped to VMAFx pixel formats;
- no frame passes through unscored on an import refusal.

A failed frame writes no pooled score: test_ffmpeg_vmafx_stop_contract.py.
The library side still holds the ADR-1679 import rules, read here from
core/src/metal: the surface layout table and the three-plane build.
The device side is test_metal_iosurface_import_parity (macOS tester bundle).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FILTER = ROOT / "ffmpeg-patches" / "src" / "vf_vmafx.c"
IMPORT_MM = ROOT / "core" / "src" / "metal" / "picture_import.mm"
HEADER = ROOT / "core" / "include" / "libvmaf" / "libvmaf_metal.h"
LAYOUT = ROOT / "core" / "src" / "metal" / "iosurface_layout.h"

METAL_SLOT = re.compile(r'\{VMAFX_BACKEND_METAL, "metal", AV_PIX_FMT_VIDEOTOOLBOX, NULL, NULL\}')
REFUSAL = re.compile(
    r"if \(!s->slot->open\) \{[^}]*import=host[^}]*return AVERROR\(ENOSYS\);", re.S
)
WHOLE_DOWNLOAD = re.compile(
    r"sw->format = frame_sw_format\(frame\);[^\n]*\n\s*int ret = "
    r"av_hwframe_transfer_data\(sw, frame, 0\);"
)
LAYOUT_ROWS = (
    "{AV_PIX_FMT_NV12, VMAFX_PIXEL_FORMAT_NV12}",
    "{AV_PIX_FMT_P010LE, VMAFX_PIXEL_FORMAT_P010}",
)
PASS_THROUGH = re.compile(r"skipping|non-VideoToolbox frame")
WANT_ALL_PLANES = re.compile(r"const unsigned want = 0x7u;")


def filter_problems(code: str) -> list[str]:
    """Every way the vmafx filter source breaks the VideoToolbox contract."""
    problems = []
    if not METAL_SLOT.search(code):
        problems.append("the metal slot does not take VideoToolbox frames without an import")
    if not REFUSAL.search(code):
        problems.append(
            "a backend without an import does not refuse with ENOSYS naming import=host"
        )
    if not WHOLE_DOWNLOAD.search(code):
        problems.append("import=host does not download the whole frame in its own format")
    for row in LAYOUT_ROWS:
        if row not in code:
            problems.append(f"{row} is not a mapped layout")
    if PASS_THROUGH.search(code):
        problems.append("a frame that fails the import is passed through unscored")
    return problems


class MetalIOSurfaceFilterContract(unittest.TestCase):
    def setUp(self) -> None:
        self.code = FILTER.read_text(encoding="utf-8")

    def test_filter_refuses_or_downloads_whole_frames(self) -> None:
        self.assertEqual(filter_problems(self.code), [])

    def test_a_filter_without_the_refusal_is_refused(self) -> None:
        """Negative: an import attempted with no import function is reported."""
        mutated = self.code.replace("return AVERROR(ENOSYS);", "return 0;")
        self.assertIn(
            "a backend without an import does not refuse with ENOSYS naming import=host",
            filter_problems(mutated),
        )

    def test_a_plane_by_plane_download_is_refused(self) -> None:
        """Negative: a download that does not move the whole frame is reported."""
        mutated = self.code.replace("av_hwframe_transfer_data(sw, frame, 0)", "copy_plane0(sw)")
        self.assertIn(
            "import=host does not download the whole frame in its own format",
            filter_problems(mutated),
        )

    def test_a_dropped_layout_and_a_pass_through_are_refused(self) -> None:
        """Negative: a missing P010 row and a skip-and-pass log line are both reported."""
        mutated = self.code.replace(LAYOUT_ROWS[1], "") + "\n/* skipping frame */\n"
        problems = filter_problems(mutated)
        self.assertIn(f"{LAYOUT_ROWS[1]} is not a mapped layout", problems)
        self.assertIn("a frame that fails the import is passed through unscored", problems)

    def test_import_reads_the_surface_layout(self) -> None:
        source = IMPORT_MM.read_text(encoding="utf-8")
        self.assertIn('#include "iosurface_layout.h"', source)
        self.assertIn("IOSurfaceGetPixelFormat(surf)", source)
        self.assertIn("vmaf_metal_plane_read_plan(", source)
        self.assertIn("vmaf_metal_read_plane(", source)
        self.assertRegex(source, r"if \(fmt == (?:nullptr|NULL)\) \{\s*return -ENOTSUP;")
        # The plane-by-plane memcpy of the surface's own plane index is gone.
        self.assertNotIn("IOSurfaceGetBaseAddressOfPlane(surf, (size_t)plane)", source)

    def test_library_build_requires_all_planes(self) -> None:
        source = IMPORT_MM.read_text(encoding="utf-8")
        self.assertRegex(source, WANT_ALL_PLANES)

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
