#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The NVTX include comes from Meson's CUDA dependency, for both consumers."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MESON = ROOT / "core/src/meson.build"
MATRIX = ROOT / ".github/workflows/libvmaf-build-matrix.yml"


def meson_block(text: str, variable: str) -> str:
    """Return the full static_library() call assigned to ``variable``."""
    match = re.search(
        rf"(?ms)^{re.escape(variable)} = static_library\(\n.*?^\)\n",
        text,
    )
    if match is None:
        raise AssertionError(f"{variable} has no static_library() block")
    return match.group(0)


def matrix_row(text: str, name: str) -> str:
    """Return the YAML list item whose ``name:`` field is exactly ``name``."""
    lines = text.splitlines()
    try:
        index = next(number for number, line in enumerate(lines) if line.strip() == f"name: {name}")
    except StopIteration as exc:
        raise AssertionError(f"matrix row {name!r} not found") from exc

    start = index
    while start > 0 and not lines[start].strip().startswith("- os:"):
        start -= 1
    end = index + 1
    while end < len(lines) and not lines[end].strip().startswith("- os:"):
        end += 1
    return "\n".join(lines[start:end])


class NvtxIncludeSingleSource(unittest.TestCase):
    def test_no_conventional_include_is_hardcoded(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        self.assertNotIn("/usr/local/cuda/include", text)
        self.assertNotIn("/opt/cuda/include", text)

    def test_the_toolkit_comes_from_mesons_cuda_dependency(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        self.assertIn("dependency('cuda', required : false)", text)
        self.assertIn("nvcc = find_program('nvcc', required : false)", text)
        self.assertIn(
            "nvtx_dependency = declare_dependency(include_directories : include_directories(nvtx_include_dir))",
            text,
        )
        self.assertNotIn("cuda_dep.partial_dependency", text)
        self.assertNotIn("get_compile_args()", text)
        self.assertNotIn("get_link_args()", text)
        self.assertIn("cc.has_header('nvtx3/nvToolsExt.h'", text)

    def test_both_consumers_share_the_one_dependency(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        self.assertIn("cuda_dependency += [dl_dependency, nvtx_dependency]", text)
        self.assertEqual(text.count("nvtx_dependency = declare_dependency("), 1)
        self.assertNotIn("nvtx_dependency = cuda_dep", text)

    def test_the_cpp23_pool_target_takes_the_dependency(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        block = meson_block(text, "gpu_picture_pool_cpp23_lib")
        self.assertIn("dependencies : [pthread_dependency, nvtx_dependency]", block)

    def test_the_other_cpp23_pool_target_does_not_take_the_dependency(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        block = meson_block(text, "picture_pool_cpp23_lib")
        self.assertNotIn("nvtx_dependency", block)

    def test_a_cuda_leg_configures_nvtx(self) -> None:
        text = MATRIX.read_text(encoding="utf-8")
        row = matrix_row(text, "Ubuntu CUDA")
        self.assertIn(
            "meson_extra: -Denable_cuda=true -Denable_nvcc=true -Denable_nvtx=true",
            row,
        )
        self.assertEqual(text.count("-Denable_nvtx=true"), 1)


if __name__ == "__main__":
    unittest.main()
