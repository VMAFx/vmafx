#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Both NVTX consumers take the CUDA include directory from one discovery.

``core/src/meson.build`` used to pass ``/usr/local/cuda/include`` to the
library-wide include set and again to the NVTX dependency. A toolkit installed
elsewhere configured against one copy and failed on the other. The test holds
the two consumers to the discovered ``nvtx_include_dir`` and pins the probe to
the header the sources actually include.
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MESON = ROOT / "core/src/meson.build"


class NvtxIncludeSingleSource(unittest.TestCase):
    def text(self) -> str:
        return MESON.read_text(encoding="utf-8")

    def test_no_consumer_repeats_the_conventional_include(self) -> None:
        text = self.text()
        self.assertNotIn("is_nvtx_enabled ? '/usr/local/cuda/include'", text)
        self.assertNotIn("include_directories('/usr/local/cuda/include')", text)

    def test_both_consumers_use_the_discovered_directory(self) -> None:
        text = self.text()
        self.assertIn("libvmaf_include_dirs += [nvtx_include_dir]", text)
        self.assertIn("cuda_inc = include_directories(nvtx_include_dir)", text)
        self.assertIn("include_directories: cuda_inc", text)

    def test_the_probe_names_the_header_the_sources_include(self) -> None:
        text = self.text()
        self.assertIn("cc.has_header('nvtx3/nvToolsExt.h')", text)
        self.assertIn("nvcc_for_nvtx.full_path() / '..' / '..' / 'include'", text)


if __name__ == "__main__":
    unittest.main()
