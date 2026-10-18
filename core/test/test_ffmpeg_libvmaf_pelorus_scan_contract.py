#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FFmpeg libvmaf filter scans every unregistered SEI for Pelorus side data (#2708).

A frame may carry several AV_FRAME_DATA_SEI_UNREGISTERED entries: one per
Pelorus producer, in the blob form or the zero-free carrier form of Pelorus
interop ABI 1.5, and foreign ones. av_frame_get_side_data() returns only the
first, so patch 0017 walks dist->side_data newest first and registers the first
entry vmaf_set_perceptual_sidedata() accepts. This test reads the series,
without a device or an FFmpeg build.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_ffmpeg_libvmaf_stop_contract import filter_changes, series_patches

NEWEST_FIRST = re.compile(r"for \(i = dist->nb_side_data - 1; i >= 0; i--\) \{")
TYPE_FILTER = re.compile(r"sd->type != AV_FRAME_DATA_SEI_UNREGISTERED[^\n]*\n\s*continue;")
STOP_ON_ACCEPT = re.compile(
    r"pw_err = vmaf_set_perceptual_sidedata\(s->vmaf, sd->data, sd->size,\s*s->frame_cnt\);"
    r"\s*if \(!pw_err\)\s*break;"
)
FIRST_ENTRY_ONLY = "av_frame_get_side_data(dist, AV_FRAME_DATA_SEI_UNREGISTERED)"


def problems(added: str) -> list[str]:
    """Every way the series' side-data read breaks the contract."""
    found = []
    if not NEWEST_FIRST.search(added):
        found.append("the side data is not scanned newest first")
    if not TYPE_FILTER.search(added):
        found.append("entries other than unregistered SEIs are handed to libvmaf")
    if not STOP_ON_ACCEPT.search(added):
        found.append("the scan does not stop at the first accepted entry")
    if FIRST_ENTRY_ONLY in added:
        found.append("only the first unregistered SEI is read")
    return found


class LibvmafPelorusScanContract(unittest.TestCase):
    def setUp(self) -> None:
        patches = [p.read_text(encoding="utf-8") for p in series_patches()]
        self.added = filter_changes(patches)[0]

    def test_series_holds_the_contract(self) -> None:
        self.assertEqual(problems(self.added), [])

    def test_first_entry_only_is_refused(self) -> None:
        mutated = self.added.replace(
            "for (i = dist->nb_side_data - 1; i >= 0; i--) {",
            "sd = av_frame_get_side_data(dist, AV_FRAME_DATA_SEI_UNREGISTERED); {",
        )
        self.assertNotEqual(mutated, self.added)
        found = problems(mutated)
        self.assertIn("the side data is not scanned newest first", found)
        self.assertIn("only the first unregistered SEI is read", found)

    def test_oldest_first_is_refused(self) -> None:
        mutated = self.added.replace(
            "for (i = dist->nb_side_data - 1; i >= 0; i--) {",
            "for (i = 0; i < dist->nb_side_data; i++) {",
        )
        self.assertNotEqual(mutated, self.added)
        self.assertIn("the side data is not scanned newest first", problems(mutated))

    def test_scan_past_an_accepted_entry_is_refused(self) -> None:
        mutated = re.sub(
            r"if \(!pw_err\)\s*break;", "if (!pw_err)\n                continue;", self.added
        )
        self.assertNotEqual(mutated, self.added)
        self.assertIn("the scan does not stop at the first accepted entry", problems(mutated))


if __name__ == "__main__":
    unittest.main()
