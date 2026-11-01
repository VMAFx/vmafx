#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The FFmpeg vmafx filter scans every unregistered SEI for Pelorus side data (#2708).

A frame may carry several AV_FRAME_DATA_SEI_UNREGISTERED entries: one per
Pelorus producer, in the blob form or the zero-free carrier form of Pelorus
interop ABI 1.5, and foreign ones. av_frame_get_side_data() returns only the
first, so vf_vmafx.c walks dist->side_data newest first and registers the first
entry vmafx_context_attach_sidedata() accepts. Device-free: reads filter source.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ffmpeg-patches" / "src" / "vf_vmafx.c"

NEWEST_FIRST = re.compile(r"for \(int i = dist->nb_side_data - 1; i >= 0; i--\) \{")
TYPE_FILTER = re.compile(r"sd->type != AV_FRAME_DATA_SEI_UNREGISTERED[^\n]*\n\s*continue;")
STOP_ON_ACCEPT = re.compile(
    r"vmafx_context_attach_sidedata\(s->context,\s*index,\s*sd->data,\s*sd->size,\s*&error\)\s*==\s*VMAFX_OK\)\s*break;"
)
FIRST_ENTRY_ONLY = "av_frame_get_side_data(dist, AV_FRAME_DATA_SEI_UNREGISTERED)"


def problems(code: str) -> list[str]:
    """Every way the filter's side-data read breaks the contract."""
    clean = re.sub(r"/\*.*?\*/", "", code, flags=re.DOTALL)
    found = []
    if not NEWEST_FIRST.search(clean):
        found.append("the side data is not scanned newest first")
    if not TYPE_FILTER.search(clean):
        found.append("entries other than unregistered SEIs are handed to vmafx")
    if not STOP_ON_ACCEPT.search(clean):
        found.append("the scan does not stop at the first accepted entry")
    if FIRST_ENTRY_ONLY in clean:
        found.append("only the first unregistered SEI is read")
    return found


class VmafxPelorusScanContract(unittest.TestCase):
    def setUp(self) -> None:
        self.code = SOURCE.read_text(encoding="utf-8")

    def test_filter_holds_the_contract(self) -> None:
        self.assertEqual(problems(self.code), [])

    def test_first_entry_only_is_refused(self) -> None:
        mutated = self.code.replace(
            "for (int i = dist->nb_side_data - 1; i >= 0; i--) {",
            "const AVFrameSideData *sd = av_frame_get_side_data(dist, AV_FRAME_DATA_SEI_UNREGISTERED); {",
        )
        self.assertNotEqual(mutated, self.code)
        found = problems(mutated)
        self.assertIn("the side data is not scanned newest first", found)
        self.assertIn("only the first unregistered SEI is read", found)

    def test_oldest_first_is_refused(self) -> None:
        mutated = self.code.replace(
            "for (int i = dist->nb_side_data - 1; i >= 0; i--) {",
            "for (int i = 0; i < dist->nb_side_data; i++) {",
        )
        self.assertNotEqual(mutated, self.code)
        self.assertIn("the side data is not scanned newest first", problems(mutated))

    def test_scan_past_an_accepted_entry_is_refused(self) -> None:
        # The contract's own pattern, so the mutation finds the call however clang-format
        # wraps it.
        mutated = STOP_ON_ACCEPT.sub(
            "vmafx_context_attach_sidedata(s->context, index, sd->data, sd->size, &error) == VMAFX_OK)\n            continue;",
            self.code,
        )
        self.assertNotEqual(mutated, self.code)
        self.assertIn("the scan does not stop at the first accepted entry", problems(mutated))


if __name__ == "__main__":
    unittest.main()
