#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep integer-ADM's viewing-geometry rejection aligned across backends."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

FEATURE_ROOT = Path(__file__).resolve().parents[1] / "src" / "feature"
TEST_ROOT = Path(__file__).resolve().parent


def function_body(source: str, name: str) -> str:
    """Return the balanced body of one named function."""
    start = -1
    definition = (
        rf"(?m)^[ \t]*(?:static[ \t]+)?(?:inline[ \t]+)?"
        rf"(?:int|void|char[ \t]*\*)[^;\n]*\b{re.escape(name)}\s*\("
    )
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
                "init_fex_hip",
                "adm_hip_validate(",
                ("adm_hip_rfactors(", "adm_hip_init_device("),
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

    def test_metal_parity_harness_releases_each_resource_once(self) -> None:
        source = (TEST_ROOT / "test_metal_integer_adm_parity.c").read_text(encoding="utf-8")
        cleanup = function_body(source, "release_run_resources")
        self.assertEqual(cleanup.count("vmaf_feature_dictionary_free(&resources->opts)"), 1)
        self.assertEqual(cleanup.count("vmaf_close(resources->vmaf)"), 1)
        self.assertEqual(cleanup.count("vmaf_metal_state_free(&resources->mstate)"), 1)

        registration_helper = function_body(source, "use_feature_options")
        registry_guard = registration_helper.find("vmaf_get_feature_extractor_by_name(")
        handoff = registration_helper.find("*opts = NULL")
        registration = registration_helper.find("vmaf_use_feature(")
        self.assertGreaterEqual(registry_guard, 0)
        self.assertGreater(handoff, registry_guard)
        self.assertGreater(registration, handoff)

        for function in ("run_cpu", "run_metal"):
            with self.subTest(function=function):
                body = function_body(source, function)
                configure = body.find("set_run_options(")
                registration = body.find("use_feature_options(")
                failure_cleanup = body.find("release_run_resources(&resources)", configure)
                self.assertGreaterEqual(configure, 0)
                self.assertGreater(registration, configure)
                self.assertGreater(failure_cleanup, configure)
                self.assertLess(failure_cleanup, registration)
                self.assertEqual(
                    body[configure:registration].count("release_run_resources(&resources)"),
                    1,
                )
                self.assertNotIn("vmaf_feature_dictionary_free", body[registration:])

    def test_metal_unavailable_paths_mark_the_process_skipped(self) -> None:
        source = (TEST_ROOT / "test_metal_integer_adm_parity.c").read_text(encoding="utf-8")
        for function in ("run_metal", "metal_geometry_status"):
            with self.subTest(function=function):
                body = function_body(source, function)
                init = body.find("vmaf_metal_state_init(")
                skipped = body.find("mu_skipped = 1", init)
                unavailable_return = body.find("return NULL", init)
                self.assertGreaterEqual(init, 0)
                self.assertGreater(skipped, init)
                self.assertGreater(unavailable_return, skipped)


if __name__ == "__main__":
    unittest.main()
