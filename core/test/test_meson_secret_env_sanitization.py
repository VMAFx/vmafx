#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression contract for Meson test credential sanitization (ADR-1333)."""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

CORE_ROOT = ROOT / "core"
CORE_MESON_BUILD = CORE_ROOT / "meson.build"
MESON_BUILD_FILES = tuple(sorted(CORE_ROOT.rglob("meson.build")))

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
    "ACTIONS_ID_TOKEN_REQUEST_TOKEN",
    "ACTIONS_RUNTIME_TOKEN",
)

SAFE_HOST_ENV_VARS = (
    "PATH",
    "PATHEXT",
    "SYSTEMROOT",
    "SystemRoot",
    "WINDIR",
    "COMSPEC",
)


def _run_cmd(cmd: list[str], cwd: Path, env: dict[str, str]) -> TextCommandResult:
    """Run one allowlisted command with bounded output and wall time."""
    return run_command(
        cmd,
        allowed_executables=(cmd[0],),
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        timeout_seconds=30,
    )


def _minimal_probe_env(tmppath: Path, probe_marker: str) -> dict[str, str]:
    """Build a small environment without enumerating arbitrary host values."""
    probe_tmp = tmppath / "tmp"
    probe_home = tmppath / "home"
    probe_tmp.mkdir(exist_ok=True)
    probe_home.mkdir(exist_ok=True)
    env = {name: os.environ[name] for name in SAFE_HOST_ENV_VARS if name in os.environ}
    env.setdefault("PATH", os.defpath)
    env.update(
        HOME=str(probe_home),
        TMPDIR=str(probe_tmp),
        TEMP=str(probe_tmp),
        TMP=str(probe_tmp),
        LANG="C",
        LC_ALL="C",
        USER="vmafx-test",
        VMAFX_TEST_REQUIRED_VAR="ordinary_value",
    )
    env.update(dict.fromkeys(SECRET_ENV_VARS, probe_marker))
    return env


def _read_meson_sources() -> dict[Path, str]:
    """Read every production Meson declaration below core/."""
    return {path: path.read_text(encoding="utf-8") for path in MESON_BUILD_FILES}


def _meson_contract_errors(sources: dict[Path, str]) -> list[str]:
    """Return fail-closed errors for setup bypasses and credential reintroduction."""
    errors: list[str] = []
    setup_sites: list[Path] = []
    for path, content in sources.items():
        setup_sites.extend([path] * len(re.findall(r"\badd_test_setup\s*\(", content)))
    if setup_sites != [CORE_MESON_BUILD]:
        relative_sites = [str(path.relative_to(ROOT)) for path in setup_sites]
        errors.append(
            f"expected exactly one add_test_setup in core/meson.build; got {relative_sites}"
        )

    root_content = sources.get(CORE_MESON_BUILD, "")
    default_setup = re.compile(
        r"add_test_setup\(\s*['\"]default['\"]\s*,"
        r"(?:(?!add_test_setup).)*?env\s*:\s*sanitized_test_env\s*,"
        r"(?:(?!add_test_setup).)*?is_default\s*:\s*true\s*,?"
        r"(?:(?!add_test_setup).)*?\)",
        re.DOTALL,
    )
    if len(default_setup.findall(root_content)) != 1:
        errors.append(
            "default setup must bind sanitized_test_env and is_default: true exactly once"
        )

    root_without_allowed_unsets = root_content
    for secret_var in SECRET_ENV_VARS:
        unset = re.compile(rf"sanitized_test_env\.unset\(\s*['\"]{re.escape(secret_var)}['\"]\s*\)")
        matches = unset.findall(root_content)
        if len(matches) != 1:
            errors.append(f"expected exactly one sanitized unset for {secret_var}")
        root_without_allowed_unsets = unset.sub("", root_without_allowed_unsets, count=1)

    for path, content in sources.items():
        scan_content = root_without_allowed_unsets if path == CORE_MESON_BUILD else content
        for secret_var in SECRET_ENV_VARS:
            if secret_var in scan_content:
                errors.append(
                    f"forbidden credential name {secret_var} reintroduced in {path.relative_to(ROOT)}"
                )
    return errors


def _probe_child_source() -> str:
    """Return a child that checks key visibility without reading any values."""
    names = ", ".join(repr(name) for name in SECRET_ENV_VARS)
    return (
        "import os, sys\n"
        f"secrets = ({names},)\n"
        "visible = any(name in os.environ for name in secrets)\n"
        "required = 'PATH' in os.environ and 'VMAFX_TEST_REQUIRED_VAR' in os.environ\n"
        "if sys.argv[1] == 'visible':\n"
        "    sys.exit(0 if visible else 1)\n"
        "if sys.argv[1] == 'hidden':\n"
        "    sys.exit(0 if not visible and required else 1)\n"
        "sys.exit(2)\n"
    )


def _sanitizing_setup() -> str:
    unset_lines = "\n".join(f"e.unset('{name}')" for name in SECRET_ENV_VARS)
    return (
        "e = environment()\n"
        f"{unset_lines}\n"
        "add_test_setup('default', env : e, is_default : true)\n"
    )


def _write_probe_project(
    tmppath: Path,
    *,
    sanitized: bool,
    expected_visibility: str,
    alternate_setup: bool = False,
    per_test_restore: bool = False,
) -> None:
    """Write a hermetic Meson project for leak and precedence probes."""
    (tmppath / "probe_child.py").write_text(_probe_child_source(), encoding="utf-8")
    lines = ["project('secret_probe', meson_version: '>= 1.4.0')"]
    if sanitized:
        lines.append(_sanitizing_setup())
    if alternate_setup:
        lines.append("add_test_setup('unsafe')")
    lines.append("py = import('python').find_installation()")
    if per_test_restore:
        lines.extend(
            (
                "per_test_env = environment()",
                "per_test_env.set('GITHUB_TOKEN', 'synthetic_per_test_restore')",
            )
        )
    env_argument = ", env : per_test_env" if per_test_restore else ""
    lines.append(
        "test('probe', py, args : [files('probe_child.py'), "
        f"'{expected_visibility}']{env_argument})"
    )
    (tmppath / "meson.build").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _diagnostic(result: TextCommandResult, probe_marker: str) -> str:
    """Return command output with even synthetic probe values redacted."""
    output = f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    return output.replace(probe_marker, "<REDACTED>")


def _run_probe(
    tmppath: Path,
    meson_exe: str,
    probe_marker: str,
    *,
    setup_name: str | None = None,
) -> tuple[TextCommandResult, dict[str, str]]:
    """Configure and execute one synthetic probe, returning its logged environment."""
    env = _minimal_probe_env(tmppath, probe_marker)
    build_dir = tmppath / "build"
    setup_res = _run_cmd([meson_exe, "setup", str(build_dir), str(tmppath)], tmppath, env)
    if setup_res.returncode != 0:
        raise AssertionError(f"meson setup failed: {_diagnostic(setup_res, probe_marker)}")
    test_cmd = [meson_exe, "test", "-C", str(build_dir)]
    if setup_name is not None:
        test_cmd.append(f"--setup={setup_name}")
    test_res = _run_cmd(test_cmd, tmppath, env)
    log_suffix = f"-{setup_name}" if setup_name is not None else ""
    log_path = build_dir / "meson-logs" / f"testlog{log_suffix}.json"
    if not log_path.exists():
        raise AssertionError(
            f"testlog.json was not generated: {_diagnostic(test_res, probe_marker)}"
        )
    log_entry = json.loads(log_path.read_text(encoding="utf-8").strip().splitlines()[0])
    return test_res, dict(log_entry.get("env", {}))


class MesonSecretEnvSanitizationContractTest(unittest.TestCase):
    """Verify the current Meson suite cannot bypass credential sanitization."""

    def test_static_meson_contract_sanitizes_every_declared_test(self) -> None:
        """Current production declarations have one non-bypassable default setup."""
        self.assertEqual(_meson_contract_errors(_read_meson_sources()), [])

    def test_static_contract_rejects_alternate_setup(self) -> None:
        sources = _read_meson_sources()
        other = CORE_ROOT / "test" / "meson.build"
        sources[other] += "\nadd_test_setup('unsafe')\n"
        errors = _meson_contract_errors(sources)
        self.assertTrue(any("exactly one add_test_setup" in error for error in errors), errors)

    def test_static_contract_rejects_explicit_per_test_reintroduction(self) -> None:
        for secret_var in SECRET_ENV_VARS:
            with self.subTest(secret_var=secret_var):
                sources = _read_meson_sources()
                other = CORE_ROOT / "test" / "meson.build"
                sources[other] += f"\ne = environment()\ne.set('{secret_var}', 'synthetic')\n"
                errors = _meson_contract_errors(sources)
                self.assertTrue(any(secret_var in error for error in errors), errors)

    def test_probe_environment_is_allowlisted_and_synthetic(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            with mock.patch.dict(
                os.environ, {"VMAFX_UNRELATED_SYNTHETIC_CREDENTIAL": "do-not-copy"}
            ):
                env = _minimal_probe_env(tmppath, "synthetic_secret")
            self.assertNotIn("VMAFX_UNRELATED_SYNTHETIC_CREDENTIAL", env)
            allowed = set(SAFE_HOST_ENV_VARS) | {
                "HOME",
                "TMPDIR",
                "TEMP",
                "TMP",
                "LANG",
                "LC_ALL",
                "USER",
                "VMAFX_TEST_REQUIRED_VAR",
                *SECRET_ENV_VARS,
            }
            self.assertLessEqual(set(env), allowed)

    def test_live_process_environment_excludes_secrets(self) -> None:
        """The registered test itself observes no forbidden credential keys."""
        if "MESON_TEST_ITERATION" not in os.environ:
            self.skipTest("not running inside the Meson test runner")
        for secret_var in SECRET_ENV_VARS:
            with self.subTest(secret_var=secret_var):
                self.assertNotIn(secret_var, os.environ)
        self.assertIn("PATH", os.environ)

    def test_reproduce_red_unsanitized_leaks_into_child_and_log(self) -> None:
        """RED: vanilla Meson exposes synthetic credential keys to child and JSON log."""
        meson_exe = shutil.which("meson")
        if not meson_exe:
            self.skipTest("meson not available on PATH")
        probe_marker = "synthetic_secret_token_red_proof"
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            _write_probe_project(tmppath, sanitized=False, expected_visibility="visible")
            test_res, logged_env = _run_probe(tmppath, meson_exe, probe_marker)
            self.assertEqual(test_res.returncode, 0, _diagnostic(test_res, probe_marker))
            self.assertTrue(any(name in logged_env for name in SECRET_ENV_VARS))

    def test_green_default_setup_excludes_secrets_and_preserves_required_env(self) -> None:
        """GREEN: the default setup removes all keys from child and JSON log."""
        meson_exe = shutil.which("meson")
        if not meson_exe:
            self.skipTest("meson not available on PATH")
        probe_marker = "synthetic_secret_token_green_proof"
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            _write_probe_project(tmppath, sanitized=True, expected_visibility="hidden")
            test_res, logged_env = _run_probe(tmppath, meson_exe, probe_marker)
            self.assertEqual(test_res.returncode, 0, _diagnostic(test_res, probe_marker))
            for secret_var in SECRET_ENV_VARS:
                with self.subTest(secret_var=secret_var):
                    self.assertNotIn(secret_var, logged_env)
            self.assertIn("PATH", logged_env)
            self.assertIn("VMAFX_TEST_REQUIRED_VAR", logged_env)

    def test_red_alternate_setup_bypasses_default_sanitizer(self) -> None:
        """RED: Meson honors an explicit alternate setup instead of the default."""
        meson_exe = shutil.which("meson")
        if not meson_exe:
            self.skipTest("meson not available on PATH")
        probe_marker = "synthetic_secret_token_alternate_setup"
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            _write_probe_project(
                tmppath,
                sanitized=True,
                expected_visibility="visible",
                alternate_setup=True,
            )
            test_res, _ = _run_probe(tmppath, meson_exe, probe_marker, setup_name="unsafe")
            self.assertEqual(test_res.returncode, 0, _diagnostic(test_res, probe_marker))

    def test_red_per_test_env_can_restore_key_after_setup(self) -> None:
        """RED: Meson applies per-test env after setup env, restoring a named key."""
        meson_exe = shutil.which("meson")
        if not meson_exe:
            self.skipTest("meson not available on PATH")
        probe_marker = "synthetic_secret_token_per_test_env"
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            _write_probe_project(
                tmppath,
                sanitized=True,
                expected_visibility="visible",
                per_test_restore=True,
            )
            test_res, _ = _run_probe(tmppath, meson_exe, probe_marker)
            self.assertEqual(test_res.returncode, 0, _diagnostic(test_res, probe_marker))


if __name__ == "__main__":
    unittest.main()
