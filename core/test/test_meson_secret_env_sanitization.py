#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression contract for Meson test credential sanitization (ADR-1333)."""

from __future__ import annotations

import functools
import json
import math
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
    "**/GNUmakefile",
    "**/makefile",
    "*.py",
    "*.sh",
    "*.ps1",
    "*.cmd",
    "*.bat",
    "**/tox.ini",
    ".github/workflows/*.yml",
    ".github/workflows/*.yaml",
    ".github/actions/**/*.yml",
    ".github/actions/**/*.yaml",
    "scripts/**/*.sh",
    "scripts/**/*.py",
    "scripts/**/*.ps1",
    "scripts/**/*.cmd",
    "scripts/**/*.bat",
    "dev/**/*.sh",
    "dev/**/*.py",
    "dev/**/*.ps1",
    "dev/**/*.cmd",
    "dev/**/*.bat",
    "tools/**/*.sh",
    "tools/**/*.py",
    "tools/**/*.ps1",
    "tools/**/*.cmd",
    "tools/**/*.bat",
    ".zed/tasks.json",
    ".claude/skills/**/*.sh",
    ".claude/skills/**/*.ps1",
    ".claude/skills/**/*.cmd",
    ".claude/skills/**/*.bat",
)

MAKEFILE_NAMES = frozenset(("Makefile", "GNUmakefile", "makefile"))
WORKFLOW_YAML_PREFIXES = ((".github", "workflows"), (".github", "actions"))
YAML_RUN_KEY = re.compile(
    r"^(?P<indent>\s*)(?P<sequence_item>-\s+)?(?:run|'run'|\"run\")\s*:\s*(?P<value>.*)$"
)
YAML_BLOCK_SCALAR = re.compile(r"^(?P<style>[>|])(?:[1-9][+-]?|[+-][1-9]?)?(?:\s+#.*)?$")

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
    r"(?:['\"]?\bmeson['\"]?\s+test\b|"
    r"\bmeson\b[^\n#)]*\)\s*['\"]?\s+test\b|"
    r"\bmeson\s+compile\b[^\n#]*\s+test(?=\s|$|[;&>|'\"])|"
    r"\$\(MESON(?:_EXEC)?\)[^\n#]*\s+test(?=\s|$|[;&>|'\"])|"
    r"['\"]meson['\"]\s*,\s*(?:['\"]compile['\"][^\n#]*|['\"]-C['\"][^\n#]*|)['\"]test['\"]|"
    r"['\"]?(?:\$\{?MESON(?:_EXEC)?\}?|\$env:MESON(?:_EXEC)?|%MESON(?:_EXEC)?%)"
    r"['\"]?\s+test\b)",
    re.IGNORECASE,
)
RAW_NINJA_TEST = re.compile(
    r"(?:(?:\bninja\b|\$\(NINJA(?:_EXEC)?\))"
    r"[^\n#]*\s+test(?=\s|$|[;&>|'\"])|"
    r"['\"]ninja['\"]\s*,[^\n#]*['\"]test['\"]|"
    r"['\"]?(?:\$\{?NINJA(?:_EXEC)?\}?|\$env:NINJA(?:_EXEC)?|%NINJA(?:_EXEC)?%)"
    r"['\"]?[^\n#]*\s+test\b)",
    re.IGNORECASE,
)
QUOTED_TEST_TOOL = re.compile(
    r"(?P<quote>['\"])(?:[^'\"\r\n]*[\\/])?(?P<tool>meson|ninja)(?:\.exe)?(?P=quote)",
    re.IGNORECASE,
)
UNQUOTED_TEST_TOOL = re.compile(
    r"(?<![A-Za-z0-9_.$}{:%-])"
    r"(?:(?:[A-Za-z]:)?(?:[^\s'\";&|(),\[\]]+[\\/])*)"
    r"(?P<tool>meson|ninja)(?:\.exe)?(?=\s|$|[;&>|'\"])",
    re.IGNORECASE,
)
_MIN_QUOTED_SCALAR_LEN = 2


class _NoValueReadsEnvironment(dict[str, str]):
    """Mapping sentinel that permits membership/deletion but rejects value reads."""

    def __getitem__(self, key: str) -> str:
        raise AssertionError(f"unexpected environment value read for key {key}")

    def get(self, key: str, default: str | None = None) -> str | None:
        raise AssertionError(f"unexpected environment value read for key {key}")


def _is_active_entrypoint_line(line: str) -> bool:
    return bool(line.strip()) and not line.lstrip().startswith("#")


def _logical_entrypoint_lines(
    content: str, continuation_markers: tuple[str, ...] = ("\\",)
) -> list[tuple[int, str]]:
    """Join shell continuations while retaining the first physical line number."""
    logical_lines: list[tuple[int, str]] = []
    pending = ""
    start_line = 1
    for line_number, line in enumerate(content.splitlines(), 1):
        if not pending:
            start_line = line_number
        if line.endswith(continuation_markers):
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


def _fold_yaml_block(lines: list[str]) -> str:
    """Apply YAML's ordinary folded-scalar line-break rules."""
    folded = ""
    for index, line in enumerate(lines):
        folded += line
        if index == len(lines) - 1:
            continue
        following = lines[index + 1]
        preserve_break = not line or not following or line[:1].isspace() or following[:1].isspace()
        folded += "\n" if preserve_break else " "
    return folded


def _strip_yaml_quotes(value: str) -> str:
    """Strip enclosing single or double quotes from a YAML scalar value."""
    stripped = value.strip()
    if len(stripped) >= _MIN_QUOTED_SCALAR_LEN and (
        (stripped.startswith('"') and stripped.endswith('"'))
        or (stripped.startswith("'") and stripped.endswith("'"))
    ):
        return stripped[1:-1]
    return stripped


def _extract_yaml_indented_lines(
    lines: list[str], start_index: int, key_indent: int
) -> tuple[list[str], int]:
    """Collect consecutive continuation lines indented deeper than key_indent."""
    collected: list[str] = []
    index = start_index
    while index < len(lines):
        line = lines[index]
        indentation = len(line) - len(line.lstrip())
        if line.strip() and indentation <= key_indent:
            break
        collected.append(line)
        index += 1
    return collected, index


def _normalize_yaml_block_lines(raw_lines: list[str], key_indent: int) -> list[str]:
    """Strip base indentation common to YAML multiline scalar lines."""
    content_indents = [len(line) - len(line.lstrip()) for line in raw_lines if line.strip()]
    content_indent = min(content_indents, default=key_indent + 1)
    return [line[content_indent:] if line.strip() else "" for line in raw_lines]


def _parse_yaml_block_scalar(
    lines: list[str], start_index: int, key_indent: int, style: str
) -> tuple[str, int]:
    """Parse a YAML block literal (|) or folded (>) scalar."""
    block_lines, next_index = _extract_yaml_indented_lines(lines, start_index, key_indent)
    normalized = _normalize_yaml_block_lines(block_lines, key_indent)
    scalar = _fold_yaml_block(normalized) if style == ">" else "\n".join(normalized)
    return scalar, next_index


def _parse_yaml_plain_or_quoted_scalar(
    lines: list[str], start_index: int, key_indent: int, initial_value: str
) -> tuple[str, int]:
    """Parse single-line, plain multiline, or quoted multiline YAML scalars."""
    cont_lines, next_index = _extract_yaml_indented_lines(lines, start_index, key_indent)
    while cont_lines and not cont_lines[-1].strip():
        cont_lines.pop()

    if not cont_lines:
        clean_val = initial_value
        if not (clean_val.startswith(('"', "'")) and clean_val.endswith(('"', "'"))):
            clean_val = clean_val.split("#", 1)[0].strip()
        return _strip_yaml_quotes(clean_val), next_index

    normalized = _normalize_yaml_block_lines(cont_lines, key_indent)
    all_lines = (
        normalized
        if not initial_value or initial_value.startswith("#")
        else [initial_value, *normalized]
    )
    joined = _strip_yaml_quotes("\n".join(all_lines).strip())
    return _fold_yaml_block(joined.splitlines()), next_index


def _workflow_run_scalars(content: str) -> list[tuple[int, str]]:
    """Extract normalized GitHub workflow/action ``run`` scalar values."""
    lines = content.splitlines()
    scalars: list[tuple[int, str]] = []
    index = 0
    while index < len(lines):
        match = YAML_RUN_KEY.match(lines[index])
        if match is None:
            index += 1
            continue

        key_indent = len(match.group("indent")) + len(match.group("sequence_item") or "")
        start_line = index + 1
        value = match.group("value").strip()
        block = YAML_BLOCK_SCALAR.fullmatch(value)
        if block is not None:
            scalar, index = _parse_yaml_block_scalar(
                lines, index + 1, key_indent, block.group("style")
            )
        else:
            scalar, index = _parse_yaml_plain_or_quoted_scalar(lines, index + 1, key_indent, value)
        scalars.append((start_line, scalar))

    return scalars


def _is_workflow_yaml(path: Path) -> bool:
    return path.suffix in {".yml", ".yaml"} and any(
        path.parts[: len(prefix)] == prefix for prefix in WORKFLOW_YAML_PREFIXES
    )


def _scan_python_line_brackets(line: str, in_quote: str, bracket_stack: list[str]) -> str:
    """Track string literals and bracket nesting across a single Python line."""
    index = 0
    escaped = False
    while index < len(line):
        ch = line[index]
        if in_quote:
            if in_quote in ('"""', "'''"):
                if line[index : index + 3] == in_quote and not escaped:
                    in_quote = ""
                    index += 2
            elif ch == "\\":
                escaped = not escaped
            elif ch == in_quote and not escaped:
                in_quote = ""
            else:
                escaped = False
        elif line[index : index + 3] in ('"""', "'''"):
            in_quote = line[index : index + 3]
            index += 2
        elif ch in ('"', "'"):
            in_quote = ch
        elif ch == "#":
            break
        elif ch in "([{":
            bracket_stack.append(ch)
        elif ch in ")]}" and bracket_stack:
            bracket_stack.pop()
        index += 1
    return in_quote


def _logical_python_lines(content: str) -> list[tuple[int, str]]:
    """Join Python explicit and implicit line continuations (parentheses, brackets, braces)."""
    logical_lines: list[tuple[int, str]] = []
    pending = ""
    start_line = 1
    bracket_stack: list[str] = []
    in_quote = ""

    for line_number, line in enumerate(content.splitlines(), 1):
        if not pending:
            start_line = line_number

        in_quote = _scan_python_line_brackets(line, in_quote, bracket_stack)

        stripped = line.rstrip()
        if stripped.endswith("\\"):
            pending += f"{stripped[:-1]} "
        elif bracket_stack or in_quote:
            pending += f"{line} "
        else:
            logical_lines.append((start_line, f"{pending}{line}"))
            pending = ""

    if pending:
        logical_lines.append((start_line, pending))
    return logical_lines


@functools.lru_cache(maxsize=1024)
def _entrypoint_commands(path: Path, content: str) -> list[tuple[int, str]]:
    """Return active logical commands with source line numbers."""
    if _is_workflow_yaml(path):
        source_blocks = _workflow_run_scalars(content)
        continuation_markers = ("\\", "`", "^")
        logical_lines = [
            (block_line + line_number - 1, line)
            for block_line, block_content in source_blocks
            for line_number, line in _logical_entrypoint_lines(block_content, continuation_markers)
        ]
    elif path.suffix == ".py":
        logical_lines = _logical_python_lines(content)
    else:
        continuation_markers = (
            ("`",)
            if path.suffix == ".ps1"
            else (("^",) if path.suffix in {".cmd", ".bat"} else ("\\",))
        )
        logical_lines = _logical_entrypoint_lines(content, continuation_markers)

    return [
        (line_number, command)
        for line_number, line in logical_lines
        if _is_active_entrypoint_line(line)
        for command in _split_entrypoint_commands(line)
    ]


def _normalize_test_tool_executables(command: str) -> str:
    """Normalize quoted, path-qualified, and Windows test-tool spellings."""
    normalized = QUOTED_TEST_TOOL.sub(
        lambda match: f"{match.group('quote')}{match.group('tool').lower()}{match.group('quote')}",
        command,
    )
    return UNQUOTED_TEST_TOOL.sub(lambda match: match.group("tool").lower(), normalized)


_ENTRYPOINT_SOURCES_CACHE: dict[Path, dict[Path, str]] = {}


def _read_entrypoint_sources() -> dict[Path, str]:
    if ROOT not in _ENTRYPOINT_SOURCES_CACHE:
        sources: dict[Path, str] = {}
        seen_makefiles: set[tuple[int, int]] = set()
        for pattern in ENTRYPOINT_GLOBS:
            for absolute_path in ROOT.glob(pattern):
                if absolute_path.is_file():
                    relative_path = absolute_path.relative_to(ROOT)
                    if relative_path.name in MAKEFILE_NAMES:
                        stat = absolute_path.stat()
                        identity = (stat.st_dev, stat.st_ino)
                        if identity in seen_makefiles:
                            continue
                        seen_makefiles.add(identity)
                    if relative_path.name not in MAKEFILE_NAMES and (
                        "tests" in relative_path.parts or relative_path.name.startswith("test_")
                    ):
                        continue
                    sources[relative_path] = absolute_path.read_text(encoding="utf-8")
        _ENTRYPOINT_SOURCES_CACHE[ROOT] = sources
    return dict(_ENTRYPOINT_SOURCES_CACHE[ROOT])


def _raw_commands_errors(path: Path, commands: list[tuple[int, str]]) -> list[str]:
    errors: list[str] = []
    for line_number, command in commands:
        cmd_lower = command.lower()
        if "meson" not in cmd_lower and "ninja" not in cmd_lower:
            continue
        command_without_runner = RUNNER_PATH.sub(" ", command)
        normalized_command = _normalize_test_tool_executables(command_without_runner)
        if RAW_MESON_TEST.search(normalized_command) or RAW_NINJA_TEST.search(normalized_command):
            errors.append(f"raw Meson test entry point at {path}:{line_number}")
    return errors


def _raw_entrypoint_errors(path: Path, content: str) -> list[str]:
    return _raw_commands_errors(path, _entrypoint_commands(path, content))


def _entrypoint_contract_errors(sources: dict[Path, str]) -> list[str]:
    """Reject direct Meson/Ninja test calls and an unreviewed runner inventory."""
    errors: list[str] = []
    actual_calls: dict[Path, tuple[str, ...]] = {}
    for path, content in sources.items():
        if path == MESON_TEST_RUNNER.relative_to(ROOT):
            continue
        commands = _entrypoint_commands(path, content)
        runner_paths = tuple(
            match.group("path")
            for _, command in commands
            if "run_meson_test.py" in command
            for match in RUNNER_PATH.finditer(command)
        )
        if runner_paths:
            actual_calls[path] = runner_paths
        errors.extend(_raw_commands_errors(path, commands))

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


# ADR-1333: Bounded deadline for Meson setup and test probe subprocesses.
# Replaces a fixed 30 s limit that is load-sensitive on heavily contested machines
# during compiler discovery and meson test execution. The finite 60--300 s override
# range keeps local probes configurable without letting inherited environment state
# remove the hang boundary.
PROBE_SUBPROCESS_TIMEOUT_DEFAULT_SECONDS = 120.0
PROBE_SUBPROCESS_TIMEOUT_MIN_SECONDS = 60.0
PROBE_SUBPROCESS_TIMEOUT_MAX_SECONDS = 300.0


def _parse_probe_subprocess_timeout(raw: str | None) -> float:
    """Return a finite probe timeout inside the configured safety bounds."""
    if raw is None:
        return PROBE_SUBPROCESS_TIMEOUT_DEFAULT_SECONDS
    try:
        timeout_seconds = float(raw)
    except ValueError as exc:
        raise ValueError(
            "VMAFX_MESON_TEST_TIMEOUT_SECONDS must be a finite number from 60 through 300"
        ) from exc
    if not math.isfinite(timeout_seconds) or not (
        PROBE_SUBPROCESS_TIMEOUT_MIN_SECONDS
        <= timeout_seconds
        <= PROBE_SUBPROCESS_TIMEOUT_MAX_SECONDS
    ):
        raise ValueError(
            "VMAFX_MESON_TEST_TIMEOUT_SECONDS must be a finite number from 60 through 300"
        )
    return timeout_seconds


PROBE_SUBPROCESS_TIMEOUT_SECONDS = _parse_probe_subprocess_timeout(
    os.environ.get("VMAFX_MESON_TEST_TIMEOUT_SECONDS")
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
        timeout_seconds=PROBE_SUBPROCESS_TIMEOUT_SECONDS,
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
                Path("package/GNUmakefile"),
                Path("package/makefile"),
                Path("package/tox.ini"),
                Path("scripts/setup/new-test.ps1"),
                Path("scripts/setup/new-test.cmd"),
                Path("scripts/setup/new-test.bat"),
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
            '"/usr/bin/meson" test -C build',
            "'/opt/meson' test -C build",
            "/usr/local/bin/meson test -C build",
            "meson.exe test -C build",
            "C:\\Tools\\meson.exe test -C build",
            '"C:\\Program Files\\Meson\\meson.exe" test -C build',
            "ninja.exe -C build test",
            '"$env:MESON" test -C build',
            "%MESON% test -C build",
            "%NINJA% -C build test",
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

    def test_entrypoint_contract_rejects_multiline_workflow_raw_meson(self) -> None:
        contents = (
            "steps:\n  - run: >-\n      meson\n      test -C build\n",
            "steps:\n  - run: |\n      meson \\\n        test -C build\n",
            "steps:\n  - run:\n      meson\n      test -C build\n",
            "steps:\n  - run: meson\n      test -C build\n",
            "steps:\n  - 'run': >-\n      meson\n      test -C build\n",
            "steps:\n  - 'run': |\n      meson \\\n        test -C build\n",
            "steps:\n  - 'run':\n      meson\n      test -C build\n",
            "steps:\n  - 'run': meson\n      test -C build\n",
            'steps:\n  - "run": >-\n      meson\n      test -C build\n',
            'steps:\n  - "run":\n      meson\n      test -C build\n',
            'steps:\n  - run: "meson\n      test -C build"\n',
            "steps:\n  - 'run': 'meson\n      test -C build'\n",
            "steps:\n  - 'run':\n      ninja -C build\n      test\n",
        )
        for content in contents:
            with self.subTest(content=content):
                path = Path(".github/workflows/unsafe.yml")
                errors = _raw_entrypoint_errors(path, content)
                self.assertTrue(
                    any("raw Meson test entry point" in error for error in errors), errors
                )
                sources = _read_entrypoint_sources()
                sources[path] = content
                errors = _entrypoint_contract_errors(sources)
                self.assertTrue(
                    any("raw Meson test entry point" in error for error in errors), errors
                )

    def test_entrypoint_contract_accepts_folded_workflow_runner(self) -> None:
        content = (
            "steps:\n"
            "  - run: >-\n"
            "      python3 scripts/ci/run_meson_test.py --\n"
            "      -C build --print-errorlogs\n"
            # Sibling step metadata is not part of the folded run scalar.
            "    shell: meson test\n"
        )
        sources = _read_entrypoint_sources()
        sources[Path(".github/workflows/nightly.yml")] = content
        self.assertEqual(_entrypoint_contract_errors(sources), [])

    def test_entrypoint_contract_discovers_and_governs_workflow_multiline_runner(self) -> None:
        templates = (
            "steps:\n  - run: >-\n      python3 scripts/ci/run_meson_test.py --\n      -C build --print-errorlogs\n    shell: meson test\n",
            "steps:\n  - run: |\n      python3 scripts/ci/run_meson_test.py -- -C build\n",
            "steps:\n  - run:\n      python3 scripts/ci/run_meson_test.py --\n      -C build\n",
            "steps:\n  - 'run': >-\n      python3 scripts/ci/run_meson_test.py --\n      -C build\n",
            "steps:\n  - 'run':\n      python3 scripts/ci/run_meson_test.py --\n      -C build\n",
            'steps:\n  - "run":\n      python3 scripts/ci/run_meson_test.py --\n      -C build\n',
        )
        for content in templates:
            with self.subTest(content=content):
                sources = _read_entrypoint_sources()
                sources[Path(".github/workflows/nightly.yml")] = content
                self.assertEqual(_entrypoint_contract_errors(sources), [])
                mutated = content.replace("python3 scripts/ci/run_meson_test.py --", "meson test")
                sources[Path(".github/workflows/nightly.yml")] = mutated
                errors = _entrypoint_contract_errors(sources)
                self.assertTrue(
                    any("raw Meson test entry point" in error for error in errors), errors
                )

    def test_entrypoint_contract_rejects_python_implicit_continuation_raw_meson(self) -> None:
        unsafe_python_snippets = (
            'cmd = [\n    "meson",\n    "test",\n    "-C",\n    "build",\n]\n',
            'cmd = (\n    "meson",\n    "test",\n    "-C",\n    "build",\n)\n',
            'subprocess.run([\n    "meson",\n    "test",\n    "-C",\n    "build",\n])\n',
            'cmd = [\n    "ninja",\n    "-C",\n    "build",\n    "test",\n]\n',
            'cmd = [\n    "meson",\n    "compile",\n    "-C",\n    "build",\n    "test",\n]\n',
            # Unclosed bracket at EOF fails closed rather than silently bypassing.
            'cmd = [\n    "meson",\n    "test",\n',
        )
        for snippet in unsafe_python_snippets:
            with self.subTest(snippet=snippet):
                path = Path("scripts/ci/unsafe.py")
                errors = _raw_entrypoint_errors(path, snippet)
                self.assertTrue(
                    any("raw Meson test entry point" in error for error in errors), errors
                )
                sources = _read_entrypoint_sources()
                sources[path] = snippet
                errors = _entrypoint_contract_errors(sources)
                self.assertTrue(
                    any("raw Meson test entry point" in error for error in errors), errors
                )

    def test_entrypoint_contract_accepts_python_implicit_continuation_runner(self) -> None:
        snippet = (
            "cmd = [\n"
            '    "python3",\n'
            '    "scripts/ci/run_meson_test.py",\n'
            '    "--",\n'
            '    "-C",\n'
            '    "build",\n'
            "]\n"
        )
        path = Path("scripts/ci/runner.py")
        errors = _raw_entrypoint_errors(path, snippet)
        self.assertEqual(errors, [])
        commands = _entrypoint_commands(path, snippet)
        self.assertTrue(
            any(RUNNER_PATH.search(cmd) for _, cmd in commands),
            commands,
        )

    def test_probe_subprocess_timeout_is_bounded_and_load_tolerant(self) -> None:
        self.assertEqual(
            _parse_probe_subprocess_timeout(None),
            PROBE_SUBPROCESS_TIMEOUT_DEFAULT_SECONDS,
        )
        for raw in ("60", "120", "180.5", "300"):
            with self.subTest(valid_override=raw):
                self.assertEqual(_parse_probe_subprocess_timeout(raw), float(raw))
        for raw in ("", "not-a-number", "nan", "inf", "-inf", "59.9", "300.1"):
            with self.subTest(invalid_override=raw):
                with self.assertRaisesRegex(ValueError, "finite number from 60 through 300"):
                    _parse_probe_subprocess_timeout(raw)
        self.assertGreaterEqual(
            PROBE_SUBPROCESS_TIMEOUT_SECONDS,
            PROBE_SUBPROCESS_TIMEOUT_MIN_SECONDS,
            "probe subprocess timeout must tolerate loaded compilation/test deadlines",
        )
        self.assertLessEqual(
            PROBE_SUBPROCESS_TIMEOUT_SECONDS,
            PROBE_SUBPROCESS_TIMEOUT_MAX_SECONDS,
            "probe subprocess timeout must retain a finite hang boundary",
        )

    def test_entrypoint_contract_rejects_powershell_split_raw_meson(self) -> None:
        content = "meson `\n  test -C build\n"
        errors = _raw_entrypoint_errors(Path("scripts/setup/unsafe.ps1"), content)
        self.assertTrue(any("raw Meson test entry point" in error for error in errors), errors)

    def test_entrypoint_contract_rejects_batch_split_raw_meson(self) -> None:
        content = "meson ^\n  test -C build\n"
        errors = _raw_entrypoint_errors(Path("scripts/setup/unsafe.cmd"), content)
        self.assertTrue(any("raw Meson test entry point" in error for error in errors), errors)

    def test_entrypoint_inventory_recurses_into_all_makefile_names(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            makefile_dir = tmppath / "package" / "tests"
            makefile_dir.mkdir(parents=True)
            for name in ("Makefile", "GNUmakefile", "makefile"):
                (makefile_dir / name).write_text("meson test -C build\n", encoding="utf-8")
            expected_paths: set[Path] = set()
            seen_identities: set[tuple[int, int]] = set()
            for name in ("Makefile", "GNUmakefile", "makefile"):
                path = makefile_dir / name
                stat = path.stat()
                identity = (stat.st_dev, stat.st_ino)
                if identity not in seen_identities:
                    seen_identities.add(identity)
                    expected_paths.add(path.relative_to(tmppath))
            with mock.patch(f"{__name__}.ROOT", tmppath):
                sources = _read_entrypoint_sources()
        observed_paths = {
            path
            for path in sources
            if path.parent == Path("package/tests") and path.name in MAKEFILE_NAMES
        }
        self.assertEqual(observed_paths, expected_paths)

    def test_entrypoint_inventory_deduplicates_case_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            makefile_dir = tmppath / "package"
            makefile_dir.mkdir()
            canonical = makefile_dir / "Makefile"
            alias = makefile_dir / "makefile"
            canonical.write_text("meson test -C build\n", encoding="utf-8")
            try:
                os.link(canonical, alias)
            except OSError as exc:
                self.skipTest(f"filesystem cannot create a Makefile hard-link alias: {exc}")
            with mock.patch(f"{__name__}.ROOT", tmppath):
                sources = _read_entrypoint_sources()
        self.assertIn(Path("package/Makefile"), sources)
        self.assertNotIn(Path("package/makefile"), sources)

    def test_entrypoint_inventory_includes_windows_scripts(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            setup_dir = tmppath / "scripts" / "setup"
            setup_dir.mkdir(parents=True)
            for suffix in ("ps1", "cmd", "bat"):
                (setup_dir / f"windows.{suffix}").write_text(
                    "meson test -C build\n", encoding="utf-8"
                )
            with mock.patch(f"{__name__}.ROOT", tmppath):
                sources = _read_entrypoint_sources()
        for suffix in ("ps1", "cmd", "bat"):
            self.assertIn(Path(f"scripts/setup/windows.{suffix}"), sources)

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
