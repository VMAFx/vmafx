# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Tests for the vmafx CLI alias and --netflix-compat flag (ADR-0690, ADR-0696)."""

import json
import os
import pty
import re
import subprocess
import tempfile
import unittest


def _run_pty(cmd):
    master, slave = pty.openpty()
    proc = subprocess.Popen(cmd, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    out = b""
    while True:
        try:
            chunk = os.read(master, 1024)
            if not chunk:
                break
            out += chunk
        except OSError:
            break
    os.close(master)
    proc.wait()
    return proc.returncode, out.decode(errors="replace")


class VmafxCliTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        candidates_vmafx = [
            os.path.join(cls.repo_root, "build", "tools", "vmafx"),
            os.path.join(cls.repo_root, "core", "build", "tools", "vmafx"),
            os.path.join(cls.repo_root, "build", "tools", "vmafx.exe"),
            os.path.join(cls.repo_root, "core", "build", "tools", "vmafx.exe"),
        ]
        candidates_vmaf = [
            os.path.join(cls.repo_root, "build", "tools", "vmaf"),
            os.path.join(cls.repo_root, "core", "build", "tools", "vmaf"),
            os.path.join(cls.repo_root, "build", "tools", "vmaf.exe"),
            os.path.join(cls.repo_root, "core", "build", "tools", "vmaf.exe"),
        ]
        cls.vmafx_bin = next((p for p in candidates_vmafx if os.path.exists(p)), None)
        cls.vmaf_bin = next((p for p in candidates_vmaf if os.path.exists(p)), None)
        assert cls.vmafx_bin is not None, f"vmafx binary not found in {candidates_vmafx}"
        assert cls.vmaf_bin is not None, f"vmaf binary not found in {candidates_vmaf}"

        cls.ref_yuv = os.path.join(
            cls.repo_root, "python", "test", "resource", "yuv", "src01_hrc00_576x324.yuv"
        )
        cls.dis_yuv = os.path.join(
            cls.repo_root, "python", "test", "resource", "yuv", "src01_hrc01_576x324.yuv"
        )
        assert os.path.exists(cls.ref_yuv), f"ref_yuv not found: {cls.ref_yuv}"
        assert os.path.exists(cls.dis_yuv), f"dis_yuv not found: {cls.dis_yuv}"

    def test_vmafx_version(self):
        res = subprocess.run([self.vmafx_bin, "-v"], capture_output=True, text=True, check=True)
        # vmaf -v writes to stderr
        combined = (res.stdout + res.stderr).strip()
        self.assertTrue(combined.startswith("VMAFX "))
        self.assertIn("(auto-backend, precision=max)", combined)

    def test_vmaf_version_unaffected(self):
        res = subprocess.run([self.vmaf_bin, "-v"], capture_output=True, text=True, check=True)
        combined = (res.stdout + res.stderr).strip()
        self.assertTrue(combined.startswith("v"))
        self.assertNotIn("VMAFX", combined)
        self.assertNotIn("precision=max", combined)

    def test_vmafx_banner_and_precision_max_pty(self):
        cmd = [
            self.vmafx_bin,
            "-r",
            self.ref_yuv,
            "-d",
            self.dis_yuv,
            "-w",
            "576",
            "-h",
            "324",
            "-p",
            "420",
            "-b",
            "8",
            "--frame_cnt",
            "3",
        ]
        rc, out = _run_pty(cmd)
        self.assertEqual(rc, 0)
        self.assertIn("VMAFX version", out)
        self.assertIn("(precision=max)", out)
        self.assertIn("vmaf_v1.0.16_3d0h", out)
        # Verify precision > 6 decimals (lossless format)
        score_match = re.search(r"vmaf_v1\.0\.16_3d0h:\s*([0-9]+\.[0-9]+)", out)
        self.assertIsNotNone(score_match)
        decimals = len(score_match.group(1).split(".")[1])
        self.assertGreater(decimals, 6)

    def test_vmafx_netflix_compat_matches_vmaf_v061(self):
        cmd_vmafx = [
            self.vmafx_bin,
            "-r",
            self.ref_yuv,
            "-d",
            self.dis_yuv,
            "-w",
            "576",
            "-h",
            "324",
            "-p",
            "420",
            "-b",
            "8",
            "--frame_cnt",
            "5",
            "--netflix-compat",
        ]
        rc_vmafx, out_vmafx = _run_pty(cmd_vmafx)
        self.assertEqual(rc_vmafx, 0)

        # In netflix-compat mode:
        # 1. Banner must NOT contain "VMAFX version" or "precision=max"
        self.assertNotIn("VMAFX version", out_vmafx)
        self.assertIn("VMAF version", out_vmafx)
        # 2. Output model must be vmaf_v0.6.1
        self.assertIn("vmaf_v0.6.1", out_vmafx)

        # Run vmaf with explicit v0.6.1 model
        cmd_vmaf = [
            self.vmaf_bin,
            "-r",
            self.ref_yuv,
            "-d",
            self.dis_yuv,
            "-w",
            "576",
            "-h",
            "324",
            "-p",
            "420",
            "-b",
            "8",
            "--frame_cnt",
            "5",
            "-m",
            "version=vmaf_v0.6.1",
        ]
        rc_vmaf, out_vmaf = _run_pty(cmd_vmaf)
        self.assertEqual(rc_vmaf, 0)

        # 3. Parsed pooled score must match vmaf score at 4 decimal places
        score_vmafx_match = re.search(r"vmaf_v0\.6\.1:\s*([0-9]+\.[0-9]+)", out_vmafx)
        self.assertIsNotNone(score_vmafx_match)
        score_vmafx = float(score_vmafx_match.group(1))

        score_vmaf_match = re.search(r"vmaf_v0\.6\.1:\s*([0-9]+\.[0-9]+)", out_vmaf)
        self.assertIsNotNone(score_vmaf_match)
        score_vmaf = float(score_vmaf_match.group(1))

        self.assertAlmostEqual(score_vmafx, score_vmaf, places=4)

    def test_vmafx_netflix_compat_underscore_alias(self):
        cmd = [
            self.vmafx_bin,
            "-r",
            self.ref_yuv,
            "-d",
            self.dis_yuv,
            "-w",
            "576",
            "-h",
            "324",
            "-p",
            "420",
            "-b",
            "8",
            "--frame_cnt",
            "3",
            "--netflix_compat",
        ]
        rc, out = _run_pty(cmd)
        self.assertEqual(rc, 0)
        self.assertIn("vmaf_v0.6.1", out)
        self.assertNotIn("VMAFX version", out)
        self.assertNotIn("(precision=max)", out)

    def test_vmafx_json_output_lossless_vs_compat(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            out_vmafx = os.path.join(tmpdir, "vmafx.json")
            cmd_vmafx = [
                self.vmafx_bin,
                "-r",
                self.ref_yuv,
                "-d",
                self.dis_yuv,
                "-w",
                "576",
                "-h",
                "324",
                "-p",
                "420",
                "-b",
                "8",
                "--frame_cnt",
                "2",
                "--json",
                "-o",
                out_vmafx,
            ]
            subprocess.run(cmd_vmafx, check=True, capture_output=True)
            with open(out_vmafx, encoding="utf-8") as f:
                content_vmafx = f.read()
            vmafx_data = json.loads(content_vmafx)
            # Default vmafx model emits v1.0.16_3d0h features like cambi and integer_aim
            frame0_metrics = vmafx_data["frames"][0]["metrics"]
            self.assertTrue(any("cambi" in k for k in frame0_metrics))

            # Compat mode writes 6 decimals for metrics
            out_compat = os.path.join(tmpdir, "compat.json")
            cmd_compat = [
                self.vmafx_bin,
                "-r",
                self.ref_yuv,
                "-d",
                self.dis_yuv,
                "-w",
                "576",
                "-h",
                "324",
                "-p",
                "420",
                "-b",
                "8",
                "--frame_cnt",
                "2",
                "--netflix-compat",
                "--json",
                "-o",
                out_compat,
            ]
            subprocess.run(cmd_compat, check=True, capture_output=True)
            with open(out_compat, encoding="utf-8") as f:
                content_compat = f.read()
            compat_data = json.loads(content_compat)
            compat_metrics = compat_data["frames"][0]["metrics"]
            self.assertIn("vmaf", compat_metrics)
            # Find line with "vmaf": in content_compat and check it has 6 decimal places
            vmaf_line = next(line for line in content_compat.splitlines() if '"vmaf":' in line)
            match = re.search(r'"vmaf":\s*([0-9]+\.[0-9]+)', vmaf_line)
            self.assertIsNotNone(match)
            self.assertEqual(len(match.group(1).split(".")[1]), 6)


if __name__ == "__main__":
    unittest.main()
