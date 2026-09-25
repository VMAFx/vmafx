#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Inventory every CUDA PTX module owner and require context-owned teardown."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CUDA_ROOT = Path(__file__).resolve().parents[1] / "src" / "feature" / "cuda"

EXPECTED_MODULES = {
    "float_adm_cuda.c": {"module"},
    "float_motion_cuda.c": {"module"},
    "float_psnr_cuda.c": {"module"},
    "float_vif_cuda.c": {"module"},
    "integer_adm_cuda.c": {
        "adm_dwt_module",
        "adm_csf_module",
        "adm_csf_den_module",
        "adm_cm_module",
    },
    "integer_cambi_cuda.c": {"module"},
    "integer_ciede_cuda.c": {"module"},
    "integer_moment_cuda.c": {"module"},
    "integer_motion_cuda.c": {"module"},
    "integer_motion_v2_cuda.c": {"module"},
    "integer_ms_ssim_cuda.c": {"module"},
    "integer_psnr_cuda.c": {"module"},
    "integer_psnr_hvs_cuda.c": {"module"},
    "integer_ssim_cuda.c": {"module"},
    "integer_vif_cuda.c": {"filter1d_module"},
    "speed_chroma_cuda.c": {"module"},
    "speed_temporal_cuda.c": {"module"},
    "ssim_cuda.c": {"module"},
    "ssimulacra2_cuda.c": {"module_blur", "module_mul"},
}

LOAD_RE = re.compile(r"cuModuleLoadData\s*\(\s*&\w+->(?P<field>[A-Za-z0-9_]+)")
RAW_OWNED_TEARDOWN_RE = re.compile(r"(?:->|\.)cu(?:ModuleUnload|StreamDestroy|EventDestroy)\s*\(")


class CudaModuleLifecycleContractTest(unittest.TestCase):
    def test_load_owner_inventory_is_complete(self) -> None:
        actual: dict[str, set[str]] = {}
        for path in sorted(CUDA_ROOT.glob("*.c")):
            fields = {match.group("field") for match in LOAD_RE.finditer(path.read_text())}
            if fields:
                actual[path.name] = fields
        self.assertEqual(EXPECTED_MODULES, actual)

    def test_all_owners_use_the_context_owned_unload_helper(self) -> None:
        for filename, module_fields in EXPECTED_MODULES.items():
            with self.subTest(path=filename):
                source = (CUDA_ROOT / filename).read_text(encoding="utf-8")
                for field in module_fields:
                    self.assertRegex(
                        source,
                        rf"vmaf_cuda_module_unload\s*\([^;]*&\w+->{re.escape(field)}\s*\)",
                        f"{filename}:{field} does not use the owning-context unload helper",
                    )

    def test_feature_owners_do_not_bypass_context_owned_teardown_helpers(self) -> None:
        for path in sorted(CUDA_ROOT.glob("*.c")):
            with self.subTest(path=path.name):
                source = path.read_text(encoding="utf-8")
                self.assertIsNone(
                    RAW_OWNED_TEARDOWN_RE.search(source),
                    "raw CUDA module, stream, or event teardown bypasses the owner context",
                )

    def test_integer_vif_unwind_preserves_non_allocation_errors(self) -> None:
        source = (CUDA_ROOT / "integer_vif_cuda.c").read_text(encoding="utf-8")
        self.assertIn(
            "return (ret != 0) ? ret : -ENOMEM;",
            source,
            "integer VIF init unwind must not relabel CUDA attribute failures as ENOMEM",
        )


if __name__ == "__main__":
    unittest.main()
