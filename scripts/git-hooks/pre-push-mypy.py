#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Type-check branch-owned ai/scripts Python files, and fail only on new findings.

An old remote tip is not a PR base after a rebase: its diff includes unrelated
changes already integrated on master. Preserve the touched-file scope in AGENTS.md.

Two properties this wrapper owes its callers, both learned from failures:

* `ai/src` is on mypy's `mypy_path`, so a file under it has two possible module
  names, `aiutils.x` from that base and `ai.src.aiutils.x` from the repository
  root. The `exclude` entry in pyproject.toml only stops mypy from finding them
  while it crawls; a path named on the command line is still checked, and mypy
  then refuses outright with "Source file found twice under different module
  names". Every branch that edited a file under `ai/src/` hit that and could not
  be pushed at all. Those files get their own run with
  `--explicit-package-bases`, which names them from the `mypy_path` base alone,
  matching the module name they have at runtime.
* The findings a branch inherits are not its bug. `mypy ai/ scripts/` in CI is
  advisory by design (`|| echo "mypy advisory only on first run"` in
  `.github/workflows/lint-and-format.yml`) because the stub coverage of numpy,
  pandas and torch is uneven, and how much of it a checkout sees depends on
  which of those packages happen to be installed. A blocking local hook that
  reported every inherited finding failed on files the branch never touched. So
  the same files are checked at the branch's merge base and only findings that
  are not there too are reported. Line numbers are left out of the comparison,
  because an edit above a finding shifts it without changing it.
"""

from __future__ import annotations

import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandFailed
from scripts.lib.safe_subprocess import run as run_command

# mypy writes `path:line: error: message  [code]`; `note:` lines are context.
FINDING_RE = re.compile(
    r"^(?P<path>[^:]+):\d+: error: (?P<message>.*?)(?:\s+\[(?P<code>[^\]]+)\])?$"
)
# ai/src is a mypy_path base: see the module docstring.
PACKAGE_BASE_PREFIX = "ai/src/"
BASELINE_DIR_PREFIX = "vmafx-mypy-baseline-"


def git(*args: str) -> str:
    executable = shutil.which("git")
    if executable is None:
        raise RuntimeError("git is required")
    result = run_command(
        [executable, *args],
        allowed_executables=(executable,),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=120,
    )
    assert isinstance(result.stdout, str)
    return result.stdout


def fingerprints(output: str) -> set[str]:
    """Findings without line numbers, so an edit above one does not rename it."""
    found = set()
    for line in output.splitlines():
        match = FINDING_RE.match(line.strip())
        if match:
            code = match.group("code") or "no-code"
            found.add(f"{match.group('path')}: [{code}] {match.group('message')}")
    return found


def run_mypy(executable: str, paths: list[str], cwd: Path) -> tuple[int, str]:
    """One mypy invocation per module-naming rule; see the module docstring."""
    based = [path for path in paths if path.startswith(PACKAGE_BASE_PREFIX)]
    plain = [path for path in paths if not path.startswith(PACKAGE_BASE_PREFIX)]
    status = 0
    output = ""
    for args, group in ((["--explicit-package-bases"], based), ([], plain)):
        if not group:
            continue
        completed = run_command(
            [executable, *args, *group],
            allowed_executables=(executable,),
            cwd=cwd,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=1800,
            max_output_bytes=64 * 1_048_576,
        )
        assert isinstance(completed.stdout, str)
        assert isinstance(completed.stderr, str)
        status = status or completed.returncode
        output += completed.stdout + completed.stderr
    return status, output


def baseline_fingerprints(executable: str, paths: list[str], root: Path, base: str) -> set[str]:
    """The same files as they are at the merge base, checked out on the side."""
    cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    cache.mkdir(parents=True, exist_ok=True)
    worktree = Path(tempfile.mkdtemp(prefix=BASELINE_DIR_PREFIX, dir=cache))
    # mkdtemp created it; `git worktree add` wants to create it itself.
    worktree.rmdir()
    try:
        git("-C", str(root), "worktree", "add", "--detach", "--quiet", str(worktree), base)
        # The comparison is only meaningful if both sides are checked under the
        # SAME rules. The baseline checkout carries the merge base's
        # pyproject.toml, so a branch that changes [tool.mypy] -- raising
        # python_version, say -- makes mypy behave differently on the two sides
        # and every newly-visible finding is misattributed to the branch. At
        # python_version 3.10 mypy aborts outright on numpy's PEP 695 `type`
        # statement and checks zero files, so the baseline comes back empty and
        # the branch appears to introduce everything.
        # Copy the branch's type-check configuration over the baseline's before
        # measuring. The FILES stay at the merge base, which is what the gate is
        # comparing; only the rules are held constant.
        for config in ("pyproject.toml", "mypy.ini", ".mypy.ini", "setup.cfg"):
            source = root / config
            if source.is_file():
                shutil.copyfile(source, worktree / config)
        present = [path for path in paths if (worktree / path).is_file()]
        if not present:
            return set()
        _, output = run_mypy(executable, present, worktree)
        return fingerprints(output)
    finally:
        # --force: the checkout is untouched, but a failed mypy run must not
        # leave a registration behind for the ADR-0332 worktree-drift guard.
        # Routed through git() so the argument vector is built in one place;
        # a cleanup failure must not mask the finding the caller is reporting.
        try:
            git("-C", str(root), "worktree", "remove", "--force", str(worktree))
        except (OSError, RuntimeError, CommandFailed):
            print(f"mypy: could not remove the baseline worktree {worktree}", file=sys.stderr)


def selected_paths(base: str, head: str) -> list[str]:
    changed = git(
        "diff", "--name-only", "-z", "--diff-filter=ACMRT", base, head, "--", "ai/", "scripts/"
    )
    return sorted(
        path
        for path in changed.split("\0")
        if path.startswith(("ai/", "scripts/")) and path.endswith(".py")
    )


def verify_paths(selected: list[str], root: Path) -> None:
    for filename in selected:
        # Keep Git's lexical filename as the identity passed to mypy.
        # Resolving for selection would silently omit newly added symlinks
        # to unchanged files. Resolution is only a containment/type check.
        path = (root / filename).resolve(strict=True)
        path.relative_to(root)
        if not path.is_file():
            raise RuntimeError(f"selected Python path is not a regular file: {filename}")


def outgoing_head(head: str) -> None:
    """The all-files first-push path exposes only LOCAL_BRANCH; normal runs TO_REF."""
    outgoing = os.environ.get("PRE_COMMIT_TO_REF", os.environ.get("PRE_COMMIT_LOCAL_BRANCH"))
    if outgoing is None:
        return
    target = git("rev-parse", "--verify", "--end-of-options", f"{outgoing}^{{commit}}").strip()
    if target != head:
        raise RuntimeError("outgoing ref differs from HEAD; check out the branch being pushed")


def report(introduced: set[str], inherited: int) -> int:
    if not introduced:
        print(f"mypy: no new findings ({inherited} inherited from the merge base, not reported)")
        return 0
    print(f"mypy: {len(introduced)} finding(s) this branch introduces:", file=sys.stderr)
    for finding in sorted(introduced):
        print(f"  {finding}", file=sys.stderr)
    print(
        "\nFindings already present at the merge base are not listed. Fixing them is\n"
        "welcome, but this gate only asks that the branch add none.",
        file=sys.stderr,
    )
    return 1


def main() -> int:
    try:
        root = Path(git("rev-parse", "--show-toplevel").strip()).resolve()
        head = git("rev-parse", "--verify", "HEAD^{commit}").strip()
        outgoing_head(head)
        base = git("merge-base", "origin/master", head).strip()
        selected = selected_paths(base, head)
        verify_paths(selected, root)
        executable = shutil.which("mypy")
        if executable is None:
            raise RuntimeError("mypy is required; install the local type-checking toolchain")
        if not selected:
            print("mypy: no ai/scripts Python files differ from the branch's master merge base")
            return 0
        status, output = run_mypy(executable, selected, root)
        current = fingerprints(output)
        if not current:
            # A non-zero status with nothing to attribute is mypy itself
            # failing, not a clean run: fail closed rather than wave it through.
            if status == 0:
                return 0
            raise RuntimeError(
                f"mypy exited {status} without reporting a finding:\n{output.strip()}"
            )
        inherited = baseline_fingerprints(executable, selected, root, base)
        return report(current - inherited, len(current & inherited))
    except (OSError, ValueError, RuntimeError, CommandFailed) as exc:
        print(f"mypy scope check failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
