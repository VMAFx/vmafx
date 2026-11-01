# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

import json
import os
import shutil
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


def _run(cmd, timeout_seconds=600):
    """Run cmd with its own executable as the only one allowed, capturing text output."""
    return run_command(
        cmd,
        allowed_executables=[cmd[0]],
        capture_output=True,
        text=True,
        timeout_seconds=timeout_seconds,
    )


def _nvidia_gpu_answers():
    nvidia_smi = shutil.which("nvidia-smi")
    if nvidia_smi is None:
        return False
    return _run([nvidia_smi], timeout_seconds=60).returncode == 0


def _probe_cuda(vmaf_bin, ref_yuv):
    """True when an NVIDIA GPU answers and ``vmaf_bin`` scores one frame with --backend cuda.

    A binary built without CUDA refuses the backend (exit 100, ADR-0543), so
    a CPU build under test skips these tests instead of failing them.
    """
    if not _nvidia_gpu_answers():
        return False
    with tempfile.TemporaryDirectory() as tmp:
        probe = [
            vmaf_bin,
            *("-r", ref_yuv, "-d", ref_yuv, "-w", "576", "-h", "324", "-p", "420", "-b", "8"),
            *("--backend", "cuda", "--no_prediction", "--feature", "psnr", "--frame_cnt", "1"),
            *("--json", "-o", os.path.join(tmp, "probe.json")),
        ]
        res = _run(probe, timeout_seconds=120)
    return res.returncode == 0


class CudaDefaultModelTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The vmaf under test (scripts/lib/vmaftest.py). Its build
        # directories are CPU builds: name a CUDA build with VMAF_BIN.
        vmaf_bin = vmaftest.find()
        if vmaf_bin is None:
            raise unittest.SkipTest(vmaftest.MISSING_MESSAGE)
        cls.vmaf_bin = str(vmaf_bin)

        cls.ref_yuv = VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324.yuv")
        cls.dis_yuv = VmafConfig.test_resource_path("yuv", "src01_hrc01_576x324.yuv")
        if not os.path.isfile(cls.ref_yuv) or not os.path.isfile(cls.dis_yuv):
            raise unittest.SkipTest("Required test YUV video files not found")
        if not _probe_cuda(cls.vmaf_bin, cls.ref_yuv):
            raise unittest.SkipTest(
                "no NVIDIA GPU on this host, or the vmaf under test was built without CUDA"
            )

    def _build_parity_command(self, backend_options, out_json):
        return [
            self.vmaf_bin,
            "--reference",
            self.ref_yuv,
            "--distorted",
            self.dis_yuv,
            "--width",
            "576",
            "--height",
            "324",
            "--pixel_format",
            "420",
            "--bitdepth",
            "8",
            *backend_options,
            "--model",
            "version=vmaf_v1.0.16_3d0h",
            "--json",
            "--output",
            out_json,
        ]

    def test_cuda_default_model_exit_zero_and_pooled_metrics(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            out_json = f.name
        try:
            cmd = [
                self.vmaf_bin,
                "--reference",
                self.ref_yuv,
                "--distorted",
                self.dis_yuv,
                "--width",
                "576",
                "--height",
                "324",
                "--pixel_format",
                "420",
                "--bitdepth",
                "8",
                "--backend",
                "cuda",
                "--model",
                "version=vmaf_v1.0.16_3d0h",
                "--json",
                "--output",
                out_json,
            ]
            res = _run(cmd)
            self.assertEqual(res.returncode, 0, f"CUDA vmaf run failed: {res.stderr}\n{res.stdout}")

            with open(out_json, "r", encoding="utf-8") as jf:
                data = json.load(jf)

            pooled = data.get("pooled_metrics", {})
            self.assertIn("vmaf", pooled, "Pooled metrics missing 'vmaf' key")
            self.assertIsNotNone(pooled["vmaf"].get("mean"), "vmaf mean score is None")
            vmaf_score = pooled["vmaf"]["mean"]
            self.assertGreater(vmaf_score, 0.0)
            self.assertLessEqual(vmaf_score, 100.0)
        finally:
            if os.path.exists(out_json):
                os.remove(out_json)

    def test_cuda_cpu_parity_default_model(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f_cuda:
            cuda_json = f_cuda.name
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f_cpu:
            cpu_json = f_cpu.name

        try:
            cmd_cuda = self._build_parity_command(["--backend", "cuda"], cuda_json)
            res_cuda = _run(cmd_cuda)
            self.assertEqual(res_cuda.returncode, 0, f"CUDA run failed: {res_cuda.stderr}")

            cmd_cpu = self._build_parity_command(["--no_cuda"], cpu_json)
            res_cpu = _run(cmd_cpu)
            self.assertEqual(res_cpu.returncode, 0, f"CPU run failed: {res_cpu.stderr}")

            with open(cuda_json, "r", encoding="utf-8") as f:
                cuda_data = json.load(f)
            with open(cpu_json, "r", encoding="utf-8") as f:
                cpu_data = json.load(f)

            cuda_pooled = cuda_data["pooled_metrics"]
            cpu_pooled = cpu_data["pooled_metrics"]

            # adm3 is dispatched to CPU due to missing adm_csf_mode on CUDA twin -> exact match
            adm3_metric = "integer_adm3_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02"
            self.assertIn(adm3_metric, cuda_pooled)
            self.assertIn(adm3_metric, cpu_pooled)
            self.assertAlmostEqual(
                cuda_pooled[adm3_metric]["mean"],
                cpu_pooled[adm3_metric]["mean"],
                places=5,
                msg="adm3 CPU-dispatched score should match CPU run",
            )

            # cambi CUDA twin parity (< 1e-2)
            cambi_metric = "cambi_hrs_1080_cmxv_17_vlt_0.06"
            if cambi_metric in cuda_pooled and cambi_metric in cpu_pooled:
                self.assertAlmostEqual(
                    cuda_pooled[cambi_metric]["mean"],
                    cpu_pooled[cambi_metric]["mean"],
                    places=2,
                    msg="cambi CUDA twin score within tolerance of CPU",
                )

            # Overall vmaf score parity (< 0.1 delta)
            self.assertAlmostEqual(
                cuda_pooled["vmaf"]["mean"],
                cpu_pooled["vmaf"]["mean"],
                delta=0.05,
                msg="Overall vmaf score should closely match between CUDA and CPU runs",
            )
        finally:
            if os.path.exists(cuda_json):
                os.remove(cuda_json)
            if os.path.exists(cpu_json):
                os.remove(cpu_json)

    def test_unknown_option_typo_rejected(self):
        cmd = [
            self.vmaf_bin,
            "--reference",
            self.ref_yuv,
            "--distorted",
            self.dis_yuv,
            "--width",
            "576",
            "--height",
            "324",
            "--pixel_format",
            "420",
            "--bitdepth",
            "8",
            "--feature",
            "adm=adm_csf_moed=2",
            "--no_prediction",
        ]
        res = _run(cmd)
        self.assertNotEqual(
            res.returncode, 0, "Typo option should cause vmaf to exit with non-zero"
        )
        combined_output = res.stdout + res.stderr
        self.assertIn(
            "unknown option 'adm_csf_moed'",
            combined_output,
            f"Expected error message not found in output: {combined_output}",
        )


if __name__ == "__main__":
    unittest.main()
