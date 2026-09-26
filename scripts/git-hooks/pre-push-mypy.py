#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Type-check branch-owned ai/scripts Python files, and fail only on new findings.

An old remote tip is not a PR base after a rebase: its diff includes unrelated
changes already integrated on master. Preserve the touched-file scope in AGENTS.md.

Three properties this wrapper owes its callers, all learned from failures:

* `ai/src` is on mypy's `mypy_path`, so a file under it has two possible module
  names, `aiutils.x` from that base and `ai.src.aiutils.x` from the repository
  root. The `exclude` entry in pyproject.toml only stops mypy from finding them
  while it crawls; a path named on the command line is still checked, and mypy
  then refuses outright with "Source file found twice under different module
  names". Every branch that edited a file under `ai/src/` hit that and could not
  be pushed at all. Those files get their own run with
  `--explicit-package-bases`, which names them from the `mypy_path` base alone,
  matching the module name they have at runtime.
* The findings a branch inherits are not its bug. The former raw
  `mypy ai/ scripts/` CI run was advisory because the stub coverage of NumPy,
  pandas, and PyTorch is uneven, and how much of it a checkout sees depends on
  which packages happen to be installed. A blocking gate that reported every
  inherited finding would fail on files the branch never touched. The same
  files are therefore checked at the selected merge base and only findings
  absent there are reported. Line numbers are left out of the comparison,
  because an edit above a finding shifts it without changing it. Required CI
  invokes this same gate and propagates its result.
* Installed third-party stubs are not a stable part of the repository's type
  contract. Their presence and supported Python syntax vary by checkout; for
  example, a newer NumPy stub can use syntax newer than this repository's
  configured mypy target and make the checker exit before reporting a source
  finding. Runs therefore exclude site packages and suppress only the missing
  imports that exclusion creates. Repository and standard-library types remain
  checked under the same hash-locked toolchain locally and in hosted CI.
"""

from __future__ import annotations

import configparser
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

import tomllib

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
ISOLATION_ARGS = ("--no-site-packages", "--disable-error-code=import-not-found")
CONFIG_FILES = ("pyproject.toml", "mypy.ini", ".mypy.ini", "setup.cfg")
BASE_REF_ENV = "VMAFX_MYPY_BASE_REF"


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


def checked_fingerprints(status: int, output: str, phase: str) -> set[str]:
    """Accept only mypy's success and ordinary-finding exit statuses."""
    found = fingerprints(output)
    if status not in (0, 1):
        detail = "without reporting a finding" if not found else "with a blocking error"
        raise RuntimeError(f"{phase} mypy exited {status} {detail}:\n{output.strip()}")
    if status == 1 and not found:
        raise RuntimeError(f"{phase} mypy exited 1 without reporting a finding:\n{output.strip()}")
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
            [executable, *ISOLATION_ARGS, *args, *group],
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
        if status in (0, 1):
            if completed.returncode in (0, 1):
                status = max(status, completed.returncode)
            else:
                status = completed.returncode
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
        for config in CONFIG_FILES:
            source = root / config
            target = worktree / config
            if source.is_file():
                shutil.copyfile(source, target)
            elif target.is_file():
                target.unlink()
        present = [path for path in paths if (worktree / path).is_file()]
        if not present:
            return set()
        status, output = run_mypy(executable, present, worktree)
        return checked_fingerprints(status, output, "baseline")
    finally:
        # --force: the checkout is untouched, but a failed mypy run must not
        # leave a registration behind for the ADR-0332 worktree-drift guard.
        # Routed through git() so the argument vector is built in one place;
        # a cleanup failure must not mask the finding the caller is reporting.
        try:
            git("-C", str(root), "worktree", "remove", "--force", str(worktree))
        except (OSError, RuntimeError, CommandFailed):
            print(f"mypy: could not remove the baseline worktree {worktree}", file=sys.stderr)


def extract_mypy_toml(content: str | None) -> dict[str, object] | None:
    if content is None:
        return None
    try:
        data = tomllib.loads(content)
    except Exception as exc:
        raise RuntimeError(f"pyproject.toml is not valid TOML: {exc}") from exc
    tool = data.get("tool")
    if isinstance(tool, dict):
        mypy = tool.get("mypy")
        if isinstance(mypy, dict):
            return mypy
    return None


def extract_mypy_cfg(content: str | None) -> str | None:
    if content is None or "[mypy" not in content:
        return None
    parser = configparser.ConfigParser()
    try:
        parser.read_string(content)
        sections = [s for s in parser.sections() if s == "mypy" or s.startswith("mypy-")]
        return "\n".join(
            f"[{s}]\n" + "\n".join(f"{k}={v}" for k, v in sorted(parser.items(s)))
            for s in sorted(sections)
        )
    except Exception:
        return content


def mypy_config_changed(root: Path, base: str, head: str) -> bool:
    diff = git(
        "-C",
        str(root),
        "diff",
        "--name-only",
        "-z",
        "--diff-filter=ACDMRT",
        base,
        head,
        "--",
        *CONFIG_FILES,
    )
    changed = [f for f in diff.split("\0") if f]
    if not changed:
        return False
    for filename in ("mypy.ini", ".mypy.ini"):
        if filename in changed:
            return True
    if "setup.cfg" in changed:
        try:
            base_cfg = git("-C", str(root), "show", f"{base}:setup.cfg")
        except (CommandFailed, RuntimeError, OSError):
            base_cfg = None
        target = root / "setup.cfg"
        head_cfg = target.read_text(encoding="utf-8") if target.is_file() else None
        if extract_mypy_cfg(base_cfg) != extract_mypy_cfg(head_cfg):
            return True
    if "pyproject.toml" in changed:
        try:
            base_toml = git("-C", str(root), "show", f"{base}:pyproject.toml")
        except (CommandFailed, RuntimeError, OSError):
            base_toml = None
        target = root / "pyproject.toml"
        head_toml = target.read_text(encoding="utf-8") if target.is_file() else None
        if extract_mypy_toml(base_toml) != extract_mypy_toml(head_toml):
            return True
    return False


def selected_paths(base: str, head: str, root: Path | None = None) -> list[str]:
    if root is None:
        root = Path(git("rev-parse", "--show-toplevel").strip()).resolve()
    if mypy_config_changed(root, base, head):
        tracked = git("-C", str(root), "ls-files", "-z", "--", "ai/", "scripts/")
        return sorted(
            path
            for path in tracked.split("\0")
            if path.startswith(("ai/", "scripts/")) and path.endswith(".py")
        )
    changed = git(
        "-C",
        str(root),
        "diff",
        "--name-only",
        "-z",
        "--diff-filter=ACMRT",
        base,
        head,
        "--",
        "ai/",
        "scripts/",
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
        base_ref = os.environ.get(BASE_REF_ENV, "origin/master").strip()
        if not base_ref:
            raise RuntimeError(f"{BASE_REF_ENV} must name a commit when set")
        base_tip = git(
            "rev-parse", "--verify", "--end-of-options", f"{base_ref}^{{commit}}"
        ).strip()
        base = git("merge-base", base_tip, head).strip()
        selected = selected_paths(base, head, root)
        verify_paths(selected, root)
        executable = shutil.which("mypy")
        if executable is None:
            raise RuntimeError("mypy is required; install the local type-checking toolchain")
        if not selected:
            print("mypy: no ai/scripts Python files differ from the selected merge base")
            return 0
        status, output = run_mypy(executable, selected, root)
        current = checked_fingerprints(status, output, "head")
        if not current:
            return 0
        inherited = baseline_fingerprints(executable, selected, root, base)
        return report(current - inherited, len(current & inherited))
    except (OSError, ValueError, RuntimeError, CommandFailed) as exc:
        print(f"mypy scope check failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
