#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify PSNR option parity across CPU, CUDA, SYCL, HIP, and Metal backends."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE_ROOT = ROOT / "core" / "src" / "feature"

GPU_INTEGER_PSNR_SOURCES = (
    "cuda/integer_psnr_cuda.c",
    "sycl/integer_psnr_sycl.cpp",
    "hip/integer_psnr_hip.c",
    "metal/integer_psnr_metal.mm",
)

FLOAT_PSNR_SOURCES = (
    "float_psnr.c",
    "cuda/float_psnr_cuda.c",
    "sycl/float_psnr_sycl.cpp",
    "hip/float_psnr_hip.c",
    "metal/float_psnr_metal.mm",
)


def extract_options_block(source: str) -> str:
    """Extract the options array block from source."""
    match = re.search(r"static\s+const\s+VmafOption\s+\w+(?:\[\d*\])?\s*=\s*\{", source)
    if not match:
        raise AssertionError("could not locate static const VmafOption array")
    start = match.start()
    depth = 0
    for idx in range(match.end() - 1, len(source)):
        if source[idx] == "{":
            depth += 1
        elif source[idx] == "}":
            depth -= 1
            if depth == 0:
                return source[start : idx + 1]
    raise AssertionError("unclosed VmafOption array")


def option_initializer(options_block: str, option_name: str) -> str:
    """Return the braced initializer for a specific named option."""
    matches = list(re.finditer(rf'\.name\s*=\s*"{re.escape(option_name)}"', options_block))
    if len(matches) != 1:
        raise AssertionError(f"expected 1 option entry for {option_name!r}, found {len(matches)}")
    start = options_block.rfind("{", 0, matches[0].start())
    if start < 0:
        raise AssertionError(f"no initializer start for {option_name!r}")
    depth = 0
    for idx in range(start, len(options_block)):
        if options_block[idx] == "{":
            depth += 1
        elif options_block[idx] == "}":
            depth -= 1
            if depth == 0:
                return options_block[start : idx + 1]
    raise AssertionError(f"no initializer end for {option_name!r}")


class GpuPsnrOptionParityContractTest(unittest.TestCase):
    def test_gpu_integer_psnr_options_schema_parity(self) -> None:
        for rel_path in GPU_INTEGER_PSNR_SOURCES:
            with self.subTest(file=rel_path):
                source = (FEATURE_ROOT / rel_path).read_text(encoding="utf-8")
                opts = extract_options_block(source)

                # Check enable_chroma option
                chroma_init = option_initializer(opts, "enable_chroma")
                self.assertIn("VMAF_OPT_TYPE_BOOL", chroma_init)
                self.assertTrue(
                    re.search(r"\.default_val(?:\.b|\s*=\s*\{\s*\.b)?\s*=\s*true", chroma_init),
                    f"{rel_path}: enable_chroma must default to true",
                )

                # Check uncapped option
                uncapped_init = option_initializer(opts, "uncapped")
                self.assertIn("VMAF_OPT_TYPE_BOOL", uncapped_init)
                self.assertTrue(
                    re.search(r"\.default_val(?:\.b|\s*=\s*\{\s*\.b)?\s*=\s*false", uncapped_init),
                    f"{rel_path}: uncapped must default to false",
                )

    def test_gpu_integer_psnr_chroma_guards_and_plane_clamping(self) -> None:
        for rel_path in GPU_INTEGER_PSNR_SOURCES:
            with self.subTest(file=rel_path):
                source = (FEATURE_ROOT / rel_path).read_text(encoding="utf-8")
                self.assertIn("n_planes", source, f"{rel_path} must track n_planes in state")
                self.assertIn(
                    "enable_chroma", source, f"{rel_path} must reference enable_chroma in state"
                )
                self.assertTrue(
                    "VMAF_PIX_FMT_YUV400P" in source,
                    f"{rel_path} must handle YUV400 monochrome format",
                )

    def test_float_psnr_options_parity(self) -> None:
        for rel_path in FLOAT_PSNR_SOURCES:
            with self.subTest(file=rel_path):
                source = (FEATURE_ROOT / rel_path).read_text(encoding="utf-8")
                opts = extract_options_block(source)
                uncapped_init = option_initializer(opts, "uncapped")
                self.assertIn("VMAF_OPT_TYPE_BOOL", uncapped_init)
                self.assertTrue(
                    re.search(r"\.default_val(?:\.b|\s*=\s*\{\s*\.b)?\s*=\s*false", uncapped_init),
                    f"{rel_path}: uncapped must default to false",
                )


if __name__ == "__main__":
    unittest.main()
