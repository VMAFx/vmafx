#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The test-suite registry: every tracked test file runs in a required check.

``.github/test-suites.json`` maps each test file of the repository to one
suite and each suite to the required checks that run it (ADR-1528). Two
commands use it:

``check``
    Fails when a tracked test file belongs to no suite or to two, when a
    suite or ``not_tests`` entry matches no tracked file, or when a suite names
    a check the Required Checks Aggregator does not require. A test directory
    nobody wired into CI is how ``tools/vmaf-tune/tests`` (2051 tests) and 60
    script tests went unrun; this check makes that a red pull request.

``run SUITE``
    Runs one suite: one pytest run over its Python test files, then every
    shell test file with ``bash``. CI runs ``run tooling``.

``list SUITE``
    Prints the test files of one suite, one per line.

``precommit-skip``
    Prints the ids of the local pre-commit hooks that only run tests of the
    tooling suite, comma-separated. The CI Pre-Commit job skips them because
    Tooling Tests runs those tests; they stay active at commit time.

A test runs once in CI (ADR-1568): ``check`` also fails when a workflow runs a
test of the tooling suite outside the job that owns it.

Exit status: 0 success, 1 a finding or a failed test, 2 a usage or I/O error.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shutil
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib import git_env
from scripts.lib.safe_subprocess import CommandTimedOut
from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(".github/test-suites.json")
AGGREGATOR = Path(".github/workflows/required-aggregator.yml")
GIT_TIMEOUT_SECONDS = 60.0
SHELL_TEST_TIMEOUT_SECONDS = 600.0
PER_TEST_TIMEOUT_SECONDS = 300
TOOLING_SUITE = "tooling"
PRECOMMIT_CONFIG = Path(".pre-commit-config.yaml")


class RegistryError(Exception):
    """The registry or the repository could not be read."""


@dataclass(frozen=True)
class Install:
    """How to build the Python environment a suite runs in (read by run_affected_suites.py).

    ``locks`` install in order with ``--require-hashes``; ``editable`` directories
    install with ``--no-deps``. ``venv_of`` names another suite whose environment
    this suite runs in (a suite CI runs in the torch venv of ``ai``).
    """

    python: str
    locks: tuple[str, ...] = ()
    editable: tuple[str, ...] = ()
    venv_of: str | None = None


@dataclass(frozen=True)
class Suite:
    name: str
    paths: tuple[str, ...]
    checks: tuple[str, ...]
    source_paths: tuple[str, ...] = ()
    install: Install | None = None
    not_local: str | None = None
    pytest_timeout: int | None = None
    pytest_timeout_method: str = "signal"
    fail_on_skip: str | None = None
    pytest_rewrite: tuple[str, str] | None = None


@dataclass(frozen=True)
class Registry:
    name_patterns: tuple[str, ...]
    path_patterns: tuple[str, ...]
    suites: tuple[Suite, ...]
    not_tests: tuple[str, ...]


def _no_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    keys = [key for key, _ in pairs]
    duplicates = sorted({key for key in keys if keys.count(key) > 1})
    if duplicates:
        raise RegistryError(f"duplicate keys in {MANIFEST}: {duplicates}")
    return dict(pairs)


def _strings(value: Any, label: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not value or not all(isinstance(v, str) and v for v in value):
        raise RegistryError(f"{label} must be a non-empty list of non-empty strings")
    return tuple(value)


def _install(value: Any, label: str) -> Install:
    if not isinstance(value, dict) or not isinstance(value.get("python"), str):
        raise RegistryError(f"{label} must be an object with a string python version")
    venv_of = value.get("venv_of")
    if venv_of is not None:
        if not isinstance(venv_of, str) or "locks" in value or "editable" in value:
            raise RegistryError(f"{label}: venv_of is a suite name and excludes locks/editable")
        return Install(python=value["python"], venv_of=venv_of)
    editable = value.get("editable", [])
    if editable:
        _strings(editable, f"{label}.editable")
    return Install(
        python=value["python"],
        locks=_strings(value.get("locks"), f"{label}.locks"),
        editable=tuple(editable),
    )


PYTEST_REWRITE_PARTS = 2


def _pytest_fields(pytest_cfg: Any, label: str) -> dict[str, Any]:
    """The `pytest` object of a suite: per-test timeout and method, and a path rewrite."""
    if not isinstance(pytest_cfg, dict):
        raise RegistryError(f"{label}.pytest must be an object")
    fields: dict[str, Any] = {}
    if "timeout" in pytest_cfg:
        timeout = pytest_cfg["timeout"]
        if not isinstance(timeout, int) or isinstance(timeout, bool) or timeout <= 0:
            raise RegistryError(f"{label}.pytest.timeout must be a positive integer")
        fields["pytest_timeout"] = timeout
    if "rewrite" in pytest_cfg:
        rewrite = _strings(pytest_cfg["rewrite"], f"{label}.pytest.rewrite")
        if len(rewrite) != PYTEST_REWRITE_PARTS:
            raise RegistryError(f"{label}.pytest.rewrite must be [from-prefix, to-prefix]")
        fields["pytest_rewrite"] = (rewrite[0], rewrite[1])
    if "method" in pytest_cfg:
        fields["pytest_timeout_method"] = str(pytest_cfg["method"])
    return fields


def _suite_run_fields(entry: dict[str, Any], label: str) -> dict[str, Any]:
    """The fields run_affected_suites.py reads: how a suite installs and runs locally."""
    has_install, has_reason = "install" in entry, "not_local" in entry
    if has_install == has_reason:
        raise RegistryError(f"{label}: give exactly one of install and not_local (a reason)")
    fields: dict[str, Any] = {}
    if has_install:
        fields["install"] = _install(entry["install"], f"{label}.install")
    elif not isinstance(entry["not_local"], str) or not entry["not_local"]:
        raise RegistryError(f"{label}.not_local must be a non-empty reason string")
    else:
        fields["not_local"] = entry["not_local"]
    if "source_paths" in entry:
        fields["source_paths"] = _strings(entry["source_paths"], f"{label}.source_paths")
    fields.update(_pytest_fields(entry.get("pytest", {}), label))
    if "fail_on_skip" in entry:
        try:
            re.compile(str(entry["fail_on_skip"]))
        except re.error as exc:
            raise RegistryError(f"{label}.fail_on_skip is not a regex: {exc}") from exc
        fields["fail_on_skip"] = str(entry["fail_on_skip"])
    return fields


def _suite(entry: Any, index: int) -> Suite:
    if not isinstance(entry, dict) or not isinstance(entry.get("name"), str):
        raise RegistryError(f"suites[{index}] must be an object with a string name")
    label = f"suites[{index}] ({entry['name']})"
    return Suite(
        name=entry["name"],
        paths=_strings(entry.get("paths"), f"{label}.paths"),
        checks=_strings(entry.get("checks"), f"{label}.checks"),
        **_suite_run_fields(entry, label),
    )


def load_registry(root: Path) -> Registry:
    """Parse the manifest, refusing duplicate keys and malformed entries."""
    try:
        text = (root / MANIFEST).read_text(encoding="utf-8")
        data = json.loads(text, object_pairs_hook=_no_duplicate_keys)
    except (OSError, ValueError) as exc:
        raise RegistryError(f"cannot read {MANIFEST}: {exc}") from exc
    if data.get("schema_version") != 1:
        raise RegistryError(f"{MANIFEST}: schema_version must be 1")
    suites = tuple(_suite(entry, i) for i, entry in enumerate(data.get("suites") or []))
    names = [suite.name for suite in suites]
    if not suites or len(set(names)) != len(names):
        raise RegistryError(f"{MANIFEST}: suites must be non-empty with unique names")
    not_tests = data.get("not_tests", [])
    if not all(isinstance(e, dict) and e.get("path") and e.get("reason") for e in not_tests):
        raise RegistryError(f"{MANIFEST}: every not_tests entry needs a path and a reason")
    return Registry(
        name_patterns=_strings(data.get("name_patterns"), "name_patterns"),
        path_patterns=tuple(data.get("path_patterns", [])),
        suites=suites,
        not_tests=tuple(e["path"] for e in not_tests),
    )


def tracked_files(root: Path) -> list[str]:
    """Every file git tracks under root, as repository-relative POSIX paths."""
    git = shutil.which("git")
    if git is None:
        raise RegistryError("git is not on PATH")
    # A hook parent's GIT_DIR / GIT_INDEX_FILE would point git at another repo.
    env = git_env.drop()
    result = run_command(
        [git, "-C", str(root), "ls-files", "-z"],
        allowed_executables=(git,),
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=GIT_TIMEOUT_SECONDS,
    )
    if result.returncode != 0:
        raise RegistryError(f"git ls-files failed in {root}: {result.stderr.strip()}")
    return [path for path in result.stdout.split("\0") if path]


def _under(path: str, prefix: str) -> bool:
    return path == prefix or (prefix.endswith("/") and path.startswith(prefix))


def path_under(path: str, prefix: str) -> bool:
    """Whether path is prefix itself or, for a directory prefix ending in `/`, inside it."""
    return _under(path, prefix)


def is_test_file(registry: Registry, path: str) -> bool:
    name = path.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatchcase(name, p) for p in registry.name_patterns) or any(
        fnmatch.fnmatchcase(path, p) for p in registry.path_patterns
    )


def required_checks(root: Path) -> set[str]:
    """The names in the aggregator's `required` list (as check-aggregator-names.sh reads them)."""
    try:
        text = (root / AGGREGATOR).read_text(encoding="utf-8")
    except OSError as exc:
        raise RegistryError(f"cannot read {AGGREGATOR}: {exc}") from exc
    match = re.search(r"const required = \[(.*?)\];", text, re.DOTALL)
    if match is None:
        raise RegistryError(f"{AGGREGATOR} has no `const required = [...]` list")
    body = re.sub(r"(?m)^\s*//.*$", "", match.group(1))
    return set(re.findall(r"'([^']+)'", body))


def _match_length(suite: Suite, path: str) -> int:
    return max((len(p) for p in suite.paths if _under(path, p)), default=0)


def _owners(registry: Registry, path: str) -> list[str]:
    """The suites whose most specific matching path claims the file.

    A file path or a deeper directory wins over a shorter prefix, so a suite can
    take single files out of a directory another suite owns.
    """
    lengths = {suite.name: _match_length(suite, path) for suite in registry.suites}
    best = max(lengths.values(), default=0)
    return [name for name, length in lengths.items() if best and length == best]


def file_findings(registry: Registry, files: Iterable[str]) -> list[str]:
    """Test files outside every suite or inside two; not_tests entries that match nothing."""
    findings = []
    used_exclusions = set()
    for path in files:
        excluded = [e for e in registry.not_tests if _under(path, e)]
        used_exclusions.update(excluded)
        if excluded or not is_test_file(registry, path):
            continue
        owners = _owners(registry, path)
        if len(owners) != 1:
            where = "no suite" if not owners else f"suites {owners}"
            findings.append(f"{path}: test file in {where}; add it to one suite of {MANIFEST}")
    for entry in sorted(set(registry.not_tests) - used_exclusions):
        findings.append(f"not_tests entry {entry!r} matches no tracked file; remove it")
    return findings


def _install_findings(registry: Registry, files: Sequence[str]) -> list[str]:
    """Install specs and source paths that name nothing tracked, or a suite that is not there."""
    findings = []
    names = {suite.name for suite in registry.suites}
    for suite in registry.suites:
        install = suite.install
        if install is not None and install.venv_of is not None and install.venv_of not in names:
            findings.append(f"suite {suite.name}: venv_of names no suite: {install.venv_of!r}")
        listed = list(suite.source_paths)
        listed += [] if install is None else [*install.locks, *(e + "/" for e in install.editable)]
        for path in listed:
            if not any(_under(f, path) for f in files):
                findings.append(f"suite {suite.name}: {path!r} matches no tracked file")
    return findings


def suite_findings(registry: Registry, files: Sequence[str], required: set[str]) -> list[str]:
    """Suite paths that hold no test file; checks the aggregator does not require."""
    findings = _install_findings(registry, files)
    tests = [f for f in files if is_test_file(registry, f)]
    for suite in registry.suites:
        for prefix in suite.paths:
            if not any(_under(f, prefix) for f in tests):
                findings.append(f"suite {suite.name}: path {prefix!r} holds no test file")
        for check in suite.checks:
            if check not in required:
                findings.append(
                    f"suite {suite.name}: check {check!r} is not in the required list of "
                    f"{AGGREGATOR}, so a red run would not block a merge"
                )
    return findings


def suite_files(registry: Registry, files: Iterable[str], name: str) -> list[str]:
    """The test files of one suite, sorted, without not_tests entries."""
    suite = next((s for s in registry.suites if s.name == name), None)
    if suite is None:
        raise RegistryError(f"no suite named {name!r} in {MANIFEST}")
    return sorted(
        path
        for path in files
        if is_test_file(registry, path)
        and not any(_under(path, e) for e in registry.not_tests)
        and _owners(registry, path) == [name]
    )


_PATH_TOKEN = re.compile(r"[\w./-]+\.(?:py|sh)\b")
_MODULE_TOKEN = re.compile(r"\b[A-Za-z_]\w*(?:\.\w+)+\b")
_DISCOVER = re.compile(r"discover\s+(?:\\\s*)?-s\s+(\S+)\s+(?:\\\s*)?-p\s+'?([^'\s]+)'?")


def referenced_files(command: str, tracked: set[str]) -> set[str]:
    """Tracked files a command names: paths, `python -m` modules, unittest discover patterns."""
    refs = set(_PATH_TOKEN.findall(command)) & tracked
    modules = {path[:-3].replace("/", "."): path for path in tracked if path.endswith(".py")}
    refs |= {modules[token] for token in _MODULE_TOKEN.findall(command) if token in modules}
    for match in _DISCOVER.finditer(command):
        directory, pattern = match.group(1).rstrip("/"), match.group(2)
        refs |= {
            path
            for path in tracked
            if path.rpartition("/")[0] == directory
            and fnmatch.fnmatchcase(path.rpartition("/")[2], pattern)
        }
    return refs


def _code_lines(text: str) -> str:
    """A workflow without its comments and step names, which only describe."""
    return "\n".join(
        line
        for line in text.splitlines()
        if not line.lstrip().startswith(("#", "- name:", "name:"))
    )


def duplicate_run_findings(root: Path, registry: Registry, files: Sequence[str]) -> list[str]:
    """Workflow commands that run a tooling test, which Tooling Tests already runs."""
    if all(suite.name != TOOLING_SUITE for suite in registry.suites):
        return []
    tooling = set(suite_files(registry, files, TOOLING_SUITE))
    tracked = set(files)
    findings = []
    for workflow in sorted((root / ".github" / "workflows").glob("*.yml")):
        text = _code_lines(workflow.read_text(encoding="utf-8"))
        for path in sorted(referenced_files(text, tracked) & tooling):
            findings.append(
                f"{workflow.name}: runs {path}, which Tooling Tests runs; a test runs once "
                f"in CI (ADR-1568), so remove it there"
            )
    return findings


def command_check(root: Path) -> int:
    registry = load_registry(root)
    files = tracked_files(root)
    findings = file_findings(registry, files)
    findings += suite_findings(registry, files, required_checks(root))
    findings += duplicate_run_findings(root, registry, files)
    for finding in findings:
        print(f"suite-registry: {finding}", file=sys.stderr)
    if findings:
        print(f"suite-registry: {len(findings)} finding(s); see ADR-1528", file=sys.stderr)
        return 1
    count = sum(len(suite_files(registry, files, suite.name)) for suite in registry.suites)
    print(f"suite-registry: OK ({count} test files in {len(registry.suites)} suites)")
    return 0


def _run_pytest(root: Path, files: Sequence[str]) -> bool:
    """One in-process pytest run over files, from the repository root.

    In-process because safe_subprocess runs the resolved interpreter, which
    would leave the virtual environment that holds the tooling lock. Every
    test has a pytest-timeout deadline, so a hung test fails instead of
    holding the job.
    """
    if not files:
        return True
    import pytest  # noqa: PLC0415 - the check command must not need pytest

    print(f"suite-registry: pytest over {len(files)} Python test files", flush=True)
    os.chdir(root)
    status = int(pytest.main(["-rs", f"--timeout={PER_TEST_TIMEOUT_SECONDS}", *files]))
    return status == 0


def run_shell_test(root: Path, bash: str, path: str) -> bool:
    try:
        result = run_command(
            [bash, path],
            allowed_executables=(bash,),
            cwd=root,
            input_data="",
            capture_output=True,
            stderr_to_stdout=True,
            text=True,
            check=False,
            timeout_seconds=SHELL_TEST_TIMEOUT_SECONDS,
        )
    except CommandTimedOut:
        print(f"FAIL {path}: exceeded {SHELL_TEST_TIMEOUT_SECONDS:g}s", flush=True)
        return False
    if result.returncode == 0:
        print(f"ok   {path}", flush=True)
        return True
    print(f"FAIL {path}: exit {result.returncode}\n{result.stdout}", flush=True)
    return False


def command_run(root: Path, name: str) -> int:
    registry = load_registry(root)
    files = suite_files(registry, tracked_files(root), name)
    python_files = [f for f in files if f.endswith(".py")]
    shell_files = [f for f in files if f.endswith(".sh")]
    others = sorted(set(files) - set(python_files) - set(shell_files))
    if others:
        raise RegistryError(f"suite {name} holds files this runner cannot run: {others}")
    bash = shutil.which("bash")
    if bash is None:
        raise RegistryError("bash is not on PATH")
    failed = [] if _run_pytest(root, python_files) else ["pytest"]
    failed += [path for path in shell_files if not run_shell_test(root, bash, path)]
    print(f"suite-registry: {name}: {len(python_files)} Python and {len(shell_files)} shell files")
    if failed:
        print(f"suite-registry: {name}: failed: {', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser(
        "check", help="every test file belongs to one suite run by a required check"
    )
    run = commands.add_parser(
        "run", help="run one suite (Python files with pytest, shell files with bash)"
    )
    run.add_argument("suite")
    listing = commands.add_parser("list", help="print the test files of one suite")
    listing.add_argument("suite")
    commands.add_parser(
        "precommit-skip", help="ids of the local pre-commit hooks that only run tooling tests"
    )
    return parser.parse_args(argv)


def precommit_test_hooks(root: Path) -> list[str]:
    """Local pre-commit hooks whose entry runs only tests of the tooling suite."""
    # Only the CI Pre-Commit job, which has PyYAML, needs this.
    import yaml  # type: ignore[import-untyped]  # noqa: PLC0415

    try:
        config = yaml.safe_load((root / PRECOMMIT_CONFIG).read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise RegistryError(f"cannot read {PRECOMMIT_CONFIG}: {exc}") from exc
    registry = load_registry(root)
    files = tracked_files(root)
    tooling = set(suite_files(registry, files, TOOLING_SUITE))
    tracked = set(files)
    hooks = [
        hook
        for repo in config.get("repos", [])
        if repo.get("repo") == "local"
        for hook in repo.get("hooks", [])
    ]
    return [
        hook["id"]
        for hook in hooks
        if (refs := referenced_files(str(hook.get("entry", "")), tracked)) and refs <= tooling
    ]


def suite_members(root: Path, name: str) -> list[str]:
    """The test files of one suite in the repository at root.

    Contract tests use it to assert the job that runs them: a test of the
    tooling suite is run by Tooling Tests and by no other step (ADR-1568).
    """
    return suite_files(load_registry(root), tracked_files(root), name)


def command_list(root: Path, name: str) -> int:
    for path in suite_members(root, name):
        print(path)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.command == "check":
            return command_check(args.root)
        if args.command == "list":
            return command_list(args.root, args.suite)
        if args.command == "precommit-skip":
            print(",".join(precommit_test_hooks(args.root)))
            return 0
        return command_run(args.root, args.suite)
    except RegistryError as exc:
        print(f"suite-registry: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
