#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression contract proving Meson test environments and logs exclude secrets (ADR-1333)."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE_MESON_BUILD = ROOT / "core" / "meson.build"

SECRET_ENV_VARS = (
    "GITHUB_PERSONAL_ACCESS_TOKEN",
    "GITHUB_TOKEN",
    "GH_TOKEN",
    "GH_ENTERPRISE_TOKEN",
    "GITHUB_ENTERPRISE_TOKEN",
    "GITHUB_PAT",
    "GH_PAT",
    "GITHUB_AUTH_TOKEN",
    "GITHUB_API_TOKEN",
    "HOMEBREW_GITHUB_API_TOKEN",
)

ORDINARY_ENV_VARS = (
    "PATH",
    "VMAFX_TEST_REQUIRED_VAR",
)


def _run_cmd(
    cmd: list[str], cwd: str | Path, env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    """Run a subprocess command with bounded execution time (HISS-02)."""
    return subprocess.run(  # noqa: S603
        cmd,
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )


def _host_env_without_github_credentials() -> dict[str, str]:
    """Copy ordinary host variables without reading credential values."""
    return {name: os.environ[name] for name in os.environ if name not in SECRET_ENV_VARS}


def _synthetic_probe_env(value: str) -> dict[str, str]:
    """Build a probe environment containing only synthetic credentials."""
    simulated_env = _host_env_without_github_credentials()
    for secret_var in SECRET_ENV_VARS:
        simulated_env[secret_var] = value
    simulated_env["VMAFX_TEST_REQUIRED_VAR"] = "ordinary_value"
    return simulated_env


def _write_sanitized_probe(tmppath: Path) -> None:
    """Write the minimal child and Meson project for the GREEN proof."""
    unset_lines = "\n".join(f"e.unset('{var}')" for var in SECRET_ENV_VARS)
    child_assert = (
        "import os, sys\n"
        "secrets = (" + ", ".join(f"'{v}'" for v in SECRET_ENV_VARS) + ")\n"
        "leaked = [s for s in secrets if s in os.environ]\n"
        "if leaked or 'VMAFX_TEST_REQUIRED_VAR' not in os.environ or 'PATH' not in os.environ:\n"
        "    sys.exit(1)\n"
        "sys.exit(0)\n"
    )
    (tmppath / "probe_child.py").write_text(child_assert, encoding="utf-8")
    meson_build = (
        "project('sanitized_probe', 'c', meson_version: '>= 1.4.0')\n"
        "e = environment()\n"
        f"{unset_lines}\n"
        "add_test_setup('default', env : e, is_default : true)\n"
        "py = import('python').find_installation()\n"
        "test('probe', py, args : files('probe_child.py'))\n"
    )
    (tmppath / "meson.build").write_text(meson_build, encoding="utf-8")


class MesonSecretEnvSanitizationContractTest(unittest.TestCase):
    """Verifies that secret-bearing GitHub credentials are never inherited or logged by Meson tests."""

    def test_static_meson_build_sanitizes_credentials(self) -> None:
        """Static verification: core/meson.build registers default test setup unsetting all tokens."""
        content = CORE_MESON_BUILD.read_text(encoding="utf-8")

        self.assertIn("add_test_setup('default'", content)
        self.assertIn("is_default : true", content)

        for secret_var in SECRET_ENV_VARS:  # HISS-02: scalar upper bound
            with self.subTest(secret_var=secret_var):
                pattern = rf"\.unset\(\s*['\"]{re.escape(secret_var)}['\"]\s*\)"
                self.assertRegex(
                    content,
                    pattern,
                    f"core/meson.build missing test environment unset for {secret_var}",
                )

        # Ensure ordinary and non-secret variables are NOT unset
        for non_secret in ("PATH", "GITHUB_ACTIONS", "GITHUB_REPOSITORY"):
            pattern = rf"\.unset\(\s*['\"]{re.escape(non_secret)}['\"]\s*\)"
            self.assertNotRegex(
                content,
                pattern,
                f"core/meson.build incorrectly unsets non-secret variable {non_secret}",
            )

    def test_live_process_environment_excludes_secrets(self) -> None:
        """In-suite live verification: when run under Meson, process has zero secret-bearing credentials."""
        if "MESON_TEST_ITERATION" not in os.environ:
            self.skipTest("Not running inside Meson test runner (MESON_TEST_ITERATION unset)")

        for secret_var in SECRET_ENV_VARS:  # HISS-02: scalar upper bound
            with self.subTest(secret_var=secret_var):
                # Never print or dump environment mapping on failure (security directive)
                self.assertFalse(
                    secret_var in os.environ,
                    f"Secret credential variable {secret_var} leaked into test process environment",
                )

        self.assertTrue("PATH" in os.environ, "Required standard environment PATH is missing")

    def test_reproduce_red_unsanitized_leaks_token(self) -> None:
        """RED proof: vanilla Meson without default sanitization leaks secrets to env and testlog.json."""
        meson_exe = shutil.which("meson")
        if not meson_exe:
            self.skipTest("meson not available on PATH")

        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            meson_build = (
                "project('unsanitized_probe', 'c', meson_version: '>= 1.4.0')\n"
                "py = import('python').find_installation()\n"
                "test('probe', py, args : ['-c', 'import sys; sys.exit(0)'])\n"
            )
            (tmppath / "meson.build").write_text(meson_build, encoding="utf-8")

            simulated_env = _synthetic_probe_env("synthetic_secret_token_red_proof")

            setup_res = _run_cmd(
                [meson_exe, "setup", str(tmppath / "build"), str(tmppath)], tmppath, simulated_env
            )
            self.assertEqual(setup_res.returncode, 0, f"meson setup failed: {setup_res.stderr}")

            test_res = _run_cmd(
                [meson_exe, "test", "-C", str(tmppath / "build")], tmppath, simulated_env
            )
            self.assertEqual(test_res.returncode, 0, f"meson test failed: {test_res.stderr}")

            log_path = tmppath / "build" / "meson-logs" / "testlog.json"
            self.assertTrue(log_path.exists(), "testlog.json was not generated")

            log_content = log_path.read_text(encoding="utf-8")
            log_entry = json.loads(log_content.strip().splitlines()[0])
            logged_env = log_entry.get("env", {})

            # RED reproduction: unsanitized Meson persists secret keys in testlog.json
            self.assertTrue(
                "GITHUB_PERSONAL_ACCESS_TOKEN" in logged_env,
                "Expected vanilla Meson to leak GITHUB_PERSONAL_ACCESS_TOKEN in testlog.json",
            )

    def test_green_sanitized_setup_excludes_secrets_and_preserves_required_env(self) -> None:
        """GREEN proof: default test setup excludes secrets from child env and logs while keeping required env."""
        meson_exe = shutil.which("meson")
        if not meson_exe:
            self.skipTest("meson not available on PATH")

        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            _write_sanitized_probe(tmppath)
            simulated_env = _synthetic_probe_env("synthetic_secret_token_green_proof")

            setup_res = _run_cmd(
                [meson_exe, "setup", str(tmppath / "build"), str(tmppath)], tmppath, simulated_env
            )
            self.assertEqual(setup_res.returncode, 0, f"meson setup failed: {setup_res.stderr}")

            test_res = _run_cmd(
                [meson_exe, "test", "-C", str(tmppath / "build")], tmppath, simulated_env
            )
            self.assertEqual(test_res.returncode, 0, f"meson test failed: {test_res.stderr}")

            log_path = tmppath / "build" / "meson-logs" / "testlog.json"
            self.assertTrue(log_path.exists(), "testlog.json was not generated")

            log_content = log_path.read_text(encoding="utf-8")
            log_entry = json.loads(log_content.strip().splitlines()[0])
            logged_env = log_entry.get("env", {})

            # GREEN proof: zero secret keys in testlog.json
            for secret_var in SECRET_ENV_VARS:  # HISS-02: scalar upper bound
                with self.subTest(secret_var=secret_var):
                    self.assertFalse(
                        secret_var in logged_env,
                        f"Secret variable {secret_var} unexpectedly persisted in testlog.json",
                    )

            # Ordinary required variables preserved
            self.assertTrue("PATH" in logged_env, "PATH missing from testlog.json env")
            self.assertTrue(
                "VMAFX_TEST_REQUIRED_VAR" in logged_env, "Required env missing from testlog.json"
            )


if __name__ == "__main__":
    unittest.main()
