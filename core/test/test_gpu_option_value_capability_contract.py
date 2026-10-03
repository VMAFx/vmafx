#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep default-only GPU option capabilities explicit and device-free."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

FEATURE_ROOT = Path(__file__).resolve().parents[1] / "src" / "feature"

DEFAULT_ONLY_OPTIONS = (
    ("cuda/float_vif_cuda.c", "vif_kernelscale"),
    ("sycl/float_vif_sycl.cpp", "vif_kernelscale"),
    ("hip/float_vif_hip.c", "vif_kernelscale"),
    ("cuda/float_adm_cuda.c", "adm_csf_mode"),
    ("sycl/float_adm_sycl.cpp", "adm_csf_mode"),
    ("hip/float_adm_hip.c", "adm_csf_mode"),
    ("metal/float_adm_metal.mm", "adm_csf_mode"),
)

FULL_RANGE_OPTIONS = (
    ("metal/integer_adm_metal.mm", "adm_csf_mode"),
    # ADR-1498: float_vif_metal takes the taps of every kernelscale from
    # vif_get_filter() as kernel arguments, as the CPU computes them.
    ("metal/float_vif_metal.mm", "vif_kernelscale"),
    # ADR-1491: these twins compute the five-frame window (the frame two back
    # on the device, the CPU's window function on the host). A
    # VMAF_OPT_FLAG_DEFAULT_ONLY on the option would send it back to the CPU.
    ("cuda/integer_motion_cuda.c", "motion_five_frame_window"),
    ("cuda/integer_motion_v2_cuda.c", "motion_five_frame_window"),
    ("sycl/integer_motion_sycl.cpp", "motion_five_frame_window"),
    ("sycl/integer_motion_v2_sycl.cpp", "motion_five_frame_window"),
    ("hip/integer_motion_hip.c", "motion_five_frame_window"),
    ("hip/integer_motion_v2_hip.c", "motion_five_frame_window"),
)


def option_initializer(source: str, option: str) -> str:
    """Return the balanced initializer containing one named option."""
    matches = list(re.finditer(rf'\.name\s*=\s*"{re.escape(option)}"', source))
    if len(matches) != 1:
        raise AssertionError(f"expected one {option!r} option, found {len(matches)}")

    start = source.rfind("{", 0, matches[0].start())
    if start < 0:
        raise AssertionError(f"{option!r} has no initializer start")

    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"{option!r} has no initializer end")


class GpuOptionValueCapabilityContractTest(unittest.TestCase):
    def test_restricted_gpu_options_are_marked_default_only(self) -> None:
        for relative_path, option in DEFAULT_ONLY_OPTIONS:
            with self.subTest(path=relative_path, option=option):
                source = (FEATURE_ROOT / relative_path).read_text(encoding="utf-8")
                initializer = option_initializer(source, option)
                self.assertIn("VMAF_OPT_FLAG_FEATURE_PARAM", initializer)
                self.assertIn("VMAF_OPT_FLAG_DEFAULT_ONLY", initializer)

    def test_implemented_gpu_options_are_not_marked_default_only(self) -> None:
        for relative_path, option in FULL_RANGE_OPTIONS:
            with self.subTest(path=relative_path, option=option):
                source = (FEATURE_ROOT / relative_path).read_text(encoding="utf-8")
                initializer = option_initializer(source, option)
                self.assertIn("VMAF_OPT_FLAG_FEATURE_PARAM", initializer)
                self.assertNotIn("VMAF_OPT_FLAG_DEFAULT_ONLY", initializer)


if __name__ == "__main__":
    unittest.main()
