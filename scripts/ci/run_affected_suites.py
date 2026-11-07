#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run the test suites a change affects, locally, in cached hash-locked venvs.

The local merge train builds libvmaf and runs the Meson fast suite and the
golden gate, but no Python suite, so a Python regression lands and only hosted
CI notices. This runner closes that gap without a second source of truth: the
file-to-suite mapping, the lock files, the editable installs, the per-test
timeout and the skip policy all come from ``.github/test-suites.json`` through
``scripts/ci/suite_registry.py`` (HISS-19).

A suite is affected when a changed file is one of its test files, sits under
one of its ``paths`` or ``source_paths``, or is a lock file or editable package
of its install spec. Each affected suite runs in
``<cache>/<suite>-<sha256 of its lock files>`` (default cache
``~/.cache/vmafx-suite-venvs``): built once with ``--require-hashes``, rebuilt
when a lock changes, with the editable packages re-pointed at this checkout on
every run. A suite fails on a test failure, on a time-cap overrun, and on a
skip whose reason is a missing dependency or a missing input the suite
declares (``fail_on_skip``); skipping an input is how a Python regression
slips through, so it is never a pass.

Exit status: 0 every affected suite passed (or nothing is affected), 1 a suite
failed, 2 a usage or I/O error. A suite that cannot run on a workstation
(``not_local`` in the registry) prints a ``NOT RUN`` line with its reason and
does not change the status unless ``--strict`` is given.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import IO

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import suite_registry as registry_lib
from scripts.lib import git_env
from scripts.lib.safe_subprocess import CommandTimedOut, TextCommandResult
from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CACHE = Path.home() / ".cache" / "vmafx-suite-venvs"
DEFAULT_TIME_CAP_SECONDS = 900.0
DEFAULT_INSTALL_CAP_SECONDS = 1800.0
GIT_TIMEOUT_SECONDS = 60.0
MAX_OUTPUT_BYTES = 256 * 1_048_576
LOCK_POLL_SECONDS = 2.0
READY_MARKER = ".vmafx-ready"
# Part of the venv key: bump it when the way a venv is built changes (a rebuild follows).
VENV_FORMAT = "2-compile-bytecode"
# A skip for a missing Python package fails in every suite; each suite adds the
# inputs CI provides (fail_on_skip in the registry).
MISSING_DEPENDENCY_SKIP = r"could not import|No module named|[Nn]ot installed"
LISTED_FAILURES = 10


class RunnerError(Exception):
    """The runner could not do its job (not a test failure)."""


@dataclass
class SuiteResult:
    name: str
    status: str  # passed | failed | not-run
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    seconds: float = 0.0
    venv: str = ""
    notes: list[str] = field(default_factory=list)


def _env_without_git() -> dict[str, str]:
    drop = ("VIRTUAL_ENV", "PYTHONPATH", "PYTHONHOME")
    return {k: v for k, v in git_env.drop().items() if k not in drop}


def _git(root: Path, *args: str) -> str:
    git = shutil.which("git")
    if git is None:
        raise RunnerError("git is not on PATH")
    result = run_command(
        [git, "-C", str(root), *args],
        allowed_executables=(git,),
        env=_env_without_git(),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=GIT_TIMEOUT_SECONDS,
    )
    if result.returncode != 0:
        raise RunnerError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout


def changed_files(root: Path, base: str, head: str) -> list[str]:
    """Paths that differ between two commits, renames split into delete + add."""
    out = _git(root, "diff", "--name-only", "--no-renames", "-z", base, head)
    return [path for path in out.split("\0") if path]


def parse_file_list(text: str) -> list[str]:
    """A comma, space or newline separated list of paths."""
    return [item for item in re.split(r"[,\s]+", text) if item]


def venv_owner(registry: registry_lib.Registry, suite: registry_lib.Suite) -> registry_lib.Suite:
    """The suite whose environment ``suite`` runs in (itself unless ``venv_of`` is set)."""
    if suite.install is None or suite.install.venv_of is None:
        return suite
    owner = next((s for s in registry.suites if s.name == suite.install.venv_of), None)
    if owner is None or owner.install is None or owner.install.venv_of is not None:
        raise RunnerError(f"suite {suite.name}: venv_of must name a suite with its own install")
    return owner


def trigger_prefixes(registry: registry_lib.Registry, suite: registry_lib.Suite) -> list[str]:
    """Every path whose change selects the suite."""
    prefixes = [*suite.paths, *suite.source_paths]
    install = venv_owner(registry, suite).install
    if install is not None:
        prefixes += [*install.locks, *(e + "/" for e in install.editable)]
    return prefixes


def affected_suites(
    registry: registry_lib.Registry, changed: Iterable[str]
) -> dict[str, list[str]]:
    """Suite name -> the changed files that select it; suites nothing selects are absent."""
    result: dict[str, list[str]] = {}
    changed = list(changed)
    for suite in registry.suites:
        prefixes = trigger_prefixes(registry, suite)
        hits = [f for f in changed if any(registry_lib.path_under(f, p) for p in prefixes)]
        if hits:
            result[suite.name] = hits
    return result


def venv_key(root: Path, owner: registry_lib.Suite) -> str:
    """sha256 over the owner's lock files (names and bytes) and Python version."""
    install = owner.install
    if install is None:
        raise RunnerError(f"suite {owner.name} has no install spec")
    digest = hashlib.sha256()
    digest.update(f"{VENV_FORMAT}\0{install.python}".encode())
    for lock in install.locks:
        digest.update(b"\0" + lock.encode() + b"\0")
        try:
            digest.update((root / lock).read_bytes())
        except OSError as exc:
            raise RunnerError(f"suite {owner.name}: cannot read lock {lock}: {exc}") from exc
    return digest.hexdigest()


def venv_python(venv: Path) -> Path:
    return venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


class VenvLock:
    """An exclusive per-venv lock, so two worktrees never install into one venv at once."""

    def __init__(self, path: Path, wait_seconds: float) -> None:
        self.path, self.wait_seconds = path, wait_seconds
        self._handle: IO[str] | None = None

    def __enter__(self) -> VenvLock:
        try:
            import fcntl  # noqa: PLC0415 - POSIX only; Windows runs without the lock
        except ImportError:
            return self
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._handle = self.path.open("a")
        deadline = time.monotonic() + self.wait_seconds
        while time.monotonic() <= deadline:
            try:
                fcntl.flock(self._handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except BlockingIOError:
                time.sleep(LOCK_POLL_SECONDS)
        self._handle.close()
        raise RunnerError(f"timed out waiting for {self.path}")

    def __exit__(self, *_exc: object) -> None:
        if self._handle is not None:
            self._handle.close()


def _run(
    argv: Sequence[str], root: Path, timeout: float, env: dict[str, str] | None = None
) -> TextCommandResult:
    return run_command(
        list(argv),
        allowed_executables=(argv[0],),
        cwd=root,
        env=env if env is not None else _env_without_git(),
        capture_output=True,
        stderr_to_stdout=True,
        text=True,
        check=False,
        timeout_seconds=timeout,
        max_output_bytes=MAX_OUTPUT_BYTES,
    )


def _via_env(argv: Sequence[str]) -> list[str]:
    """Run a venv interpreter through env(1).

    safe_subprocess resolves a symlinked executable, and a venv's python is a
    symlink: the resolved interpreter would leave the venv. env resolves to
    itself and then execs the venv path as given.
    """
    env_exe = shutil.which("env")
    return [env_exe, *argv] if env_exe is not None else list(argv)


def _pip_argv(venv: Path, spec: Sequence[str]) -> list[str]:
    """`uv pip install` into the venv when uv exists, else the venv's own pip."""
    uv = shutil.which("uv")
    if uv is not None:
        return [
            uv,
            "pip",
            "install",
            "--compile-bytecode",
            "--python",
            str(venv_python(venv)),
            *spec,
        ]
    return _via_env([str(venv_python(venv)), "-m", "pip", "install", *spec])


def _create_venv(venv: Path, python_spec: str, root: Path, cap: float) -> None:
    uv = shutil.which("uv")
    if uv is not None:
        argv = [uv, "venv", "--python", python_spec, str(venv)]
    else:
        interpreter = shutil.which(f"python{python_spec}") or sys.executable
        argv = [interpreter, "-m", "venv", str(venv)]
    result = _run(argv, root, cap)
    if result.returncode != 0:
        raise RunnerError(f"cannot create {venv}:\n{result.stdout[-2000:]}")


def _install(venv: Path, spec: Sequence[str], root: Path, cap: float, what: str) -> None:
    result = _run(_pip_argv(venv, spec), root, cap)
    if result.returncode != 0:
        raise RunnerError(f"installing {what} failed:\n{result.stdout[-3000:]}")


def ensure_venv(
    root: Path, owner: registry_lib.Suite, cache: Path, install_cap: float
) -> tuple[Path, float | None]:
    """The owner's venv, built if absent; the build time, or None when it was cached.

    The caller holds the venv lock. Editable packages are re-installed on every
    run (``--no-deps``, a second or two) because the cached venv is shared by
    every checkout and must point at this one.
    """
    install = owner.install
    if install is None:
        raise RunnerError(f"suite {owner.name} has no install spec")
    venv = cache / f"{owner.name}-{venv_key(root, owner)[:16]}"
    built: float | None = None
    if not (venv / READY_MARKER).exists():
        started = time.monotonic()
        shutil.rmtree(venv, ignore_errors=True)
        _create_venv(venv, install.python, root, install_cap)
        for lock in install.locks:
            spec = ["--require-hashes", "--no-build-isolation", "-r", lock]
            _install(venv, spec, root, install_cap, lock)
        (venv / READY_MARKER).write_text("ok\n", encoding="utf-8")
        built = time.monotonic() - started
    for package in install.editable:
        _install(
            venv, ["--no-deps", "--no-build-isolation", "-e", package], root, install_cap, package
        )
    return venv, built


def find_vmaf_bin(root: Path, explicit: str | None) -> str | None:
    """The vmaf binary tests use: --vmaf-bin, $VMAF_BIN, then this checkout's own build.

    Another checkout's build directory is never searched: a stale binary fails
    tests that a fresh one passes. The merge train passes the binary it built.
    """
    for given in (explicit, os.environ.get("VMAF_BIN"), os.environ.get("VMAF_BIN_FOR_TESTS")):
        if given:
            return given
    for rel in ("build/tools/vmaf", "core/build/tools/vmaf"):
        candidate = root / rel
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return None


@dataclass
class JunitSummary:
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    failures: list[str] = field(default_factory=list)
    skips: list[tuple[str, str]] = field(default_factory=list)


def parse_junit(path: Path) -> JunitSummary:
    """Counts and skip reasons from a pytest junit file; xfail is not a skip."""
    summary = JunitSummary()
    try:
        cases = ET.parse(path).getroot().iter("testcase")  # noqa: S314 - our own pytest's file
    except (OSError, ET.ParseError) as exc:
        raise RunnerError(f"cannot read pytest results {path}: {exc}") from exc
    for case in cases:
        name = f"{case.get('classname', '')}::{case.get('name', '')}"
        skip = case.find("skipped")
        if case.find("failure") is not None or case.find("error") is not None:
            summary.failed += 1
            summary.failures.append(name)
        elif skip is not None and skip.get("type") != "pytest.xfail":
            summary.skipped += 1
            summary.skips.append((name, (skip.get("message") or "") + " " + (skip.text or "")))
        else:
            summary.passed += 1
    return summary


def skip_violations(suite: registry_lib.Suite, skips: Sequence[tuple[str, str]]) -> list[str]:
    """Skips whose reason names a missing dependency or an input the suite declares."""
    pattern = re.compile(MISSING_DEPENDENCY_SKIP)
    declared = re.compile(suite.fail_on_skip) if suite.fail_on_skip else None
    return [
        f"{name}: skipped: {reason.strip()[:200]}"
        for name, reason in skips
        if pattern.search(reason) or (declared is not None and declared.search(reason))
    ]


def _rewritten(suite: registry_lib.Suite, path: str) -> str:
    """The path pytest is given: `compat/python-vmaf` is not an importable package name."""
    if suite.pytest_rewrite is not None and path.startswith(suite.pytest_rewrite[0]):
        return suite.pytest_rewrite[1] + path[len(suite.pytest_rewrite[0]) :]
    return path


def _pytest_argv(
    python: str, suite: registry_lib.Suite, junit: Path, files: Sequence[str]
) -> list[str]:
    argv = [python, "-m", "pytest", "-p", "no:cacheprovider", "-rs", "-q", f"--junitxml={junit}"]
    if suite.pytest_timeout is not None:
        argv += [
            f"--timeout={suite.pytest_timeout}",
            f"--timeout-method={suite.pytest_timeout_method}",
        ]
    return _via_env([*argv, *(_rewritten(suite, f) for f in files)])


def run_pytest(
    root: Path,
    suite: registry_lib.Suite,
    python: str,
    files: Sequence[str],
    cap: float,
    env: dict[str, str],
) -> tuple[SuiteResult, list[str]]:
    """One pytest run over the suite's Python files, bounded by ``cap`` seconds."""
    result = SuiteResult(name=suite.name, status="passed")
    if not files:
        return result, []
    junit = Path(tempfile.gettempdir()) / f"vmafx-affected-{suite.name}-{os.getpid()}.xml"
    junit.unlink(missing_ok=True)
    details: list[str] = []
    try:
        proc = _run(_pytest_argv(python, suite, junit, files), root, cap, env)
    except CommandTimedOut:
        result.status = "failed"
        return result, [f"pytest exceeded the {cap:g}s time cap"]
    if not junit.exists():
        result.status = "failed"
        return result, [f"pytest wrote no results (exit {proc.returncode}):", proc.stdout[-3000:]]
    summary = parse_junit(junit)
    junit.unlink(missing_ok=True)
    result.passed, result.failed, result.skipped = summary.passed, summary.failed, summary.skipped
    violations = skip_violations(suite, summary.skips)
    if summary.failed or violations or proc.returncode != 0:
        result.status = "failed"
        details += [f"FAILED {name}" for name in summary.failures[:LISTED_FAILURES]]
        details += violations[:LISTED_FAILURES]
        if not (summary.failed or violations):
            details.append(f"pytest exit {proc.returncode}:\n{proc.stdout[-3000:]}")
    return result, details


def run_shell_files(root: Path, files: Sequence[str]) -> tuple[int, list[str]]:
    """Each shell test with bash (the registry's own runner); returns passed count, failures."""
    if not files:
        return 0, []
    bash = shutil.which("bash")
    if bash is None:
        raise RunnerError("bash is not on PATH")
    failed = [f for f in files if not registry_lib.run_shell_test(root, bash, f)]
    return len(files) - len(failed), [f"FAILED {f}" for f in failed]


def suite_test_files(root: Path, registry: registry_lib.Registry, name: str) -> list[str]:
    files = registry_lib.suite_files(registry, registry_lib.tracked_files(root), name)
    return [f for f in files if (root / f).is_file()]


def run_suite(
    root: Path,
    registry: registry_lib.Registry,
    suite: registry_lib.Suite,
    options: argparse.Namespace,
) -> SuiteResult:
    """Build or reuse the venv, then run the suite's Python and shell tests."""
    if suite.not_local is not None:
        return SuiteResult(suite.name, "not-run", notes=[suite.not_local])
    owner = venv_owner(registry, suite)
    files = suite_test_files(root, registry, suite.name)
    env = _env_without_git()
    binary = find_vmaf_bin(root, options.vmaf_bin)
    if binary:
        env["VMAF_BIN"] = env["VMAF_BIN_FOR_TESTS"] = binary
    started = time.monotonic()
    if options.python_exe:
        python, venv_note = options.python_exe, "python override"
        suite_env = dict(env)
        suite_env["PATH"] = f"{Path(python).parent}{os.pathsep}{env.get('PATH', '')}"
        result, details = _run_in(root, suite, python, files, options, suite_env)
    else:
        cache = Path(options.venv_root)
        lock = VenvLock(cache / f"{owner.name}.lock", options.install_cap)
        with lock:
            venv, built = ensure_venv(root, owner, cache, options.install_cap)
            venv_note = "venv cached" if built is None else f"venv built in {built:.0f}s"
            suite_env = dict(env)
            suite_env["VIRTUAL_ENV"] = str(venv)
            suite_env["PATH"] = f"{venv / 'bin'}{os.pathsep}{env.get('PATH', '')}"
            result, details = _run_in(
                root, suite, str(venv_python(venv)), files, options, suite_env
            )
    result.seconds = time.monotonic() - started
    result.venv = venv_note
    result.notes = details
    return result


def _run_in(
    root: Path,
    suite: registry_lib.Suite,
    python: str,
    files: Sequence[str],
    options: argparse.Namespace,
    env: dict[str, str],
) -> tuple[SuiteResult, list[str]]:
    py_files = [f for f in files if f.endswith(".py")]
    sh_files = [f for f in files if f.endswith(".sh")]
    result, details = run_pytest(root, suite, python, py_files, options.time_cap, env)
    shell_passed, shell_failures = run_shell_files(root, sh_files)
    result.passed += shell_passed
    result.failed += len(shell_failures)
    if shell_failures:
        result.status = "failed"
    return result, details + shell_failures


def format_result(result: SuiteResult) -> str:
    if result.status == "not-run":
        return f"{result.name}: NOT RUN (cannot run on a workstation: {result.notes[0]})"
    verdict = "passed" if result.status == "passed" else "FAILED"
    return (
        f"{result.name}: {verdict}: {result.passed} passed, {result.failed} failed, "
        f"{result.skipped} skipped in {result.seconds:.1f}s ({result.venv})"
    )


def parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root")
    parser.add_argument("--base", help="base commit of the change")
    parser.add_argument("--head", help="head commit of the change")
    parser.add_argument("--files", help="changed files, comma or space separated ('-': stdin)")
    parser.add_argument("--list", action="store_true", help="print the affected suites only")
    parser.add_argument("--strict", action="store_true", help="a suite that cannot run fails")
    parser.add_argument("--time-cap", type=float, default=DEFAULT_TIME_CAP_SECONDS)
    parser.add_argument("--install-cap", type=float, default=DEFAULT_INSTALL_CAP_SECONDS)
    parser.add_argument(
        "--venv-root", default=os.environ.get("VMAFX_SUITE_VENVS", str(DEFAULT_CACHE))
    )
    parser.add_argument("--vmaf-bin", help="vmaf binary for suites that drive one")
    parser.add_argument(
        "--python-exe",
        default=os.environ.get("VMAFX_SUITE_PYTHON"),
        help="run in this interpreter instead of a cached venv (diagnostics)",
    )
    args = parser.parse_args(argv)
    if (args.base is None) != (args.head is None) or (args.base is None) == (args.files is None):
        parser.error("give --base with --head, or --files")
    return args


def collect_changed(options: argparse.Namespace) -> list[str]:
    if options.files is not None:
        text = sys.stdin.read() if options.files == "-" else options.files
        return parse_file_list(text)
    return changed_files(options.root, options.base, options.head)


def main(argv: Sequence[str] | None = None) -> int:
    options = parse_args(argv)
    try:
        registry = registry_lib.load_registry(options.root)
        affected = affected_suites(registry, collect_changed(options))
        if options.list or not affected:
            return _report_list(registry, affected)
        results = [
            run_suite(options.root, registry, s, options)
            for s in registry.suites
            if s.name in affected
        ]
    except (RunnerError, registry_lib.RegistryError) as exc:
        print(f"run-affected-suites: error: {exc}", file=sys.stderr)
        return 2
    return _report_results(results, options.strict)


def _report_list(registry: registry_lib.Registry, affected: dict[str, list[str]]) -> int:
    if not affected:
        print("run-affected-suites: no suite is affected by this change")
        return 0
    for suite in registry.suites:
        hits = affected.get(suite.name)
        if hits:
            where = "" if suite.not_local is None else f" [not runnable locally: {suite.not_local}]"
            print(f"{suite.name}: {len(hits)} changed file(s), e.g. {hits[0]}{where}")
    return 0


def _report_results(results: Sequence[SuiteResult], strict: bool) -> int:
    failed = False
    for result in results:
        print(format_result(result), flush=True)
        for note in result.notes if result.status == "failed" else []:
            print(f"    {note}", file=sys.stderr)
        failed |= result.status == "failed" or (strict and result.status == "not-run")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
