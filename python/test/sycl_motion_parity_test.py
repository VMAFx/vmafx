# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""SYCL integer motion parity tests (ADR-0214, ADR-0219).

Verifies per-frame motion2 parity between the CPU reference and SYCL backend
on the standard test pairs (1080p checkerboard 1-px, 1080p checkerboard 10-px,
and 576x324 src01). Ensures that score clipping with motion_max_val matches
CPU semantics exactly (ADR-0219).
"""

from __future__ import absolute_import

import json
import os
import tempfile
import unittest
from pathlib import Path

from vmaf import ExternalProgram, run_process
from vmaf.config import VmafConfig


def _get_vmaf_cli():
    # Prefer worktree build if present
    worktree_vmaf = Path(__file__).resolve().parents[2] / "core" / "build" / "tools" / "vmaf"
    if worktree_vmaf.exists() and os.access(worktree_vmaf, os.X_OK):
        return str(worktree_vmaf)
    return ExternalProgram.vmafexec


def _probe_sycl():
    vmaf = _get_vmaf_cli()
    if not Path(vmaf).exists() or not os.access(vmaf, os.X_OK):
        return False
    ref = VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324.yuv")
    if not Path(ref).exists():
        return False
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "probe.json"
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
            run_process(cmd, timeout=30)
            return out.exists()
        except (AssertionError, FileNotFoundError, OSError):
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
        if not Path(ref_path).exists() or not Path(dis_path).exists():
            raise unittest.SkipTest(f"Missing test fixture: {ref_file} or {dis_file}")

        with tempfile.TemporaryDirectory() as tmp:
            out_cpu = Path(tmp) / "cpu.json"
            out_sycl = Path(tmp) / "sycl.json"

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

            run_process(cmd_cpu)
            run_process(cmd_sycl)

            with out_cpu.open(encoding="utf-8") as f:
                data_cpu = json.load(f)
            with out_sycl.open(encoding="utf-8") as f:
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

        for i, (fc, fs) in enumerate(zip(cpu_frames, sycl_frames, strict=False)):
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

        for i, (fc, fs) in enumerate(zip(cpu_frames, sycl_frames, strict=False)):
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

        for i, (fc, fs) in enumerate(zip(cpu_frames, sycl_frames, strict=False)):
            c_val = fc["metrics"][metric]
            s_val = fs["metrics"][metric]
            self.assertAlmostEqual(
                c_val, s_val, places=4, msg=f"Frame {i} motion2 drift: CPU={c_val} SYCL={s_val}"
            )


if __name__ == "__main__":
    unittest.main()
