"""The `filters` check of vmafx_filter_check.py refuses a libvmaf-family filter."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from vmafx_filter_check import vmaf_named_filters

HEADER = """Filters:
  T.. = Timeline support
  .S. = Slice threading
  ..C = Command support
  A = Audio input/output
  V = Video input/output
  N = Dynamic number and/or type of input/output
  | = Source or sink filter
"""

FORK_BUILD = HEADER + """ ... vmafmotion        V->V       Calculate the VMAF Motion score.
 ... vmafx             VV->V      Calculate VMAF and other VMAFx metrics of two video streams.
 ... vmafx_pre         V->V       Learned ONNX pre-processing filter run by the VMAFx library.
 ... vmafx_tune        VV->V      Recommend a CRF for the next pass from its VMAF.
"""


class VmafNamedFiltersTest(unittest.TestCase):
    def test_the_fork_build_has_none(self) -> None:
        """Positive: vmafx, vmafx_tune, vmafx_pre and FFmpeg's vmafmotion pass."""
        self.assertEqual(vmaf_named_filters(FORK_BUILD), [])

    def test_a_libvmaf_filter_is_reported(self) -> None:
        """Negative: the upstream libvmaf filters and a fork vmaf_pre are reported."""
        listing = FORK_BUILD + (
            " ... libvmaf           VV->V      Calculate the VMAF between two video streams.\n"
            " ... libvmaf_cuda      VV->V      Calculate the VMAF between two video streams.\n"
            " ... vmaf_pre          V->V       Learned pre-filter.\n"
            # FFmpeg 7 and newer print two flag columns.
            " .. libvmaf_sycl      VV->V      Calculate the VMAF between two video streams.\n"
        )
        self.assertEqual(
            vmaf_named_filters(listing), ["libvmaf", "libvmaf_cuda", "libvmaf_sycl", "vmaf_pre"]
        )

    def test_the_legend_and_empty_output_name_nothing(self) -> None:
        """Boundary: the legend lines and an empty listing hold no filter."""
        self.assertEqual(vmaf_named_filters(HEADER), [])
        self.assertEqual(vmaf_named_filters(""), [])


if __name__ == "__main__":
    unittest.main()
