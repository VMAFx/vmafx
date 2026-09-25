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

from scripts.ci.run_meson_test import (  # noqa: E402
    CREDENTIAL_ENV_VARS,
    sanitize_process_environment,
)
from scripts.lib.safe_subprocess import TextCommandResult  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

CORE_ROOT = ROOT / "core"
CORE_MESON_BUILD = CORE_ROOT / "meson.build"
MESON_BUILD_FILES = tuple(sorted(CORE_ROOT.rglob("meson.build")))
MESON_TEST_RUNNER = ROOT / "scripts" / "ci" / "run_meson_test.py"
PRE_COMMIT_CONFIG = ROOT / ".pre-commit-config.yaml"

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

ENTRYPOINT_GLOBS = (
    "**/Makefile",
    "*.py",
    "*.sh",
    "**/tox.ini",
    ".github/workflows/*.yml",
    ".github/workflows/*.yaml",
    ".github/actions/**/*.yml",
    ".github/actions/**/*.yaml",
    "scripts/**/*.sh",
    "scripts/**/*.py",
    "dev/**/*.sh",
    "dev/**/*.py",
    "tools/**/*.sh",
    "tools/**/*.py",
    ".zed/tasks.json",
    ".claude/skills/**/*.sh",
)

EXPECTED_RUNNER_PATHS = {
    Path("Makefile"): ("scripts/ci/run_meson_test.py",) * 4,
    Path(".github/workflows/build.yml"): ("scripts/ci/run_meson_test.py",) * 3,
    Path(".github/workflows/libvmaf-build-matrix.yml"): (
        "scripts/ci/run_meson_test.py",
        "scripts/ci/run_meson_test.py",
        "scripts/ci/run_meson_test.py",
        r"scripts\ci\run_meson_test.py",
    ),
    Path(".github/workflows/nightly.yml"): ("scripts/ci/run_meson_test.py",),
    Path(".github/workflows/sanitizers.yml"): ("../scripts/ci/run_meson_test.py",) * 2,
    Path(".github/workflows/sycl-parity.yml"): ("scripts/ci/run_meson_test.py",),
    Path(".github/workflows/tests-and-quality-gates.yml"): ("../scripts/ci/run_meson_test.py",) * 5,
    Path(".zed/tasks.json"): ("/workspace/scripts/ci/run_meson_test.py",),
    Path(".claude/skills/bisect-regression/scaffold.sh"): ("scripts/ci/run_meson_test.py",),
    Path("scripts/dev/preflight.sh"): ("scripts/ci/run_meson_test.py",) * 3,
    Path("scripts/setup/ubuntu.sh"): ("scripts/ci/run_meson_test.py",),
    Path("scripts/sync-pelorus-interop.sh"): ("scripts/ci/run_meson_test.py",),
}

RUNNER_SCRIPT_BASENAME = "run_meson_test.py"
RUNNER_PATH = re.compile(r"(?P<path>(?:[A-Za-z0-9_.$(){}\\/:-]+[\\/])?run_meson_test\.py)")
RAW_MESON_TEST = re.compile(
    r"(?:\bmeson\s+test\b|"
    r"\bmeson\b[^\n#)]*\)\s*['\"]?\s+test\b|"
    r"\bmeson\s+compile\b[^\n#]*\s+test(?=\s|$|[;&>|'\"])|"
    r"\$\(MESON(?:_EXEC)?\)[^\n#]*\s+test(?=\s|$|[;&>|'\"])|"
    r"['\"]meson['\"]\s*,\s*['\"]test['\"]|"
    r"['\"]?\$\{?MESON(?:_EXEC)?\}?['\"]?\s+test\b)"
)
RAW_NINJA_TEST = re.compile(
    r"(?:(?:\bninja\b|\$\(NINJA(?:_EXEC)?\))"
    r"[^\n#]*\s+test(?=\s|$|[;&>|'\"])|"
    r"['\"]ninja['\"]\s*,[^\n#]*['\"]test['\"]|"
    r"['\"]?\$\{?NINJA(?:_EXEC)?\}?['\"]?[^\n#]*\s+test\b)"
)


class _NoValueReadsEnvironment(dict[str, str]):
    """Mapping sentinel that permits membership/deletion but rejects value reads."""

    def __getitem__(self, key: str) -> str:
        raise AssertionError(f"unexpected environment value read for key {key}")

    def get(self, key: str, default: str | None = None) -> str | None:
        raise AssertionError(f"unexpected environment value read for key {key}")


def _is_active_entrypoint_line(line: str) -> bool:
    return bool(line.strip()) and not line.lstrip().startswith("#")


def _logical_entrypoint_lines(content: str) -> list[tuple[int, str]]:
    """Join shell continuations while retaining the first physical line number."""
    logical_lines: list[tuple[int, str]] = []
    pending = ""
    start_line = 1
    for line_number, line in enumerate(content.splitlines(), 1):
        if not pending:
            start_line = line_number
        if line.endswith("\\"):
            pending += f"{line[:-1]} "
            continue
        logical_lines.append((start_line, f"{pending}{line}"))
        pending = ""
    if pending:
        logical_lines.append((start_line, pending))
    return logical_lines


def _split_entrypoint_commands(line: str) -> list[str]:
    """Split shell-style command separators without splitting quoted text."""
    commands: list[str] = []
    command_start = 0
    quote = ""
    escaped = False
    index = 0
    while index < len(line):
        character = line[index]
        if escaped:
            escaped = False
        elif character == "\\":
            escaped = True
        elif quote:
            if character == quote:
                quote = ""
        elif character in "'\"":
            quote = character
        elif character in ";&|":
            command = line[command_start:index].strip()
            if command:
                commands.append(command)
            while index + 1 < len(line) and line[index + 1] in ";&|":
                index += 1
            command_start = index + 1
        index += 1
    command = line[command_start:].strip()
    if command:
        commands.append(command)
    return commands


def _entrypoint_commands(content: str) -> list[tuple[int, str]]:
    """Return active logical commands with source line numbers."""
    return [
        (line_number, command)
        for line_number, line in _logical_entrypoint_lines(content)
        if _is_active_entrypoint_line(line)
        for command in _split_entrypoint_commands(line)
    ]


def _read_entrypoint_sources() -> dict[Path, str]:
    sources: dict[Path, str] = {}
    for pattern in ENTRYPOINT_GLOBS:
        for absolute_path in ROOT.glob(pattern):
            if absolute_path.is_file():
                relative_path = absolute_path.relative_to(ROOT)
                if relative_path.name != "Makefile" and (
                    "tests" in relative_path.parts or relative_path.name.startswith("test_")
                ):
                    continue
                sources[relative_path] = absolute_path.read_text(encoding="utf-8")
    return sources


def _raw_entrypoint_errors(path: Path, content: str) -> list[str]:
    errors: list[str] = []
    for line_number, command in _entrypoint_commands(content):
        command_without_runner = RUNNER_PATH.sub(" ", command)
        if RAW_MESON_TEST.search(command_without_runner) or RAW_NINJA_TEST.search(
            command_without_runner
        ):
            errors.append(f"raw Meson test entry point at {path}:{line_number}")
    return errors


def _entrypoint_contract_errors(sources: dict[Path, str]) -> list[str]:
    """Reject direct Meson/Ninja test calls and an unreviewed runner inventory."""
    errors: list[str] = []
    actual_calls: dict[Path, tuple[str, ...]] = {}
    for path, content in sources.items():
        if path == MESON_TEST_RUNNER.relative_to(ROOT):
            continue
        runner_paths = tuple(
            match.group("path")
            for _, command in _entrypoint_commands(content)
            for match in RUNNER_PATH.finditer(command)
        )
        if runner_paths:
            actual_calls[path] = runner_paths
        errors.extend(_raw_entrypoint_errors(path, content))

    if actual_calls != EXPECTED_RUNNER_PATHS:
        errors.append(
            f"repository runner inventory changed: expected {EXPECTED_RUNNER_PATHS}, "
            f"got {actual_calls}"
        )
    return errors


def _precommit_contract_pattern() -> re.Pattern[str]:
    """Return the checked-in hook path filter for this contract."""
    content = PRE_COMMIT_CONFIG.read_text(encoding="utf-8")
    hook = re.search(
        r"^\s*- id: meson-test-secret-env-contract\n" r"(?P<body>(?:(?!^\s*- id: ).*(?:\n|\Z))*)",
        content,
        re.MULTILINE,
    )
    if hook is None:
        raise AssertionError("meson-test-secret-env-contract hook is missing")
    files = re.search(r"^\s*files:\s*'(?P<pattern>[^']+)'\s*$", hook.group("body"), re.MULTILINE)
    if files is None:
        raise AssertionError("meson-test-secret-env-contract files filter is missing")
    return re.compile(files.group("pattern"))


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
        f"e = environment()\n{unset_lines}\nadd_test_setup('default', env : e, is_default : true)\n"
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
    repository_runner: bool = False,
) -> tuple[TextCommandResult, set[str], str, str]:
    """Execute a synthetic probe and return only disposable fixture-log data."""
    env = _minimal_probe_env(tmppath, probe_marker)
    build_dir = tmppath / "build"
    setup_res = _run_cmd([meson_exe, "setup", str(build_dir), str(tmppath)], tmppath, env)
    if setup_res.returncode != 0:
        raise AssertionError(f"meson setup failed: {_diagnostic(setup_res, probe_marker)}")
    if repository_runner:
        test_cmd = [
            sys.executable,
            str(MESON_TEST_RUNNER),
            "--meson-executable",
            meson_exe,
            "--",
            "-C",
            str(build_dir),
        ]
    else:
        test_cmd = [meson_exe, "test", "-C", str(build_dir)]
    if setup_name is not None:
        test_cmd.append(f"--setup={setup_name}")
    test_res = _run_cmd(test_cmd, tmppath, env)
    log_suffix = f"-{setup_name}" if setup_name is not None else ""
    json_log_path = build_dir / "meson-logs" / f"testlog{log_suffix}.json"
    text_log_path = build_dir / "meson-logs" / f"testlog{log_suffix}.txt"
    if not json_log_path.exists() or not text_log_path.exists():
        raise AssertionError(
            f"Meson fixture logs were not generated: {_diagnostic(test_res, probe_marker)}"
        )
    json_log = json_log_path.read_text(encoding="utf-8")
    text_log = text_log_path.read_text(encoding="utf-8")
    log_entry = json.loads(json_log.strip().splitlines()[0])
    return test_res, set(log_entry.get("env", {})), text_log, json_log


class MesonSecretEnvSanitizationContractTest(unittest.TestCase):
    """Verify the current Meson suite cannot bypass credential sanitization."""

    def test_runner_and_meson_use_the_same_credential_inventory(self) -> None:
        self.assertEqual(tuple(CREDENTIAL_ENV_VARS), SECRET_ENV_VARS)

    def test_runner_deletes_credentials_without_reading_values(self) -> None:
        environment = _NoValueReadsEnvironment(
            {**dict.fromkeys(SECRET_ENV_VARS, "unreadable"), "PATH": "unreadable"}
        )
        sanitize_process_environment(environment)
        self.assertEqual(set(environment), {"PATH"})

    def test_supported_entrypoints_use_repository_runner(self) -> None:
        self.assertEqual(_entrypoint_contract_errors(_read_entrypoint_sources()), [])

    def test_precommit_hook_covers_every_contract_input_scope(self) -> None:
        pattern = _precommit_contract_pattern()
        governed_paths = set(_read_entrypoint_sources())
        governed_paths.update(path.relative_to(ROOT) for path in MESON_BUILD_FILES)
        governed_paths.update(
            {
                PRE_COMMIT_CONFIG.relative_to(ROOT),
                MESON_TEST_RUNNER.relative_to(ROOT),
                Path("core/test/test_meson_secret_env_sanitization.py"),
                Path("scripts/new-test-entrypoint.sh"),
                Path("dev/new-test-entrypoint.py"),
                Path("tools/new-test-entrypoint.sh"),
                Path(".github/actions/new-test/action.yml"),
                Path(".github/workflows/new-test.yml"),
                Path(".claude/skills/new-test/run.sh"),
                Path("package/Makefile"),
                Path("package/tox.ini"),
            }
        )
        for path in sorted(governed_paths):
            with self.subTest(path=path):
                self.assertIsNotNone(pattern.search(path.as_posix()))

    def test_entrypoint_contract_rejects_each_runner_bypass(self) -> None:
        sources = _read_entrypoint_sources()
        for path, expected_paths in EXPECTED_RUNNER_PATHS.items():
            lines = sources[path].splitlines(keepends=True)
            runner_lines = [
                index
                for index, line in enumerate(lines)
                if _is_active_entrypoint_line(line) and RUNNER_SCRIPT_BASENAME in line
            ]
            self.assertEqual(len(runner_lines), len(expected_paths), path)
            for call_number, line_index in enumerate(runner_lines, 1):
                with self.subTest(path=path, call_number=call_number):
                    mutated_lines = lines.copy()
                    mutated_lines[line_index] = mutated_lines[line_index].replace(
                        RUNNER_SCRIPT_BASENAME, "meson test", 1
                    )
                    errors = _raw_entrypoint_errors(path, "".join(mutated_lines))
                    self.assertTrue(
                        any("raw Meson test entry point" in error for error in errors),
                        errors,
                    )

    def test_entrypoint_contract_rejects_new_raw_entrypoint(self) -> None:
        sources = _read_entrypoint_sources()
        unsafe_commands = (
            "meson test -C build",
            "ninja -C build test",
            "meson compile -C build test",
            'python3 -c \'run(["meson", "test", "-C", "build"])\'',
            '"$MESON" test -C build',
        )
        for unsafe_command in unsafe_commands:
            with self.subTest(unsafe_command=unsafe_command):
                mutated_sources = dict(sources)
                mutated_sources[Path("scripts/new-test-entrypoint.sh")] = f"{unsafe_command}\n"
                errors = _entrypoint_contract_errors(mutated_sources)
                self.assertTrue(
                    any("raw Meson test entry point" in error for error in errors), errors
                )

    def test_entrypoint_contract_rejects_raw_command_after_runner(self) -> None:
        content = "python3 scripts/ci/run_meson_test.py -- -C build; meson test -C build\n"
        errors = _raw_entrypoint_errors(Path("scripts/unsafe.sh"), content)
        self.assertTrue(any("raw Meson test entry point" in error for error in errors), errors)

    def test_entrypoint_contract_rejects_backslash_split_raw_meson(self) -> None:
        content = "meson \\" + "\n    test -C build\n"
        errors = _raw_entrypoint_errors(Path("package/Makefile"), content)
        self.assertTrue(any("raw Meson test entry point" in error for error in errors), errors)

    def test_entrypoint_inventory_recurses_into_nested_makefiles(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            nested_makefile = tmppath / "package" / "tests" / "Makefile"
            nested_makefile.parent.mkdir(parents=True)
            nested_makefile.write_text("meson test -C build\n", encoding="utf-8")
            with mock.patch(f"{__name__}.ROOT", tmppath):
                sources = _read_entrypoint_sources()
        self.assertIn(Path("package/tests/Makefile"), sources)

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
            test_res, logged_keys, text_log, json_log = _run_probe(tmppath, meson_exe, probe_marker)
            self.assertEqual(test_res.returncode, 0, _diagnostic(test_res, probe_marker))
            self.assertTrue(any(name in logged_keys for name in SECRET_ENV_VARS))
            self.assertTrue(any(name in text_log for name in SECRET_ENV_VARS))
            self.assertTrue(any(name in json_log for name in SECRET_ENV_VARS))
            self.assertIn(probe_marker, text_log)
            self.assertIn(probe_marker, json_log)

    def test_red_default_setup_cannot_sanitize_parent_text_log(self) -> None:
        """RED: setup sanitization happens after Meson records its parent env."""
        meson_exe = shutil.which("meson")
        if not meson_exe:
            self.skipTest("meson not available on PATH")
        probe_marker = "synthetic_secret_token_parent_log_red_proof"
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            _write_probe_project(tmppath, sanitized=True, expected_visibility="hidden")
            test_res, logged_keys, text_log, json_log = _run_probe(tmppath, meson_exe, probe_marker)
            self.assertEqual(test_res.returncode, 0, _diagnostic(test_res, probe_marker))
            for secret_var in SECRET_ENV_VARS:
                with self.subTest(secret_var=secret_var):
                    self.assertNotIn(secret_var, logged_keys)
                    self.assertNotIn(secret_var, json_log)
                    self.assertIn(secret_var, text_log)
            self.assertIn("PATH", logged_keys)
            self.assertIn("VMAFX_TEST_REQUIRED_VAR", logged_keys)
            self.assertNotIn(probe_marker, json_log)
            self.assertIn(probe_marker, text_log)

    def test_green_repository_runner_sanitizes_both_log_formats(self) -> None:
        """GREEN: the supported runner removes keys before Meson starts."""
        meson_exe = shutil.which("meson")
        if not meson_exe:
            self.skipTest("meson not available on PATH")
        probe_marker = "synthetic_secret_token_parent_log_green_proof"
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            _write_probe_project(tmppath, sanitized=True, expected_visibility="hidden")
            test_res, logged_keys, text_log, json_log = _run_probe(
                tmppath,
                meson_exe,
                probe_marker,
                repository_runner=True,
            )
            self.assertEqual(test_res.returncode, 0, _diagnostic(test_res, probe_marker))
            for secret_var in SECRET_ENV_VARS:
                with self.subTest(secret_var=secret_var):
                    self.assertNotIn(secret_var, logged_keys)
                    self.assertNotIn(secret_var, text_log)
                    self.assertNotIn(secret_var, json_log)
            self.assertNotIn(probe_marker, text_log)
            self.assertNotIn(probe_marker, json_log)
            self.assertIn("PATH", logged_keys)
            self.assertIn("VMAFX_TEST_REQUIRED_VAR", logged_keys)

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
            test_res, _, _, _ = _run_probe(tmppath, meson_exe, probe_marker, setup_name="unsafe")
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
            test_res, _, _, _ = _run_probe(tmppath, meson_exe, probe_marker)
            self.assertEqual(test_res.returncode, 0, _diagnostic(test_res, probe_marker))


if __name__ == "__main__":
    unittest.main()
