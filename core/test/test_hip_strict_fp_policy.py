#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the HIP strict FP policy at the build level, device-free.

ADR-1407: every HIP kernel is compiled by hipcc with one list,
`hip_strict_fp_args` (contraction off, correctly rounded fp32 division and
square root), so a kernel's arithmetic is the CPU reference's. hipcc's default
for device code is `-ffp-contract=fast`. No AMD device runs in CI, so this
test reads `core/src/meson.build` and `core/test/meson.build` and checks the
load-bearing shapes: the list holds both flags, every HSACO compile gets it,
no per-kernel flag table can exempt a kernel, no kernel source turns
contraction back on, and the device probe of test_hip_fp_arith_contract is
compiled with the same list. Every check has a planted-regression case that
must be detected. `test_hip_fp_arith_contract` is the on-device half.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC_MESON = ROOT / "core" / "src" / "meson.build"
TEST_MESON = ROOT / "core" / "test" / "meson.build"
HIP_FEATURE = ROOT / "core" / "src" / "feature" / "hip"

POLICY_BEGIN = "# BEGIN VMAF HIP strict FP policy"
POLICY_END = "# END VMAF HIP strict FP policy"
POLICY_LIST = (
    "hip_strict_fp_args = ['-ffp-contract=off', '-fhip-fp32-correctly-rounded-divide-sqrt']"
)
KERNEL_COMMAND = "hip_include_flags + hip_strict_fp_args +"
PROBE_COMMAND = "+ hip_strict_fp_args + ['@INPUT@', '-o', '@OUTPUT@'],"
# A pragma or attribute that turns contraction on for part of a kernel.
CONTRACT_ON = re.compile(r"fp\s+contract\s*\(\s*(?:on|fast)\s*\)|-ffp-contract=(?:on|fast)")


def _sources() -> dict[str, str]:
    sources = {
        "src": SRC_MESON.read_text(encoding="utf-8"),
        "test": TEST_MESON.read_text(encoding="utf-8"),
    }
    for path in sorted(HIP_FEATURE.rglob("*")):
        if path.suffix in (".hip", ".h") and path.is_file():
            sources[str(path.relative_to(HIP_FEATURE))] = path.read_text(encoding="utf-8")
    return sources


def _policy(src_meson: str) -> str:
    """The policy block of core/src/meson.build, markers excluded."""
    begin = src_meson.find(POLICY_BEGIN)
    end = src_meson.find(POLICY_END)
    return src_meson[begin:end] if 0 <= begin < end else ""


def _hsaco_targets(src_meson: str) -> list[str]:
    """Every custom_target() that runs hipcc --genco in core/src/meson.build."""
    return re.findall(r"custom_target\('hip_hsaco_'[^)]*?\[hipcc_exe, '--genco'\][^)]*", src_meson)


def _failures(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    src = sources["src"]
    policy = _policy(src)
    if POLICY_LIST not in policy or src.count("hip_strict_fp_args = ") != 1:
        failures.append("src/meson.build: hip_strict_fp_args is not the one two-flag list")
    targets = _hsaco_targets(src)
    if not targets or any(KERNEL_COMMAND not in target for target in targets):
        failures.append("src/meson.build: a HIP kernel is compiled without hip_strict_fp_args")
    if re.search(r"\bhip_cu_extra_flags\b|\bper_kernel_flags = hip", src):
        failures.append("src/meson.build: a per-kernel hipcc flag table is back")
    if sources["test"].count(PROBE_COMMAND) != 1:
        failures.append("test/meson.build: the fp-arith probe is not built with the kernels' list")
    for name, text in sources.items():
        if name not in ("src", "test") and CONTRACT_ON.search(text):
            failures.append(f"{name}: a kernel source turns contraction back on")
    return failures


def _replace(sources: dict[str, str], name: str, old: str, new: str) -> dict[str, str]:
    if old not in sources[name]:
        raise AssertionError(f"planted regression anchor missing in {name}: {old!r}")
    out = dict(sources)
    out[name] = sources[name].replace(old, new, 1)
    return out


class HipStrictFpPolicyTest(unittest.TestCase):
    def assert_detected(self, sources: dict[str, str], needle: str) -> None:
        failures = _failures(sources)
        self.assertTrue(any(needle in item for item in failures), failures)

    def test_live_build_keeps_the_policy(self) -> None:
        self.assertEqual(_failures(_sources()), [])

    def test_every_kernel_source_is_scanned(self) -> None:
        kernels = [name for name in _sources() if name.endswith(".hip")]
        self.assertGreater(len(kernels), 20)

    def test_contracting_list_is_detected(self) -> None:
        sources = _replace(
            _sources(), "src", POLICY_LIST, "hip_strict_fp_args = ['-ffp-contract=fast']"
        )
        self.assert_detected(sources, "not the one two-flag list")

    def test_dropped_division_flag_is_detected(self) -> None:
        sources = _replace(
            _sources(), "src", POLICY_LIST, "hip_strict_fp_args = ['-ffp-contract=off']"
        )
        self.assert_detected(sources, "not the one two-flag list")

    def test_second_definition_is_detected(self) -> None:
        sources = _replace(
            _sources(), "src", POLICY_END, "hip_strict_fp_args = []\n    " + POLICY_END
        )
        self.assert_detected(sources, "not the one two-flag list")

    def test_kernel_built_without_the_list_is_detected(self) -> None:
        sources = _replace(_sources(), "src", KERNEL_COMMAND, "hip_include_flags +")
        self.assert_detected(sources, "compiled without hip_strict_fp_args")

    def test_per_kernel_table_is_detected(self) -> None:
        sources = _replace(
            _sources(),
            "src",
            KERNEL_COMMAND,
            "hip_include_flags + hip_cu_extra_flags.get(name, hip_strict_fp_args) +",
        )
        self.assert_detected(sources, "per-kernel hipcc flag table is back")

    def test_probe_built_with_defaults_is_detected(self) -> None:
        sources = _replace(_sources(), "test", PROBE_COMMAND, "+ ['@INPUT@', '-o', '@OUTPUT@'],")
        self.assert_detected(sources, "not built with the kernels' list")

    def test_contracting_pragma_is_detected(self) -> None:
        sources = _replace(
            _sources(),
            "float_psnr/float_psnr_score.hip",
            "#include <hip/hip_runtime.h>",
            "#include <hip/hip_runtime.h>\n#pragma clang fp contract(fast)",
        )
        self.assert_detected(sources, "turns contraction back on")


if __name__ == "__main__":
    unittest.main()
