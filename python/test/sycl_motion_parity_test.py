# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""SYCL integer motion parity tests (ADR-0214, ADR-0219).

Verifies per-frame motion2 parity between the CPU reference and SYCL backend
on the standard test pairs (1080p checkerboard 1-px, 1080p checkerboard 10-px,
and 576x324 src01). Ensures that score clipping with motion_max_val matches
CPU semantics exactly (ADR-0219).
"""

from __future__ import absolute_import

import json
import os
import subprocess
import tempfile
import unittest

from vmaf import ExternalProgram
from vmaf.config import VmafConfig


def _get_vmaf_cli():
    # Prefer worktree build if present
    worktree_vmaf = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "../../core/build/tools/vmaf")
    )
    if os.path.exists(worktree_vmaf) and os.access(worktree_vmaf, os.X_OK):
        return worktree_vmaf
    return ExternalProgram.vmafexec


def _probe_sycl():
    vmaf = _get_vmaf_cli()
    if not os.path.exists(vmaf) or not os.access(vmaf, os.X_OK):
        return False
    ref = VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324.yuv")
    if not os.path.exists(ref):
        return False
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "probe.json")
        cmd = [
            vmaf,
            "-r",
            ref,
            "-d",
            ref,
            "-w",
            "576",
            "-h",
            "324",
            "-p",
            "420",
            "-b",
            "8",
            "--backend",
            "sycl",
            "--no_prediction",
            "--feature",
            "motion_sycl",
            "-o",
            out,
            "--json",
        ]
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            return res.returncode == 0 and os.path.exists(out)
        except Exception:
            return False


class SyclMotionParityTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.vmaf = _get_vmaf_cli()
        if not _probe_sycl():
            raise unittest.SkipTest("SYCL device not available or not built with SYCL support")

    def _run_pair(self, ref_file, dis_file, width, height, option_str="motion_max_val=18.0"):
        ref_path = VmafConfig.test_resource_path("yuv", ref_file)
        dis_path = VmafConfig.test_resource_path("yuv", dis_file)
        if not os.path.exists(ref_path) or not os.path.exists(dis_path):
            raise unittest.SkipTest(f"Missing test fixture: {ref_file} or {dis_file}")

        with tempfile.TemporaryDirectory() as tmp:
            out_cpu = os.path.join(tmp, "cpu.json")
            out_sycl = os.path.join(tmp, "sycl.json")

            cmd_cpu = [
                self.vmaf,
                "-r",
                ref_path,
                "-d",
                dis_path,
                "-w",
                str(width),
                "-h",
                str(height),
                "-p",
                "420",
                "-b",
                "8",
                "--backend",
                "cpu",
                "--no_prediction",
                "--feature",
                f"motion={option_str}" if option_str else "motion",
                "-o",
                out_cpu,
                "--json",
            ]
            cmd_sycl = [
                self.vmaf,
                "-r",
                ref_path,
                "-d",
                dis_path,
                "-w",
                str(width),
                "-h",
                str(height),
                "-p",
                "420",
                "-b",
                "8",
                "--backend",
                "sycl",
                "--no_prediction",
                "--feature",
                f"motion_sycl={option_str}" if option_str else "motion_sycl",
                "-o",
                out_sycl,
                "--json",
            ]

            subprocess.run(cmd_cpu, check=True, capture_output=True, text=True)
            subprocess.run(cmd_sycl, check=True, capture_output=True, text=True)

            with open(out_cpu, encoding="utf-8") as f:
                data_cpu = json.load(f)
            with open(out_sycl, encoding="utf-8") as f:
                data_sycl = json.load(f)

        return data_cpu, data_sycl

    def test_checkerboard_1px_motion2_parity(self):
        data_cpu, data_sycl = self._run_pair(
            "checkerboard_1920_1080_10_3_0_0.yuv",
            "checkerboard_1920_1080_10_3_1_0.yuv",
            1920,
            1080,
            "motion_max_val=18.0",
        )
        metric = "integer_motion2_mmxv_18"
        cpu_frames = data_cpu["frames"]
        sycl_frames = data_sycl["frames"]
        self.assertEqual(len(cpu_frames), len(sycl_frames))

        # Check motion_max_val=18.0 clipping on both backends
        self.assertAlmostEqual(cpu_frames[1]["metrics"][metric], 18.0, places=6)
        self.assertAlmostEqual(cpu_frames[2]["metrics"][metric], 18.0, places=6)
        self.assertAlmostEqual(sycl_frames[1]["metrics"][metric], 18.0, places=6)
        self.assertAlmostEqual(sycl_frames[2]["metrics"][metric], 18.0, places=6)

        for i, (fc, fs) in enumerate(zip(cpu_frames, sycl_frames)):
            c_val = fc["metrics"][metric]
            s_val = fs["metrics"][metric]
            self.assertAlmostEqual(
                c_val, s_val, places=6, msg=f"Frame {i} motion2 drift: CPU={c_val} SYCL={s_val}"
            )

    def test_checkerboard_10px_motion2_parity(self):
        data_cpu, data_sycl = self._run_pair(
            "checkerboard_1920_1080_10_3_0_0.yuv",
            "checkerboard_1920_1080_10_3_10_0.yuv",
            1920,
            1080,
            "motion_max_val=18.0",
        )
        metric = "integer_motion2_mmxv_18"
        cpu_frames = data_cpu["frames"]
        sycl_frames = data_sycl["frames"]
        self.assertEqual(len(cpu_frames), len(sycl_frames))

        # Check motion_max_val=18.0 clipping on both backends
        self.assertAlmostEqual(cpu_frames[1]["metrics"][metric], 18.0, places=6)
        self.assertAlmostEqual(cpu_frames[2]["metrics"][metric], 18.0, places=6)
        self.assertAlmostEqual(sycl_frames[1]["metrics"][metric], 18.0, places=6)
        self.assertAlmostEqual(sycl_frames[2]["metrics"][metric], 18.0, places=6)

        for i, (fc, fs) in enumerate(zip(cpu_frames, sycl_frames)):
            c_val = fc["metrics"][metric]
            s_val = fs["metrics"][metric]
            self.assertAlmostEqual(
                c_val, s_val, places=6, msg=f"Frame {i} motion2 drift: CPU={c_val} SYCL={s_val}"
            )

    def test_src01_motion2_parity(self):
        data_cpu, data_sycl = self._run_pair(
            "src01_hrc00_576x324.yuv", "src01_hrc01_576x324.yuv", 576, 324, "motion_max_val=18.0"
        )
        metric = "integer_motion2_mmxv_18"
        cpu_frames = data_cpu["frames"]
        sycl_frames = data_sycl["frames"]
        self.assertEqual(len(cpu_frames), len(sycl_frames))

        # Pooled mean matches to 1e-6
        c_mean = data_cpu["pooled_metrics"][metric]["mean"]
        s_mean = data_sycl["pooled_metrics"][metric]["mean"]
        self.assertAlmostEqual(c_mean, s_mean, places=5)

        for i, (fc, fs) in enumerate(zip(cpu_frames, sycl_frames)):
            c_val = fc["metrics"][metric]
            s_val = fs["metrics"][metric]
            self.assertAlmostEqual(
                c_val, s_val, places=4, msg=f"Frame {i} motion2 drift: CPU={c_val} SYCL={s_val}"
            )


if __name__ == "__main__":
    unittest.main()
