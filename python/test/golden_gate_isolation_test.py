# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Tests for Netflix golden gate build-directory and compiler isolation."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest

import vmaf
from vmaf import project_path

_PROBE = (
    "import json, vmaf; "
    "print(json.dumps([vmaf.ExternalProgram.vmafexec, vmaf.ExternalProgram.vmaf_feature]))"
)


def _child_env(env):
    """Return `env` set up for a fresh interpreter that imports vmaf through python/."""
    child = dict(env)
    child["PYTHONPATH"] = os.pathsep.join(
        [project_path("python"), project_path(os.path.join("compat", "python-vmaf"))]
    )
    return child


def _import_time_paths(env):
    """Resolve ExternalProgram's executable paths as a fresh import under `env` sees them.

    The paths are fixed when the package is imported. Re-importing it in this
    process with importlib.reload goes through the python/vmaf shim, which
    installs a new module object in sys.modules and rewrites the original's
    __spec__, so every later test that patches `vmaf.<name>` misses the module
    its imported names are bound to. A child interpreter has no shared state.
    """
    res = subprocess.run(
        [sys.executable, "-c", _PROBE],
        capture_output=True,
        text=True,
        env=_child_env(env),
        check=False,
    )
    if res.returncode != 0:
        raise AssertionError(f"importing vmaf failed: {res.stderr}")
    return json.loads(res.stdout.strip().splitlines()[-1])


def _env_without(*names):
    env = dict(os.environ)
    for name in names:
        env.pop(name, None)
    return env


class GoldenGateBuildDirIsolationTest(unittest.TestCase):
    """Verify build-directory isolation and configuration contracts."""

    def test_default_build_dir_preserves_developer_build(self):
        """When VMAF_BUILD_DIR is not set, default to core/build."""
        env = _env_without("VMAF_BUILD_DIR", "VMAFEXEC_PATH", "VMAF_PATH")
        vmafexec, feature = _import_time_paths(env)
        self.assertEqual(vmafexec, project_path(os.path.join("core", "build", "tools", "vmaf")))
        self.assertEqual(
            feature, project_path(os.path.join("core", "build", "tools", "vmaf_feature"))
        )

    def test_vmaf_build_dir_environment_override(self):
        """When VMAF_BUILD_DIR is set, ExternalProgram resolves under it."""
        custom_build_dir = "core/build-golden"
        env = _env_without("VMAFEXEC_PATH", "VMAF_PATH")
        env["VMAF_BUILD_DIR"] = custom_build_dir
        vmafexec, feature = _import_time_paths(env)
        self.assertEqual(vmafexec, project_path(os.path.join(custom_build_dir, "tools", "vmaf")))
        self.assertEqual(
            feature, project_path(os.path.join(custom_build_dir, "tools", "vmaf_feature"))
        )

    def test_vmaf_build_dir_absolute_path_override(self):
        """When VMAF_BUILD_DIR is an absolute path, resolve directly."""
        with tempfile.TemporaryDirectory() as tmpdir:
            env = _env_without("VMAFEXEC_PATH", "VMAF_PATH")
            env["VMAF_BUILD_DIR"] = tmpdir
            vmafexec, feature = _import_time_paths(env)
            self.assertEqual(vmafexec, os.path.join(tmpdir, "tools", "vmaf"))
            self.assertEqual(feature, os.path.join(tmpdir, "tools", "vmaf_feature"))

    def test_vmafexec_path_precedence_over_build_dir(self):
        """Direct VMAFEXEC_PATH takes precedence over VMAF_BUILD_DIR."""
        with tempfile.NamedTemporaryFile() as direct_bin:
            env = dict(os.environ)
            env["VMAF_BUILD_DIR"] = "core/build-golden"
            env["VMAFEXEC_PATH"] = direct_bin.name
            vmafexec, _ = _import_time_paths(env)
            self.assertEqual(vmafexec, direct_bin.name)

    def test_vmafexec_path_nonexistent_does_not_silently_fallback(self):
        """Non-existent VMAFEXEC_PATH must not silently fall back to build_dir."""
        nonexistent = "/nonexistent/tools/vmaf"
        env = dict(os.environ)
        env["VMAF_BUILD_DIR"] = "core/build-golden"
        env["VMAFEXEC_PATH"] = nonexistent
        vmafexec, _ = _import_time_paths(env)
        self.assertEqual(vmafexec, nonexistent)
        with self.assertRaises(AssertionError):
            vmaf.required(vmafexec)

    def test_import_vmaf_does_not_pollute_stdout_when_externals_absent(self):
        """Importing vmaf must not emit spurious ImportError prints to stdout."""
        res = subprocess.run(
            [sys.executable, "-c", "import vmaf"],
            capture_output=True,
            text=True,
            env=_child_env(os.environ),
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout, "", f"Expected empty stdout on import, got: {res.stdout!r}")


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
