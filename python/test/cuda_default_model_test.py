# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent

import json
import os
import subprocess
import tempfile
import unittest

from vmaf.config import VmafConfig


def _find_vmaf_binary():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    candidates = [
        os.path.join(repo_root, "core", "build", "tools", "vmaf"),
        os.path.join(repo_root, "build", "tools", "vmaf"),
        os.path.join(repo_root, "core", "build-cuda", "tools", "vmaf"),
        os.path.join(repo_root, "core", "build-all", "tools", "vmaf"),
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def _probe_cuda(vmaf_bin):
    if not vmaf_bin:
        return False
    # Check if nvidia-smi runs and CUDA device is responsive
    try:
        res = subprocess.run(["nvidia-smi"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if res.returncode != 0:
            return False
    except FileNotFoundError:
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
        if not os.path.isfile(cls.ref_yuv) or not os.path.isfile(cls.dis_yuv):
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
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
            res_cuda = subprocess.run(
                cmd_cuda, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )
            self.assertEqual(res_cuda.returncode, 0, f"CUDA run failed: {res_cuda.stderr}")

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
            res_cpu = subprocess.run(
                cmd_cpu, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )
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
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
