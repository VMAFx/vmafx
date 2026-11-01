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
import sys
import tempfile
import unittest
from pathlib import Path

from vmaf.config import VmafConfig

_REPO_ROOT = Path(__file__).resolve().parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from scripts.lib import vmaftest  # noqa: E402 - needs the repository root on sys.path
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402


def _probe_sycl(vmaf):
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
            res = run_command(
                cmd,
                allowed_executables=[cmd[0]],
                capture_output=True,
                text=True,
                timeout_seconds=30,
            )
            return res.returncode == 0 and os.path.exists(out)
        except Exception:
            return False


def _motion_command(vmaf, ref_path, dis_path, width, height, backend, feature, option_str, out):
    return [
        vmaf,
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
        backend,
        "--no_prediction",
        "--feature",
        f"{feature}={option_str}" if option_str else feature,
        "-o",
        out,
        "--json",
    ]


class SyclMotionParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        vmaf = vmaftest.find()
        if vmaf is None:
            raise unittest.SkipTest(vmaftest.MISSING_MESSAGE)
        cls.vmaf = str(vmaf)
        if not _probe_sycl(cls.vmaf):
            raise unittest.SkipTest("SYCL device not available or not built with SYCL support")

    def _run_pair(self, ref_file, dis_file, width, height, option_str="motion_max_val=18.0"):
        ref_path = VmafConfig.test_resource_path("yuv", ref_file)
        dis_path = VmafConfig.test_resource_path("yuv", dis_file)
        if not os.path.exists(ref_path) or not os.path.exists(dis_path):
            raise unittest.SkipTest(f"Missing test fixture: {ref_file} or {dis_file}")

        with tempfile.TemporaryDirectory() as tmp:
            out_cpu = os.path.join(tmp, "cpu.json")
            out_sycl = os.path.join(tmp, "sycl.json")

            cmd_cpu = _motion_command(
                self.vmaf, ref_path, dis_path, width, height, "cpu", "motion", option_str, out_cpu
            )
            cmd_sycl = _motion_command(
                self.vmaf,
                ref_path,
                dis_path,
                width,
                height,
                "sycl",
                "motion_sycl",
                option_str,
                out_sycl,
            )

            run_command(
                cmd_cpu,
                check=True,
                capture_output=True,
                text=True,
                allowed_executables=[cmd_cpu[0]],
                timeout_seconds=600,
            )
            run_command(
                cmd_sycl,
                check=True,
                capture_output=True,
                text=True,
                allowed_executables=[cmd_sycl[0]],
                timeout_seconds=600,
            )

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
