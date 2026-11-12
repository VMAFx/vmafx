# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Compare scanner findings with the merge-base copy of a changed file."""

from __future__ import annotations

import argparse
import collections
import io
import os
import re
import shutil
import tempfile
import tokenize
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from scripts.lib.safe_subprocess import run as run_command

Finding = tuple[int, str, str]
# Covers tracked .py (<= 220,626 B), C/C++ (<= 841,275 B), and untagged blobs
# copied before shebang inspection (<= 30,810,303 B); git_bytes shares it.
MAX_SOURCE_BYTES = 64 * 1_048_576


class ToolError(RuntimeError):
    """A scanner or git is missing or failed."""


def tool(name: str) -> str:
    """Return the executable path or raise ToolError when it is unavailable."""
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
        max_output_bytes=MAX_SOURCE_BYTES,
    )
    return proc.returncode, proc.stdout


def git_output(git: str, args: list[str], cwd: Path) -> tuple[int, str]:
    """Return the command exit status and filesystem-decoded standard output."""
    status, output = git_bytes(git, args, cwd)
    return status, os.fsdecode(output)


def default_branch(git: str, root: Path) -> str:
    """Return the origin HEAD target, falling back to origin/master if unresolved."""
    status, output = git_output(git, ["symbolic-ref", "refs/remotes/origin/HEAD"], root)
    return output.strip() if status == 0 else "origin/master"


def merge_base(git: str, root: Path, branch: str, ref: str) -> str:
    """Return the common ancestor of branch and the outgoing ref or HEAD; raise if absent."""
    ref = ref or "HEAD"
    status, output = git_output(git, ["merge-base", ref, branch], root)
    if status != 0 or not output.strip():
        raise ToolError(f"no merge base between {ref} and {branch}")
    return output.strip()


def renames(git: str, root: Path, base: str, ref: str) -> dict[str, str]:
    """Map each path renamed since base to its original path."""
    target = [ref] if ref else []
    status, output = git_output(
        git, ["diff", "-M", "--name-status", "-z", "--diff-filter=R", base, *target], root
    )
    if status != 0:
        raise ToolError(f"git diff against {base} failed")
    fields = output.split("\0")
    return {fields[index + 2]: fields[index + 1] for index in range(0, len(fields) - 2, 3)}


def revision_modes(git: str, root: Path, ref: str) -> dict[str, str]:
    """Read bounded tree output, accepting only well-formed supported mode/type pairs."""
    status, output = git_output(git, ["ls-tree", "-r", "-z", ref], root)
    if status != 0:
        raise ToolError(f"git ls-tree against {ref} failed")
    if output and not output.endswith("\0"):
        raise ToolError(f"unterminated git ls-tree record against {ref}")
    modes = {}
    pattern = (
        r"(100644 blob|100755 blob|120000 blob|160000 commit) [0-9a-f]{40}(?:[0-9a-f]{24})?\t(.+)"
    )
    for record in output.split("\0")[:-1]:
        match = re.fullmatch(pattern, record, flags=re.DOTALL)
        if match is None:
            raise ToolError(f"malformed git ls-tree record against {ref}")
        mode_type, name = match.groups()
        if name in modes:
            raise ToolError(f"duplicate git ls-tree path against {ref}: {name}")
        modes[name] = mode_type.split(" ", 1)[0]
    return modes


def revision_bytes(
    git: str, root: Path, ref: str, relative: str, *, modes: dict[str, str] | None = None
) -> bytes | None:
    """Read a literal regular blob; absent paths, symlinks and gitlinks return None."""
    entries = revision_modes(git, root, ref) if modes is None else modes
    if entries.get(relative) not in {"100644", "100755"}:
        return None
    status, output = git_bytes(git, ["--literal-pathspecs", "show", f"{ref}:{relative}"], root)
    if status != 0:
        raise ToolError(f"git show {ref}:{relative} failed")
    return output


def base_text(
    git: str, root: Path, base: str, relative: str, *, modes: dict[str, str] | None = None
) -> bytes:
    """Return regular base bytes; absent paths, symlinks and gitlinks count as empty."""
    return revision_bytes(git, root, base, relative, modes=modes) or b""


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
    ref: str = field(default_factory=lambda: os.environ.get("PRE_COMMIT_TO_REF") or "")
    _modes: dict[str, dict[str, str]] = field(default_factory=dict, init=False, repr=False)

    def __post_init__(self) -> None:
        """Resolve the outgoing revision, merge base and detected rename mapping."""
        self.base = merge_base(
            self.git, self.root, self.branch or default_branch(self.git, self.root), self.ref
        )
        self.moved = renames(self.git, self.root, self.base, self.ref)

    def current_bytes(self, relative: str) -> bytes | None:
        """Return bounded regular outgoing or local bytes, skipping links and local escapes."""
        if self.ref:
            return revision_bytes(
                self.git, self.root, self.ref, relative, modes=self.modes(self.ref)
            )
        path = self.root / relative
        if path.is_symlink():
            return None
        if not path.resolve().is_relative_to(self.root.resolve()):
            raise ToolError(f"source path escapes repository: {relative}")
        if not path.is_file():
            return None
        if path.stat().st_size > MAX_SOURCE_BYTES:
            raise ToolError(f"{path} exceeds {MAX_SOURCE_BYTES} bytes")
        with path.open("rb") as handle:
            content = handle.read(MAX_SOURCE_BYTES + 1)
        if len(content) > MAX_SOURCE_BYTES:
            raise ToolError(f"{path} exceeds {MAX_SOURCE_BYTES} bytes")
        return content

    def old_bytes(self, relative: str) -> bytes:
        """Return original regular base bytes; missing paths and links count as empty."""
        return base_text(
            self.git,
            self.root,
            self.base,
            self.moved.get(relative, relative),
            modes=self.modes(self.base),
        )

    def modes(self, ref: str) -> dict[str, str]:
        """Cache one validated mode listing per selected revision."""
        if ref not in self._modes:
            self._modes[ref] = revision_modes(self.git, self.root, ref)
        return self._modes[ref]

    def outgoing_paths(self) -> list[Path]:
        """List added or modified outgoing paths across the complete merge-base delta."""
        status, output = git_output(
            self.git,
            ["diff", "-M", "--name-only", "-z", "--diff-filter=ACMRT", self.base, self.ref],
            self.root,
        )
        if status != 0:
            raise ToolError(f"git diff against {self.base} failed")
        return [Path(name) for name in output.split("\0") if name]

    def tracked_paths(self) -> list[Path]:
        """List tracked outgoing paths with cached modes, or retain local index selection."""
        if self.ref:
            return [Path(name) for name in self.modes(self.ref)]
        status, output = git_output(self.git, ["ls-files", "-z"], self.root)
        if status != 0:
            raise ToolError("cannot list tracked source files")
        return [Path(name) for name in output.split("\0") if name]

    def copy(self, path: Path, directory: Path) -> tuple[str, Path] | None:
        """Copy available source bytes for scanning and return the relative path and copy path."""
        if ".." in path.parts:
            raise ToolError(f"source path escapes repository: {path}")
        relative = path.absolute().relative_to(self.root.resolve()).as_posix()
        content = self.current_bytes(relative)
        if content is None:
            return None
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        return relative, target


def comparison_arguments(
    parser: argparse.ArgumentParser, argv: list[str] | None, scanner: str
) -> tuple[str, Comparison, list[Path]]:
    """Parse shared scanner options and select positional or complete outgoing paths."""
    parser.add_argument("--base-branch")
    parser.add_argument(
        "--outgoing", action="store_true", help="scan the outgoing merge-base delta"
    )
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args(argv)
    if args.outgoing and args.files:
        parser.error("--outgoing cannot be combined with positional files")
    executable = tool(scanner)
    ref = os.environ.get("PRE_COMMIT_TO_REF") or ("HEAD" if args.outgoing else "")
    comparison = Comparison(tool("git"), Path.cwd(), args.base_branch, ref=ref)
    paths = comparison.outgoing_paths() if args.outgoing else args.files
    return executable, comparison, paths


def added_findings(current: list[Finding], old: list[Finding]) -> list[Finding]:
    """Return findings in `current` absent from the `(rule, line)` multiset of `old`."""
    old_keys = collections.Counter((rule, line) for _, rule, line in old)
    added: list[Finding] = []
    for found in current:
        key = (found[1], found[2])
        if old_keys[key] > 0:
            old_keys[key] -= 1
        else:
            added.append(found)
    return added


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
        old_found = (old_scan or scan)(old)
    return added_findings(scan(current), old_found)
