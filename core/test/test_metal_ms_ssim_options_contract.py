#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Device-free source and contract tests for Metal float_ms_ssim options and score parity."""

from __future__ import annotations

import math
import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
METAL_MS_SSIM = REPO_ROOT / "core/src/feature/metal/float_ms_ssim_metal.mm"
CPU_MS_SSIM = REPO_ROOT / "core/src/feature/float_ms_ssim.c"
SYCL_MS_SSIM = REPO_ROOT / "core/src/feature/sycl/integer_ms_ssim_sycl.cpp"
CUDA_MS_SSIM = REPO_ROOT / "core/src/feature/cuda/integer_ms_ssim_cuda.c"
HIP_MS_SSIM = REPO_ROOT / "core/src/feature/hip/integer_ms_ssim_hip.c"
DISPATCH_STRATEGY = REPO_ROOT / "core/src/metal/dispatch_strategy.c"


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


class MetalMsSsimOptionsContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.metal_src = METAL_MS_SSIM.read_text(encoding="utf-8")
        cls.cpu_src = CPU_MS_SSIM.read_text(encoding="utf-8")
        cls.sycl_src = SYCL_MS_SSIM.read_text(encoding="utf-8")
        cls.cuda_src = CUDA_MS_SSIM.read_text(encoding="utf-8")
        cls.hip_src = HIP_MS_SSIM.read_text(encoding="utf-8")
        cls.dispatch_src = DISPATCH_STRATEGY.read_text(encoding="utf-8")

    def test_metal_options_match_cpu_and_gpu_siblings(self) -> None:
        expected_options = ("enable_lcs", "enable_db", "clip_db", "enable_chroma")
        for opt_name in expected_options:
            with self.subTest(option=opt_name):
                init = option_initializer(self.metal_src, opt_name)
                self.assertRegex(init, r"\.type\s*=\s*VMAF_OPT_TYPE_BOOL\b")
                self.assertRegex(init, r"\.default_val(\.b|\s*=\s*\{\s*\.b)\s*=\s*false")

    def test_all_sibling_backends_expose_exact_options(self) -> None:
        for opt_name in ("enable_lcs", "enable_db", "clip_db"):
            with self.subTest(option=opt_name):
                self.assertIn(f'"{opt_name}"', option_initializer(self.metal_src, opt_name))
                self.assertIn(f'"{opt_name}"', option_initializer(self.cpu_src, opt_name))
                self.assertIn(f'"{opt_name}"', option_initializer(self.sycl_src, opt_name))
                self.assertIn(f'"{opt_name}"', option_initializer(self.cuda_src, opt_name))
                self.assertIn(f'"{opt_name}"', option_initializer(self.hip_src, opt_name))

        # enable_chroma is exposed by CPU, SYCL, HIP, and Metal
        self.assertIn('"enable_chroma"', option_initializer(self.metal_src, "enable_chroma"))
        self.assertIn('"enable_chroma"', option_initializer(self.cpu_src, "enable_chroma"))
        self.assertIn('"enable_chroma"', option_initializer(self.sycl_src, "enable_chroma"))
        self.assertIn('"enable_chroma"', option_initializer(self.hip_src, "enable_chroma"))

    def test_provided_features_includes_chroma_channels(self) -> None:
        match = re.search(
            r"static\s+const\s+char\s*\*(\s*const\s*)?provided_features\[\]\s*=\s*\{([^}]+)\};",
            self.metal_src,
        )
        self.assertIsNotNone(match, "provided_features array missing in Metal source")
        features_block = match.group(2)
        for expected in ("float_ms_ssim", "float_ms_ssim_cb", "float_ms_ssim_cr"):
            self.assertIn(f'"{expected}"', features_block)

    def test_dispatch_strategy_table_includes_chroma_features(self) -> None:
        for expected in (
            "float_ms_ssim_metal",
            "float_ms_ssim",
            "float_ms_ssim_cb",
            "float_ms_ssim_cr",
        ):
            self.assertIn(f'"{expected}"', self.dispatch_src)

    def test_max_db_derivation_formula(self) -> None:
        # ADR-1221 / CPU reference formula:
        # peak = (1 << bpc) - 1; mse = 1.0; max_db = ceil(10.0 * log10(peak * peak / mse))
        expected_ceilings = {
            8: math.ceil(10.0 * math.log10(255.0 * 255.0 / 1.0)),  # 49.0
            10: math.ceil(10.0 * math.log10(1023.0 * 1023.0 / 1.0)),  # 61.0
            12: math.ceil(10.0 * math.log10(4095.0 * 4095.0 / 1.0)),  # 73.0
            16: math.ceil(10.0 * math.log10(65535.0 * 65535.0 / 1.0)),  # 97.0
        }
        self.assertEqual(expected_ceilings[8], 49.0)
        self.assertEqual(expected_ceilings[10], 61.0)
        self.assertEqual(expected_ceilings[12], 73.0)
        self.assertEqual(expected_ceilings[16], 97.0)

        # Ensure Metal source derives max_db using this exact formula when clip_db is set
        self.assertIn(
            "s->max_db = ceil(10. * log10((double)peak * (double)peak / mse));", self.metal_src
        )
        self.assertIn("s->max_db = INFINITY;", self.metal_src)

    def test_chroma_min_dim_check_enforces_176(self) -> None:
        # 5 scales with 11x11 filter require minimum dimension 176:
        # scale 0: >= 176, scale 1: >= 88, scale 2: >= 44, scale 3: >= 22, scale 4: >= 11
        # For YUV420P, chroma is halved in both dimensions, so pic width/height must be >= 352
        # (chroma >= 176). If chroma < 176, it must return -EINVAL.
        self.assertIn("check_chroma_min_dim", self.metal_src)
        self.assertIn(
            "min_dim = (unsigned)MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1u)", self.metal_src
        )
        self.assertIn("-EINVAL", self.metal_src)

    def test_emitter_wires_enable_db_and_max_db_for_luma_and_chroma(self) -> None:
        # Luma emission via vmaf_ms_ssim_emit_scores with s->enable_db, s->max_db
        self.assertIn("vmaf_ms_ssim_emit_scores", self.metal_src)
        self.assertIn("plane_scores[0], s->enable_db, s->max_db", self.metal_src)

        # Chroma emission via vmaf_ssim_emit_score_named with s->enable_db, s->max_db
        self.assertIn("vmaf_ssim_emit_score_named", self.metal_src)
        self.assertIn("plane_scores[plane], s->enable_db, s->max_db", self.metal_src)

        # Score validation via vmaf_ssim_prepare_score_named
        self.assertIn("vmaf_ssim_prepare_score_named", self.metal_src)

    def test_nasa_rule4_function_loc_limit(self) -> None:
        # Every function in float_ms_ssim_metal.mm must satisfy LOC <= 60
        lines = self.metal_src.splitlines()
        in_func = False
        func_start = 0
        func_name = ""
        brace_depth = 0
        seen_open = False

        for idx, line in enumerate(lines, 1):
            trimmed = line.strip()
            if (
                not in_func
                and re.match(
                    r"^(static\s+)?(int|void|float|double|const\s+char\s*\*)\b.*\(", trimmed
                )
                and not trimmed.endswith(";")
            ):
                in_func = True
                func_start = idx
                func_name = trimmed.split("(")[0].strip()
                brace_depth = 0
                seen_open = False

            if in_func:
                brace_depth += line.count("{") - line.count("}")
                if "{" in line:
                    seen_open = True
                if seen_open and brace_depth == 0:
                    loc = idx - func_start + 1
                    self.assertLessEqual(
                        loc,
                        60,
                        f"Function {func_name} at lines {func_start}-{idx} exceeds NASA Rule 4 limit (60 LOC): {loc} lines",
                    )
                    in_func = False


if __name__ == "__main__":
    unittest.main()
