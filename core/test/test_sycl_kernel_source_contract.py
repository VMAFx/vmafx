#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Protect fp64-free SpEED kernels and explicit SYCL output captures."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYCL_ROOT = ROOT / "core" / "src" / "feature" / "sycl"

SPEED_DEVICE_PREFIXES = {
    "speed_chroma_sycl.cpp": "struct SpeedChromaSyclState",
    "speed_temporal_sycl.cpp": "struct SpeedTemporalSyclState",
}
SPEED_ROLE_LAUNCHERS = {
    "speed_chroma_sycl.cpp": ("launch_chroma_indterm", "launch_chroma_score"),
    "speed_temporal_sycl.cpp": ("launch_temporal_indterm", "launch_temporal_score"),
}
EXPECTED_LAUNCHER_COUNT = 2  # definition and single call site
MOMENT_OUTPUT_COUNT = 4


def _sources() -> dict[str, str]:
    names = (
        *SPEED_DEVICE_PREFIXES,
        "float_psnr_sycl.cpp",
        "integer_psnr_sycl.cpp",
        "integer_moment_sycl.cpp",
    )
    return {name: (SYCL_ROOT / name).read_text(encoding="utf-8") for name in names}


def _contract_failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for name, host_state_marker in SPEED_DEVICE_PREFIXES.items():
        source = sources[name]
        device_prefix, separator, _host_suffix = source.partition(host_state_marker)
        if not separator:
            failures.append(f"{name}: missing host-state boundary {host_state_marker}")
            continue
        if re.search(r"\bdouble\b", device_prefix):
            failures.append(f"{name}: double appears in the device-kernel region")
        for launcher in SPEED_ROLE_LAUNCHERS[name]:
            if source.count(f"{launcher}(") != EXPECTED_LAUNCHER_COUNT:
                failures.append(f"{name}: missing unique kernel launcher {launcher}")
        if re.search(r"\blaunch_(?:indterm|score)\(", source):
            failures.append(f"{name}: ambiguous cross-TU kernel launcher name")

    float_psnr = sources["float_psnr_sycl.cpp"]
    if "FpsnrOutput output" not in float_psnr or "output.partials" not in float_psnr:
        failures.append("float_psnr_sycl.cpp: output pointer lacks its kernel-argument struct")

    integer_psnr = sources["integer_psnr_sycl.cpp"]
    if "PsnrKernelArgs args" not in integer_psnr or "*args.sse" not in integer_psnr:
        failures.append("integer_psnr_sycl.cpp: output pointer lacks its kernel-argument struct")

    moment = sources["integer_moment_sycl.cpp"]
    if "int64_t *const e_sums = d_sums;" not in moment:
        failures.append("integer_moment_sycl.cpp: missing explicit e_sums capture alias")
    if moment.count("atomic64(e_sums[") != MOMENT_OUTPUT_COUNT or "atomic64(d_sums[" in moment:
        failures.append("integer_moment_sycl.cpp: kernel uses the raw d_sums parameter")
    return failures


class SyclKernelSourceContractTest(unittest.TestCase):
    def test_live_sources_keep_fp32_and_capture_contracts(self) -> None:
        self.assertEqual(_contract_failures(_sources()), [])

    def test_fp64_speed_regression_is_detected(self) -> None:
        sources = _sources()
        marker = SPEED_DEVICE_PREFIXES["speed_temporal_sycl.cpp"]
        sources["speed_temporal_sycl.cpp"] = sources["speed_temporal_sycl.cpp"].replace(
            marker, "double local_sum = 0.0;\n" + marker, 1
        )
        self.assertTrue(any("device-kernel" in item for item in _contract_failures(sources)))

    def test_raw_moment_capture_regression_is_detected(self) -> None:
        sources = _sources()
        moment = sources["integer_moment_sycl.cpp"].replace(
            "    int64_t *const e_sums = d_sums;\n", "", 1
        )
        sources["integer_moment_sycl.cpp"] = moment.replace("e_sums", "d_sums")
        failures = _contract_failures(sources)
        self.assertTrue(any("raw d_sums" in item for item in failures))

    def test_colliding_speed_kernel_name_is_detected(self) -> None:
        sources = _sources()
        sources["speed_chroma_sycl.cpp"] = sources["speed_chroma_sycl.cpp"].replace(
            "launch_chroma_score", "launch_score"
        )
        failures = _contract_failures(sources)
        self.assertTrue(any("ambiguous cross-TU" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
