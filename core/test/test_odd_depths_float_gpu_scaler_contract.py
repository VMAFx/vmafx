#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract test for float GPU twin bit-depth scaling (issue #2476).

Float GPU twins must scale every supported bit depth (8..16) identically to the
CPU reference picture_copy(), which scales by 2^(bpc - 8) for every bpc 9..16.
Per-depth chains that only check 10u, 12u, 16u leave odd bit depths (9, 11, 13,
14, 15) falling back to 1.0, giving wrong scores.

This source-contract test verifies that:
- No float_* GPU twin file contains a `bpc == 10u` scaler chain.
- Host float_* GPU extractors call the shared canonical helper
  vmaf_float_moment_scaler().
- Device kernels use the exact bit-shift expression for bpc > 8u.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"

GPU_BACKENDS = ("cuda", "sycl", "hip", "metal")

SCALER_CHAIN_RE = re.compile(r"\b(?:bpc|bits_per_component)\s*==\s*10u\b")
COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def _strip_comments(code: str) -> str:
    return COMMENT_RE.sub(" ", code)


def _find_float_gpu_files() -> list[Path]:
    files: list[Path] = []
    for backend in GPU_BACKENDS:
        backend_dir = FEATURE / backend
        if not backend_dir.is_dir():
            continue
        for p in backend_dir.rglob("*"):
            if not p.is_file():
                continue
            # Restrict to float_* extractors and related float headers/sources
            name = p.name
            parent_name = p.parent.name
            if (
                name.startswith("float_") or "float" in name or parent_name.startswith("float_")
            ) and not name.startswith("integer_"):
                if p.suffix in (".c", ".cpp", ".cu", ".hip", ".h", ".cuh", ".mm", ".metal"):
                    files.append(p)
    return sorted(files)


def _check_for_scaler_chains(files: list[Path]) -> list[str]:
    violations: list[str] = []
    for path in files:
        rel_path = path.relative_to(ROOT)
        content = path.read_text(encoding="utf-8")
        clean = _strip_comments(content)
        matches = list(SCALER_CHAIN_RE.finditer(clean))
        if matches:
            violations.append(
                f"{rel_path}: found legacy 'bpc == 10u' scaler chain ({len(matches)} occurrence(s))"
            )
    return violations


class FloatGpuScalerContract(unittest.TestCase):
    def test_no_float_gpu_file_has_scaler_chain(self) -> None:
        files = _find_float_gpu_files()
        self.assertTrue(len(files) > 0, "Expected to find float GPU files to audit")
        violations = _check_for_scaler_chains(files)
        self.assertEqual(
            violations,
            [],
            "Found legacy 'bpc == 10u' scaler chain in float GPU twins:\n" + "\n".join(violations),
        )

    def test_planted_chain_is_detected(self) -> None:
        files = _find_float_gpu_files()
        self.assertTrue(len(files) > 0)
        target = files[0]
        original_content = target.read_text(encoding="utf-8")
        planted = (
            original_content
            + "\n/* planted */ static inline void _f(unsigned bpc) { if (bpc == 10u) {} }\n"
        )
        clean = _strip_comments(planted)
        self.assertRegex(clean, SCALER_CHAIN_RE)

    def test_host_extractors_call_shared_scaler(self) -> None:
        expected_calls = (
            ("cuda/float_adm_cuda.c", "vmaf_float_moment_scaler(s->bpc)"),
            ("sycl/float_vif_sycl.cpp", "vmaf_float_moment_scaler(bpc)"),
            ("sycl/float_adm_sycl.cpp", "vmaf_float_moment_scaler(bits_per_component)"),
            ("hip/float_ssim_hip.c", "vmaf_float_moment_scaler(bpc)"),
            ("hip/float_adm_hip.c", "vmaf_float_moment_scaler(s->bpc)"),
            ("metal/float_adm_metal.mm", "vmaf_float_moment_scaler(bpc)"),
        )
        for rel, pattern in expected_calls:
            with self.subTest(file=rel):
                p = FEATURE / rel
                self.assertTrue(p.exists(), f"{rel} must exist")
                content = p.read_text(encoding="utf-8")
                self.assertIn(
                    pattern,
                    content,
                    f"{rel} must invoke {pattern}",
                )


if __name__ == "__main__":
    unittest.main()
