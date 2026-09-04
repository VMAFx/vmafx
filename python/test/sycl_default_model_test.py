# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Regression test for default model under SYCL backend.

Asserts that running vmaf with the default model under `--backend sycl`
exits 0 without crashing and produces a valid 'vmaf' key in the output.
Skips unless a 2-frame input on SYCL exits 0.
"""

from __future__ import absolute_import

import json
import os
import subprocess
import tempfile
import unittest

from vmaf import ExternalProgram
from vmaf.config import VmafConfig


def _sycl_two_frame_probe():
    ref_path = VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324_2frames.yuv")
    if not os.path.exists(ref_path):
        return False
    cmd = [
        ExternalProgram.vmafexec,
        "--backend",
        "sycl",
        "-r",
        ref_path,
        "-d",
        ref_path,
        "-w",
        "576",
        "-h",
        "324",
        "-p",
        "420",
        "-b",
        "8",
        "--json",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        return proc.returncode == 0
    except Exception:
        return False


@unittest.skipUnless(_sycl_two_frame_probe(), "SYCL device not available or 2-frame probe failed")
class SyclDefaultModelTest(unittest.TestCase):
    """Verify that the default model under --backend sycl runs and emits vmaf."""

    def test_default_model_under_sycl_backend(self):
        ref_path = VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324_2frames.yuv")
        with tempfile.TemporaryDirectory() as tmp:
            out_json = os.path.join(tmp, "out.json")
            cmd = [
                ExternalProgram.vmafexec,
                "--backend",
                "sycl",
                "-r",
                ref_path,
                "-d",
                ref_path,
                "-w",
                "576",
                "-h",
                "324",
                "-p",
                "420",
                "-b",
                "8",
                "--json",
                "-o",
                out_json,
            ]
            res = subprocess.run(cmd, capture_output=True, text=True, check=False)
            self.assertEqual(
                res.returncode,
                0,
                f"vmaf --backend sycl failed with returncode {res.returncode}:\n{res.stderr}",
            )
            with open(out_json, encoding="utf-8") as fh:
                data = json.load(fh)
            self.assertIn("pooled_metrics", data)
            self.assertIn(
                "vmaf",
                data["pooled_metrics"],
                f"'vmaf' key not found in pooled_metrics: {data.get('pooled_metrics')}",
            )
            self.assertIn("frames", data)
            self.assertTrue(len(data["frames"]) > 0)
            self.assertIn("vmaf", data["frames"][0]["metrics"])
            self.assertAlmostEqual(data["pooled_metrics"]["vmaf"]["mean"], 100.0, places=3)


if __name__ == "__main__":
    unittest.main()
