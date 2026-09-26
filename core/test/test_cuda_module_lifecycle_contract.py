#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Inventory every CUDA PTX module owner and require context-owned teardown."""

from __future__ import annotations

import re
import unittest
from pathlib import Path
from typing import ClassVar

CUDA_ROOT = Path(__file__).resolve().parents[1] / "src" / "feature" / "cuda"
KERNEL_TEMPLATE = Path(__file__).resolve().parents[1] / "src" / "cuda" / "kernel_template.h"

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

EXPECTED_BUFFER_OWNERS = {
    "float_adm_cuda.c",
    "float_motion_cuda.c",
    "float_psnr_cuda.c",
    "float_vif_cuda.c",
    "integer_adm_cuda.c",
    "integer_cambi_cuda.c",
    "integer_motion_cuda.c",
    "integer_motion_v2_cuda.c",
    "integer_ms_ssim_cuda.c",
    "integer_psnr_hvs_cuda.c",
    "integer_ssim_cuda.c",
    "integer_vif_cuda.c",
    "speed_chroma_cuda.c",
    "speed_temporal_cuda.c",
    "ssim_cuda.c",
    "ssimulacra2_cuda.c",
}

LOAD_RE = re.compile(r"cuModuleLoadData\s*\(\s*&\w+->(?P<field>[A-Za-z0-9_]+)")
RAW_OWNED_TEARDOWN_RE = re.compile(r"(?:->|\.)cu(?:ModuleUnload|StreamDestroy|EventDestroy)\s*\(")
RAW_DEVICE_FREE_RE = re.compile(r"(?:->|\.)cuMemFree\s*\(")
LEGACY_BUFFER_FREE_RE = re.compile(r"vmaf_cuda_buffer_(?:host_)?free\s*\(")
OWNED_BUFFER_FREE_RE = re.compile(r"vmaf_cuda_(?:deviceptr_|buffer_(?:host_)?)free_owned\s*\(")


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
        teardown_sources = [*sorted(CUDA_ROOT.glob("*.c")), KERNEL_TEMPLATE]
        for path in teardown_sources:
            with self.subTest(path=path.name):
                source = path.read_text(encoding="utf-8")
                self.assertIsNone(
                    RAW_OWNED_TEARDOWN_RE.search(source),
                    "raw CUDA module, stream, or event teardown bypasses the owner context",
                )
                self.assertIsNone(
                    RAW_DEVICE_FREE_RE.search(source),
                    "raw CUDA device-buffer free bypasses the owner context",
                )

    def test_buffer_owners_use_retry_safe_owned_helpers(self) -> None:
        actual: set[str] = set()
        for path in sorted(CUDA_ROOT.glob("*.c")):
            source = path.read_text(encoding="utf-8")
            with self.subTest(path=path.name):
                self.assertIsNone(
                    LEGACY_BUFFER_FREE_RE.search(source),
                    "legacy buffer free loses ownership after a later teardown failure",
                )
            if OWNED_BUFFER_FREE_RE.search(source):
                actual.add(path.name)
        self.assertEqual(EXPECTED_BUFFER_OWNERS, actual)

    def test_integer_vif_unwind_preserves_cuda_attribute_error(self) -> None:
        source = (CUDA_ROOT / "integer_vif_cuda.c").read_text(encoding="utf-8")
        self.assertRegex(
            source,
            r"vif_init_unwind\s*\(\s*fex\s*,\s*s\s*,\s*"
            r"vmaf_cuda_result_to_errno\s*\(\s*\(int\)attr_res\s*\)\s*\)",
            "integer VIF must pass the CUDA attribute error into retry-safe unwind",
        )


class MotionForcezeroSourceContractTest(unittest.TestCase):
    """Device-free source-level contract for float/integer_motion_cuda force-zero path.

    These tests verify at the source AST level (regex) that both CUDA motion
    extractors satisfy the following invariants without requiring a CUDA device:

    1. feature_name_dict is constructed *before* the motion_force_zero early
       return — i.e. the dict build call precedes the force-zero branch.
    2. The force-zero branch keeps the close callback active (does NOT null it
       out) so teardown can free the dict.
    3. No CUDA lifecycle call (cuModuleLoadData / kernel_lifecycle_init) is
       reached before the force-zero early return.

    The CPU-only motion score test cannot bind this CUDA source order, so this
    inventory keeps the device-free path and its teardown owner explicit.
    """

    MOTION_FILES: ClassVar[list[str]] = [
        "float_motion_cuda.c",
        "integer_motion_cuda.c",
    ]

    # Regex that matches the vmaf_feature_name_dict_from_provided_features call.
    DICT_BUILD_RE = re.compile(r"vmaf_feature_name_dict_from_provided_features\s*\(")
    # Regex that matches the motion_force_zero early-return guard.
    FORCE_ZERO_RE = re.compile(r"if\s*\(\s*s\s*->\s*motion_force_zero\s*\)")
    # Regex that matches nulling out the close callback (we must NOT see this
    # inside or before the force-zero branch for the dict teardown to work).
    CLOSE_NULL_RE = re.compile(r"fex\s*->\s*close\s*=\s*NULL")
    # Regex for CUDA lifecycle calls that must NOT appear before force-zero.
    CUDA_LIFECYCLE_RE = re.compile(r"vmaf_cuda_kernel_lifecycle_init\s*\(|cuModuleLoadData\s*\(")

    def _init_fex_body(self, source: str) -> str:
        """Extract the body of init_fex_cuda as text."""
        # Find "static int init_fex_cuda(" and capture up to the matching "}"
        start = source.find("static int init_fex_cuda(")
        if start == -1:
            self.fail("init_fex_cuda not found in source")
        # Walk braces to find the end
        depth = 0
        i = source.index("{", start)
        begin = i
        while i < len(source):
            if source[i] == "{":
                depth += 1
            elif source[i] == "}":
                depth -= 1
                if depth == 0:
                    return source[begin : i + 1]
            i += 1
        raise AssertionError("init_fex_cuda body end not found")

    def test_dict_built_before_force_zero_branch(self) -> None:
        for filename in self.MOTION_FILES:
            with self.subTest(file=filename):
                source = (CUDA_ROOT / filename).read_text(encoding="utf-8")
                body = self._init_fex_body(source)
                dict_m = self.DICT_BUILD_RE.search(body)
                fz_m = self.FORCE_ZERO_RE.search(body)
                self.assertIsNotNone(
                    dict_m, f"{filename}: feature_name_dict build not found in init_fex_cuda"
                )
                self.assertIsNotNone(
                    fz_m, f"{filename}: motion_force_zero branch not found in init_fex_cuda"
                )
                self.assertLess(
                    dict_m.start(),
                    fz_m.start(),
                    f"{filename}: feature_name_dict must be built before the motion_force_zero branch",
                )

    def test_close_callback_not_nulled_on_force_zero_path(self) -> None:
        for filename in self.MOTION_FILES:
            with self.subTest(file=filename):
                source = (CUDA_ROOT / filename).read_text(encoding="utf-8")
                body = self._init_fex_body(source)
                # If fex->close = NULL appears anywhere inside init_fex_cuda, the
                # force-zero teardown dict free is broken.
                self.assertIsNone(
                    self.CLOSE_NULL_RE.search(body),
                    f"{filename}: init_fex_cuda must not null the close callback; "
                    "close_fex_cuda frees feature_name_dict on all paths",
                )

    def test_no_cuda_lifecycle_before_force_zero_return(self) -> None:
        for filename in self.MOTION_FILES:
            with self.subTest(file=filename):
                source = (CUDA_ROOT / filename).read_text(encoding="utf-8")
                body = self._init_fex_body(source)
                fz_m = self.FORCE_ZERO_RE.search(body)
                self.assertIsNotNone(fz_m, f"{filename}: motion_force_zero branch not found")
                # Text before the force-zero branch must not contain CUDA lifecycle calls.
                before_fz = body[: fz_m.start()]
                m = self.CUDA_LIFECYCLE_RE.search(before_fz)
                self.assertIsNone(
                    m,
                    f"{filename}: CUDA lifecycle call appears before the motion_force_zero "
                    "early return — force-zero path must be device-free",
                )


if __name__ == "__main__":
    unittest.main()
