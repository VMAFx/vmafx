#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep integer-ADM's viewing-geometry rejection aligned across backends."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

FEATURE_ROOT = Path(__file__).resolve().parents[1] / "src" / "feature"


def function_body(source: str, name: str) -> str:
    """Return the balanced body of one named function."""
    start = -1
    definition = rf"(?m)^[ \t]*(?:static[ \t]+)?(?:inline[ \t]+)?int[^;\n]*\b{re.escape(name)}\s*\("
    for match in re.finditer(definition, source):
        candidate = source.find("{", match.end())
        declaration = source.find(";", match.end())
        if candidate >= 0 and (declaration < 0 or candidate < declaration):
            start = candidate
            break
    if start < 0:
        raise AssertionError(f"function {name!r} has no body")

    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"function {name!r} has an unterminated body")


class AdmViewingGeometryContractTest(unittest.TestCase):
    def test_shared_guard_preserves_cpu_threshold_without_integer_overflow(self) -> None:
        source = (FEATURE_ROOT / "adm_csf_fixed_point.h").read_text(encoding="utf-8")
        guard_body = function_body(source, "adm_viewing_geometry_check")
        self.assertIn("adm_norm_view_dist * (double)adm_ref_display_height", guard_body)
        self.assertIn("ADM_MIN_VIEWING_GEOMETRY", guard_body)
        self.assertRegex(source, r"#define\s+ADM_MIN_VIEWING_GEOMETRY\s+\(3240\.0\)")

        fixed_body = function_body(source, "adm_csf_fixed_scale")
        guard = fixed_body.find("adm_viewing_geometry_check(")
        self.assertGreaterEqual(guard, 0, "fixed-point conversion must use the shared guard")
        self.assertGreater(fixed_body.find("adm_csf_scale0_fixed("), guard)
        self.assertGreater(fixed_body.find("while (fixed[0] >= limit"), guard)
        self.assertIn("adm_csf_fixed_scale(", function_body(source, "adm_csf_check_scale"))

    def test_backend_config_validation_reaches_shared_fixed_point_guard(self) -> None:
        for relative_path in (
            "cuda/integer_adm_cuda.c",
            "hip/integer_adm_hip.c",
            "sycl/integer_adm_sycl.cpp",
        ):
            with self.subTest(path=relative_path):
                source = (FEATURE_ROOT / relative_path).read_text(encoding="utf-8")
                self.assertIn(
                    "adm_csf_check_scale(",
                    function_body(source, "adm_csf_config_check"),
                )

        metal = (FEATURE_ROOT / "metal/integer_adm_metal.mm").read_text(encoding="utf-8")
        self.assertIn("adm_csf_fixed_scale(", function_body(metal, "compute_i_rfactor"))

    def test_checks_precede_cpu_compute_or_gpu_normalization_and_device_work(self) -> None:
        contracts = (
            ("integer_adm.c", "extract", "adm_viewing_geometry_check(", ("integer_compute_adm(",)),
            (
                "cuda/integer_adm_cuda.c",
                "init_fex_cuda",
                "adm_csf_config_check(",
                ("adm_cuda_init_device(",),
            ),
            (
                "hip/integer_adm_hip.c",
                "adm_hip_validate",
                "adm_csf_config_check(",
                (),
            ),
            (
                "sycl/integer_adm_sycl.cpp",
                "init_fex_sycl",
                "adm_csf_config_check(",
                ("vmaf_sycl_shared_frame_init(",),
            ),
            (
                "metal/integer_adm_metal.mm",
                "init_fex_metal",
                "compute_i_rfactor(",
                ("vmaf_metal_context_new(",),
            ),
        )
        for relative_path, function, guard_call, later_operations in contracts:
            with self.subTest(path=relative_path):
                source = (FEATURE_ROOT / relative_path).read_text(encoding="utf-8")
                body = function_body(source, function)
                guard = body.find(guard_call)
                self.assertGreaterEqual(guard, 0, "geometry validation path is missing")
                for operation in later_operations:
                    self.assertGreater(
                        body.find(operation),
                        guard,
                        f"{operation} must follow the viewing-geometry guard",
                    )


if __name__ == "__main__":
    unittest.main()
