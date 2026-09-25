# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Tests for Netflix golden gate build-directory and compiler isolation."""

from __future__ import annotations

import importlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import vmaf
from vmaf import ExternalProgram, project_path


class GoldenGateBuildDirIsolationTest(unittest.TestCase):
    """Verify build-directory isolation and configuration contracts."""

    def test_default_build_dir_preserves_developer_build(self):
        """When VMAF_BUILD_DIR is not set, default to core/build."""
        env = dict(os.environ)
        env.pop("VMAF_BUILD_DIR", None)
        env.pop("VMAFEXEC_PATH", None)
        env.pop("VMAF_PATH", None)
        with mock.patch.dict(os.environ, env, clear=True):
            expected_vmafexec = project_path(os.path.join("core", "build", "tools", "vmaf"))
            expected_feature = project_path(os.path.join("core", "build", "tools", "vmaf_feature"))
            vmaf_module = sys.modules.get("vmaf")
            reloaded = importlib.reload(vmaf_module)
            self.assertEqual(reloaded.ExternalProgram.vmafexec, expected_vmafexec)
            self.assertEqual(reloaded.ExternalProgram.vmaf_feature, expected_feature)

    def test_vmaf_build_dir_environment_override(self):
        """When VMAF_BUILD_DIR is set, ExternalProgram resolves under it."""
        custom_build_dir = "core/build-golden"
        env = dict(os.environ)
        env["VMAF_BUILD_DIR"] = custom_build_dir
        env.pop("VMAFEXEC_PATH", None)
        env.pop("VMAF_PATH", None)
        with mock.patch.dict(os.environ, env, clear=True):
            expected_vmafexec = project_path(os.path.join(custom_build_dir, "tools", "vmaf"))
            expected_feature = project_path(os.path.join(custom_build_dir, "tools", "vmaf_feature"))
            vmaf_module = sys.modules.get("vmaf")
            reloaded = importlib.reload(vmaf_module)
            self.assertEqual(reloaded.ExternalProgram.vmafexec, expected_vmafexec)
            self.assertEqual(reloaded.ExternalProgram.vmaf_feature, expected_feature)

    def test_vmaf_build_dir_absolute_path_override(self):
        """When VMAF_BUILD_DIR is an absolute path, resolve directly."""
        with tempfile.TemporaryDirectory() as tmpdir:
            env = dict(os.environ)
            env["VMAF_BUILD_DIR"] = tmpdir
            env.pop("VMAFEXEC_PATH", None)
            env.pop("VMAF_PATH", None)
            with mock.patch.dict(os.environ, env, clear=True):
                expected_vmafexec = os.path.join(tmpdir, "tools", "vmaf")
                expected_feature = os.path.join(tmpdir, "tools", "vmaf_feature")
                vmaf_module = sys.modules.get("vmaf")
                reloaded = importlib.reload(vmaf_module)
                self.assertEqual(reloaded.ExternalProgram.vmafexec, expected_vmafexec)
                self.assertEqual(reloaded.ExternalProgram.vmaf_feature, expected_feature)

    def test_vmafexec_path_precedence_over_build_dir(self):
        """Direct VMAFEXEC_PATH takes precedence over VMAF_BUILD_DIR."""
        with tempfile.NamedTemporaryFile() as direct_bin:
            env = dict(os.environ)
            env["VMAF_BUILD_DIR"] = "core/build-golden"
            env["VMAFEXEC_PATH"] = direct_bin.name
            with mock.patch.dict(os.environ, env, clear=True):
                vmaf_module = sys.modules.get("vmaf")
                reloaded = importlib.reload(vmaf_module)
                self.assertEqual(reloaded.ExternalProgram.vmafexec, direct_bin.name)


class GoldenCompilerValidationTest(unittest.TestCase):
    """Test validation of compiler IDs for golden profile."""

    def setUp(self):
        self.script_path = os.path.join(vmaf.VMAF_ROOT, "scripts", "ci", "setup-golden-build.sh")

    def test_script_exists_and_executable(self):
        """The setup-golden-build.sh script must exist and be executable."""
        self.assertTrue(os.path.isfile(self.script_path), f"{self.script_path} does not exist")
        self.assertTrue(
            os.access(self.script_path, os.X_OK), f"{self.script_path} is not executable"
        )

    def test_validate_compiler_positive_gcc(self):
        """GCC is explicitly supported."""
        with tempfile.TemporaryDirectory() as tmpdir:
            meson_info = os.path.join(tmpdir, "meson-info")
            os.makedirs(meson_info)
            compilers_json = os.path.join(meson_info, "intro-compilers.json")
            with open(compilers_json, "w") as f:
                json.dump({"host": {"c": {"id": "gcc"}, "cpp": {"id": "gcc"}}}, f)

            res = subprocess.run(
                [self.script_path, "--check-compiler", tmpdir],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                res.returncode, 0, f"Expected success, got {res.returncode}: {res.stderr}"
            )

    def test_validate_compiler_positive_clang(self):
        """Clang is explicitly supported."""
        with tempfile.TemporaryDirectory() as tmpdir:
            meson_info = os.path.join(tmpdir, "meson-info")
            os.makedirs(meson_info)
            compilers_json = os.path.join(meson_info, "intro-compilers.json")
            with open(compilers_json, "w") as f:
                json.dump({"host": {"c": {"id": "clang"}, "cpp": {"id": "clang"}}}, f)

            res = subprocess.run(
                [self.script_path, "--check-compiler", tmpdir],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                res.returncode, 0, f"Expected success, got {res.returncode}: {res.stderr}"
            )

    def test_validate_compiler_negative_intel_llvm_fails_loudly(self):
        """intel-llvm (oneAPI icx) causes FP drift and must be rejected loudly."""
        with tempfile.TemporaryDirectory() as tmpdir:
            meson_info = os.path.join(tmpdir, "meson-info")
            os.makedirs(meson_info)
            compilers_json = os.path.join(meson_info, "intro-compilers.json")
            with open(compilers_json, "w") as f:
                json.dump({"host": {"c": {"id": "intel-llvm"}, "cpp": {"id": "intel-llvm"}}}, f)

            res = subprocess.run(
                [self.script_path, "--check-compiler", tmpdir],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(res.returncode, 0, "Validator must reject intel-llvm")
            self.assertIn("intel-llvm", res.stderr + res.stdout)
            self.assertIn("unsupported", (res.stderr + res.stdout).lower())

    def test_validate_compiler_negative_missing_info(self):
        """Missing intro-compilers.json in check mode must fail loudly."""
        with tempfile.TemporaryDirectory() as tmpdir:
            res = subprocess.run(
                [self.script_path, "--check-compiler", tmpdir],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(res.returncode, 0)
