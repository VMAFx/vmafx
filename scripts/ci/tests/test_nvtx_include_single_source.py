#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The NVTX include comes from Meson's CUDA dependency, for both consumers."""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MESON = ROOT / "core/src/meson.build"
MATRIX = ROOT / ".github/workflows/libvmaf-build-matrix.yml"


class NvtxIncludeSingleSource(unittest.TestCase):
    def test_no_conventional_include_is_hardcoded(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        self.assertNotIn("/usr/local/cuda/include", text)
        self.assertNotIn("/opt/cuda/include", text)

    def test_the_toolkit_comes_from_mesons_cuda_dependency(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        self.assertIn("dependency('cuda', required : false)", text)
        self.assertIn("partial_dependency(compile_args : true, includes : true)", text)
        self.assertIn("cc.has_header('nvtx3/nvToolsExt.h'", text)

    def test_both_consumers_share_the_one_dependency(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        self.assertIn("cuda_dependency += [dl_dependency, nvtx_dependency]", text)
        self.assertNotIn("nvtx_include_dir", text)

    def test_the_cpp23_pool_target_takes_the_dependency(self) -> None:
        text = MESON.read_text(encoding="utf-8")
        self.assertIn("dependencies : [pthread_dependency, nvtx_dependency]", text)

    def test_a_cuda_leg_configures_nvtx(self) -> None:
        text = MATRIX.read_text(encoding="utf-8")
        self.assertIn("-Denable_nvtx=true", text)


if __name__ == "__main__":
    unittest.main()
