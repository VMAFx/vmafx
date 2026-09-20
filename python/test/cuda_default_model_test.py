# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path

from vmaf import run_process
from vmaf.config import VmafConfig


def _find_vmaf_binary():
    repo_root = Path(__file__).resolve().parents[2]
    candidates = [
        repo_root / "core" / "build" / "tools" / "vmaf",
        repo_root / "build" / "tools" / "vmaf",
        repo_root / "core" / "build-cuda" / "tools" / "vmaf",
        repo_root / "core" / "build-all" / "tools" / "vmaf",
    ]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


def _probe_cuda(vmaf_bin):
    if not vmaf_bin:
        return False
    # Check if nvidia-smi runs and CUDA device is responsive
    nvidia_smi = shutil.which("nvidia-smi")
    if nvidia_smi is None:
        return False
    try:
        run_process([nvidia_smi])
    except (AssertionError, FileNotFoundError):
        return False
    return True


class CudaDefaultModelTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.vmaf_bin = _find_vmaf_binary()
        if not cls.vmaf_bin:
            raise unittest.SkipTest("vmaf binary not found in build tree")
        if not _probe_cuda(cls.vmaf_bin):
            raise unittest.SkipTest("CUDA / NVIDIA GPU not available on this host")

        cls.ref_yuv = VmafConfig.test_resource_path("yuv", "src01_hrc00_576x324.yuv")
        cls.dis_yuv = VmafConfig.test_resource_path("yuv", "src01_hrc01_576x324.yuv")
        if not Path(cls.ref_yuv).is_file() or not Path(cls.dis_yuv).is_file():
            raise unittest.SkipTest("Required test YUV video files not found")

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
            run_process(cmd)

            with Path(out_json).open(encoding="utf-8") as jf:
                data = json.load(jf)

            pooled = data.get("pooled_metrics", {})
            self.assertIn("vmaf", pooled, "Pooled metrics missing 'vmaf' key")
            self.assertIsNotNone(pooled["vmaf"].get("mean"), "vmaf mean score is None")
            vmaf_score = pooled["vmaf"]["mean"]
            self.assertGreater(vmaf_score, 0.0)
            self.assertLessEqual(vmaf_score, 100.0)
        finally:
            if Path(out_json).exists():
                Path(out_json).unlink()

    def test_cuda_cpu_parity_default_model(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f_cuda:
            cuda_json = f_cuda.name
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f_cpu:
            cpu_json = f_cpu.name

        try:
            cmd_cuda = [
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
                cuda_json,
            ]
            run_process(cmd_cuda)

            cmd_cpu = [
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
                "--no_cuda",
                "--model",
                "version=vmaf_v1.0.16_3d0h",
                "--json",
                "--output",
                cpu_json,
            ]
            run_process(cmd_cpu)

            with Path(cuda_json).open(encoding="utf-8") as f:
                cuda_data = json.load(f)
            with Path(cpu_json).open(encoding="utf-8") as f:
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
            if Path(cuda_json).exists():
                Path(cuda_json).unlink()
            if Path(cpu_json).exists():
                Path(cpu_json).unlink()

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
        with self.assertRaises(AssertionError) as error:
            run_process(cmd)
        combined_output = str(error.exception)
        self.assertIn(
            "unknown option 'adm_csf_moed'",
            combined_output,
            f"Expected error message not found in output: {combined_output}",
        )


if __name__ == "__main__":
    unittest.main()
