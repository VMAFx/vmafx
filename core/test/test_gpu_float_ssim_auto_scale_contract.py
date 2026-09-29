#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin dimension-aware float-SSIM fallback without requiring a GPU.

CUDA, HIP and Metal implement scale 1 only and fall back whenever the resolved
scale is above 1 (ADR-1324). SYCL decimates on the device (ADR-1370) and falls
back only for a geometry its decimation cannot compute exactly.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

CORE_ROOT = Path(__file__).resolve().parents[1]
FEATURE_ROOT = CORE_ROOT / "src" / "feature"

BACKENDS = (
    ("cuda/integer_ssim_cuda.c", "check_context_cuda", "compute_scale", "scale_override"),
    ("hip/float_ssim_hip.c", "check_context_hip", "ssim_hip_compute_scale", "scale_override"),
    ("metal/float_ssim_metal.mm", "check_context_metal", "ssim_metal_compute_scale", "scale"),
)


def function_body(source: str, signature: str) -> str:
    """Return one brace-balanced function body from backend source."""
    body_start = source.index("{", source.index(signature))
    depth = 0
    for offset in range(body_start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[body_start : offset + 1]
    raise AssertionError(f"unterminated function body for {signature}")


def auto_scale(width: int, height: int, override: int = 0) -> int:
    """Mirror the CPU SSIM short-side scale contract."""
    if override > 0:
        return override
    return max(1, int(min(width, height) / 256.0 + 0.5))


class GpuFloatSsimAutoScaleContractTest(unittest.TestCase):
    def test_threshold_cases_cross_the_cpu_auto_scale_boundary(self) -> None:
        self.assertEqual(auto_scale(320, 240), 1)
        self.assertEqual(auto_scale(383, 383), 1)
        self.assertEqual(auto_scale(384, 384), 2)
        self.assertEqual(auto_scale(960, 540), 2)
        self.assertEqual(auto_scale(960, 540, 1), 1)

    def test_scale_one_gpu_twins_enforce_the_same_context_fallback(self) -> None:
        for relative_path, callback, scale_helper, scale_member in BACKENDS:
            with self.subTest(path=relative_path):
                source = (FEATURE_ROOT / relative_path).read_text(encoding="utf-8")
                signature = f"static int {callback}("
                body = function_body(source, signature)
                returns = re.findall(r"\breturn\s+[^;]+;", body)
                self.assertEqual(len(returns), 1)
                self.assertRegex(
                    returns[0],
                    rf"\breturn\s+{re.escape(scale_helper)}\s*\(\s*w\s*,\s*h\s*,"
                    rf"\s*s->{scale_member}\s*\)\s*==\s*1\s*\?\s*0\s*:\s*-ENOTSUP\s*;",
                )
                self.assertRegex(source, rf"\.context_check\s*=\s*{re.escape(callback)}")
                self.assertRegex(source, r'\.context_fallback_name\s*=\s*"float_ssim"')

    def test_sycl_twin_falls_back_only_where_decimation_is_inexact(self) -> None:
        source = (FEATURE_ROOT / "sycl/integer_ssim_sycl.cpp").read_text(encoding="utf-8")
        body = function_body(source, "static int check_context_sycl(")
        returns = re.findall(r"\breturn\s+[^;]+;", body)
        self.assertEqual(len(returns), 1)
        self.assertRegex(
            returns[0],
            r"\breturn\s+float_ssim_geometry_supported\s*\(\s*w\s*,\s*h\s*,\s*scale\s*\)"
            r"\s*\?\s*0\s*:\s*-ENOTSUP\s*;",
        )
        self.assertRegex(body, r"compute_scale\s*\(\s*w\s*,\s*h\s*,\s*s->scale_override\s*\)")
        self.assertNotRegex(body, r"==\s*1\s*\?")
        predicate = function_body(source, "static bool float_ssim_geometry_supported(")
        self.assertIn("SSIM_MAX_EXACT_SCALE", predicate)
        self.assertIn("(unsigned)SSIM_K", predicate)
        self.assertRegex(source, r"\.context_check\s*=\s*check_context_sycl")
        self.assertRegex(source, r'\.context_fallback_name\s*=\s*"float_ssim"')
        # The device plane size is the CPU's own iqa_decimate() rule.
        self.assertIn(
            "iqa_decimate_dim(", function_body(source, "static unsigned decimated_extent(")
        )
        decimate = (FEATURE_ROOT / "iqa/decimate.c").read_text(encoding="utf-8")
        self.assertIn("iqa_decimate_dim(w, factor)", decimate)
        self.assertIn("iqa_decimate_dim(h, factor)", decimate)

    def test_model_dispatch_resolves_before_backend_translation(self) -> None:
        header = (FEATURE_ROOT / "feature_extractor.h").read_text(encoding="utf-8")
        self.assertIn("context_check", header)
        self.assertIn("context_fallback_name", header)
        self.assertIn("allow_context_fallback", header)

        libvmaf = (CORE_ROOT / "src" / "libvmaf.c").read_text(encoding="utf-8")
        model_start = libvmaf.index("int vmaf_use_features_from_model(")
        model_end = libvmaf.index("int vmaf_use_features_from_model_collection(")
        self.assertIn("fex_ctx->allow_context_fallback = true;", libvmaf[model_start:model_end])

        direct_start = libvmaf.index("int vmaf_use_feature(")
        direct_end = libvmaf.index("static unsigned compute_fex_flags(")
        self.assertNotIn("allow_context_fallback = true", libvmaf[direct_start:direct_end])

        read_start = libvmaf.index("int vmaf_read_pictures(")
        read_end = libvmaf.index("#ifdef HAVE_SYCL", read_start)
        read_body = libvmaf[read_start:read_end]
        self.assertIn("resolve_context_fallbacks(vmaf)", read_body)
        self.assertLess(
            read_body.index("resolve_context_fallbacks(vmaf)"),
            read_body.index("read_pictures_frame_translate(vmaf"),
        )


if __name__ == "__main__":
    unittest.main()
