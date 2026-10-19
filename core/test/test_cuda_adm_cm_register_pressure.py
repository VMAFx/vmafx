#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression test for CUDA ADM CM fatbin register pressure and spill stack (ADR-1226)."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = Path(os.environ.get("VMAFX_DEVICE_DEP_BUILD_DIR", ROOT / "build")).resolve()

RESOURCE_PATTERN = re.compile(
    r"Function\s+([a-zA-Z0-9_]+):\s*\n\s*REG:(\d+)\s+STACK:(\d+)\s+SHARED:(\d+)\s+LOCAL:(\d+)"
)

# Registers per thread, at most, for every architecture of the fatbin. The blanket budget is
# ADR-1226's 208; a kernel listed in KERNEL_BUDGETS has its own measured budget instead, and the
# ADR that sets it names the reason (ADR-2134: the exact 64-bit scale-0 angle-flag sums).
DEFAULT_REGISTER_BUDGET = 208
KERNEL_BUDGETS = {"adm_cm_aim_line_kernel_4": 209}

EXPECTED_KERNELS = {
    "adm_cm_aim_line_kernel_2",
    "adm_cm_aim_line_kernel_4",
    "adm_cm_line_kernel_8",
    "i4_adm_cm_aim_line_kernel_fused",
    "i4_adm_cm_line_kernel_fused",
}


def dump_fatbin_resources(fatbin_path: Path) -> str:
    """Run cuobjdump -res-usage on the provided fatbin file."""
    cuobjdump = shutil.which("cuobjdump")
    if not cuobjdump:
        raise unittest.SkipTest("cuobjdump executable not found in PATH")
    res = subprocess.run(  # noqa: S603 -- cuobjdump resolved by shutil.which, fixed argv
        [cuobjdump, "-res-usage", str(fatbin_path)],
        capture_output=True,
        text=True,
        check=True,
        timeout=15,
    )
    return res.stdout


def verify_kernel_metrics(
    tc: unittest.TestCase,
    fn_name: str,
    reg_s: str,
    stack_s: str,
    local_s: str,
    fatbin_path: Path,
) -> None:
    """Assert zero stack/local spill and bounded registers for a kernel entry."""
    reg = int(reg_s)
    stack = int(stack_s)
    local = int(local_s)

    tc.assertEqual(
        stack,
        0,
        f"Kernel {fn_name} has {stack} B stack spill in {fatbin_path} (expected 0)",
    )
    tc.assertEqual(
        local,
        0,
        f"Kernel {fn_name} has {local} B local spill in {fatbin_path} (expected 0)",
    )
    budget = KERNEL_BUDGETS.get(fn_name, DEFAULT_REGISTER_BUDGET)
    tc.assertLessEqual(
        reg,
        budget,
        f"Kernel {fn_name} uses {reg} registers (expected <= {budget})",
    )


class TestCudaAdmCmRegisterPressure(unittest.TestCase):
    """Verifies that adm_cm.fatbin kernels have 0 spill stack and bounded registers."""

    def test_adm_cm_fatbin_resource_usage(self) -> None:
        fatbin_path = BUILD_DIR / "src" / "adm_cm.fatbin"
        if not fatbin_path.is_file():
            self.skipTest(f"adm_cm.fatbin not found at {fatbin_path}")

        dump_output = dump_fatbin_resources(fatbin_path)

        # Ensure adm_cm_aim_line_kernel_8 is eliminated (ADR-1226)
        self.assertNotIn(
            "Function adm_cm_aim_line_kernel_8",
            dump_output,
            "adm_cm_aim_line_kernel_8 should be replaced by adaptive kernels (ADR-1226)",
        )

        matches = RESOURCE_PATTERN.findall(dump_output)
        self.assertGreater(len(matches), 0, "No kernel functions found in cuobjdump output")

        found_kernels = set()
        for fn_name, reg_s, stack_s, _shared, local_s in matches:
            found_kernels.add(fn_name)
            verify_kernel_metrics(self, fn_name, reg_s, stack_s, local_s, fatbin_path)

        self.assertTrue(
            set(KERNEL_BUDGETS) <= EXPECTED_KERNELS,
            "a per-kernel budget names a kernel the test does not expect",
        )
        self.assertTrue(
            EXPECTED_KERNELS.issubset(found_kernels),
            f"Expected kernels {EXPECTED_KERNELS} but found {found_kernels}",
        )


if __name__ == "__main__":
    unittest.main()
