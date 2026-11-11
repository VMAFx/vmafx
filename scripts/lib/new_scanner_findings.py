# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Compare scanner findings with the merge-base copy of a changed file."""

from __future__ import annotations

import collections
import io
import os
import shutil
import tempfile
import tokenize
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from scripts.lib.safe_subprocess import run as run_command

Finding = tuple[int, str, str]


class ToolError(RuntimeError):
    """A scanner or git is missing or failed."""


def tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise ToolError(f"{name} is not on PATH")
    return path


def git_bytes(git: str, args: list[str], cwd: Path) -> tuple[int, bytes]:
    """Run a read-only git command in the selected repository."""
    # Read-only git in `cwd`; the hook's GIT_DIR / GIT_INDEX_FILE are dropped so
    # `cwd` alone names the repository.
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    proc = run_command(
        [git, *args],
        allowed_executables=(git,),
        cwd=cwd,
        env=env,
        capture_output=True,
        text=False,
        timeout_seconds=60,
        max_output_bytes=64 * 1_048_576,
    )
    return proc.returncode, proc.stdout


def git_output(git: str, args: list[str], cwd: Path) -> tuple[int, str]:
    status, output = git_bytes(git, args, cwd)
    return status, os.fsdecode(output)


def default_branch(git: str, root: Path) -> str:
    status, output = git_output(git, ["symbolic-ref", "refs/remotes/origin/HEAD"], root)
    return output.strip() if status == 0 else "origin/master"


def merge_base(git: str, root: Path, branch: str) -> str:
    ref = os.environ.get("PRE_COMMIT_TO_REF") or "HEAD"
    status, output = git_output(git, ["merge-base", ref, branch], root)
    if status != 0 or not output.strip():
        raise ToolError(f"no merge base between {ref} and {branch}")
    return output.strip()


def renames(git: str, root: Path, base: str) -> dict[str, str]:
    """Map each path renamed since base to its original path."""
    target = [os.environ["PRE_COMMIT_TO_REF"]] if os.environ.get("PRE_COMMIT_TO_REF") else []
    status, output = git_output(
        git, ["diff", "-M", "--name-status", "-z", "--diff-filter=R", base, *target], root
    )
    if status != 0:
        raise ToolError(f"git diff against {base} failed")
    fields = output.split("\0")
    return {fields[index + 2]: fields[index + 1] for index in range(0, len(fields) - 2, 3)}


def revision_bytes(git: str, root: Path, ref: str, relative: str) -> bytes | None:
    """Read one literal blob; absence differs from an empty file."""
    status, names = git_output(
        git, ["--literal-pathspecs", "ls-tree", "-z", "--name-only", ref, "--", relative], root
    )
    if status != 0:
        raise ToolError(f"git ls-tree against {ref} failed")
    if relative not in names.split("\0"):
        return None
    status, output = git_bytes(git, ["--literal-pathspecs", "show", f"{ref}:{relative}"], root)
    if status != 0:
        raise ToolError(f"git show {ref}:{relative} failed")
    return output


def base_text(git: str, root: Path, base: str, relative: str) -> bytes:
    return revision_bytes(git, root, base, relative) or b""


def python_text(content: bytes) -> str:
    """Decode Python source using its BOM or PEP 263 encoding cookie."""
    encoding, _ = tokenize.detect_encoding(io.BytesIO(content).readline)
    return content.decode(encoding)


@dataclass
class Comparison:
    """One shared revision selection for Bandit and flawfinder."""

    git: str
    root: Path
    branch: str | None
    base: str = field(init=False)
    moved: dict[str, str] = field(init=False)
    ref: str = field(init=False)

    def __post_init__(self) -> None:
        self.ref = os.environ.get("PRE_COMMIT_TO_REF") or ""
        self.base = merge_base(
            self.git, self.root, self.branch or default_branch(self.git, self.root)
        )
        self.moved = renames(self.git, self.root, self.base)

    def current_bytes(self, relative: str) -> bytes | None:
        if self.ref:
            return revision_bytes(self.git, self.root, self.ref, relative)
        path = self.root / relative
        if path.is_symlink():
            return None
        if not path.resolve().is_relative_to(self.root.resolve()):
            raise ToolError(f"source path escapes repository: {relative}")
        return path.read_bytes() if path.is_file() else None

    def old_bytes(self, relative: str) -> bytes:
        return base_text(self.git, self.root, self.base, self.moved.get(relative, relative))

    def tracked_paths(self) -> list[Path]:
        args = ["ls-tree", "-r", "-z", "--name-only", self.ref] if self.ref else ["ls-files", "-z"]
        status, output = git_output(self.git, args, self.root)
        if status != 0:
            raise ToolError("cannot list tracked source files")
        return [Path(name) for name in output.split("\0") if name]

    def copy(self, path: Path, directory: Path) -> tuple[str, Path] | None:
        if ".." in path.parts:
            raise ToolError(f"source path escapes repository: {path}")
        relative = path.absolute().relative_to(self.root.resolve()).as_posix()
        content = self.current_bytes(relative)
        if content is None:
            return None
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        # Preserve shebang classification even when outgoing files are absent locally.
        target.chmod(0o700)
        return relative, target


def new_findings(
    scan: Callable[[Path], list[Finding]],
    current: Path,
    old_text: str | bytes,
    *,
    old_scan: Callable[[Path], list[Finding]] | None = None,
    force: bool = False,
) -> list[Finding]:
    """Return findings absent at base; skip both scans when the bytes match.

    Bytes, not decoded text: a file that is not UTF-8 still reaches the scanner.
    """
    old_bytes = old_text.encode("utf-8") if isinstance(old_text, str) else old_text
    if not force and current.read_bytes() == old_bytes:
        return []
    with tempfile.TemporaryDirectory(prefix="scanner-base-") as temp:
        old = Path(temp) / current.name
        old.write_bytes(old_bytes)
        old_keys = collections.Counter((rule, line) for _, rule, line in (old_scan or scan)(old))
    added = []
    for found in scan(current):
        key = (found[1], found[2])
        if old_keys[key] > 0:
            old_keys[key] -= 1
        else:
            added.append(found)
    return added
