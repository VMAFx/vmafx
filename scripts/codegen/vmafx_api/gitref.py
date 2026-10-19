# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The definition as committed at another revision, read through git.

`Unavailable` carries the reason a comparison cannot run (no git, not a
checkout, an unknown ref, a shallow clone without the merge base, no
definition at that revision); the CLI reports it and exits 77, Meson's skip
code, so a gate that did not run is never reported as passing. `tree_at`
extracts a directory as committed (the chart a migration test renders).
"""

from __future__ import annotations

import io
import shutil
import subprocess
import tarfile
from pathlib import Path

import tomllib

from .loader import parse
from .model import Api

GIT_TIMEOUT = 60  # seconds; HISS-02 bounds every external call


class Unavailable(RuntimeError):
    """The earlier definition cannot be read; the message says why."""


def _git_bytes(root: Path, *args: str) -> bytes:
    git = shutil.which("git")
    if git is None:
        raise Unavailable("git is not on PATH")
    try:
        done = subprocess.run(  # noqa: S603 -- resolved git, fixed argv
            [git, "-C", str(root), *args],
            check=False,
            capture_output=True,
            timeout=GIT_TIMEOUT,
        )
    except subprocess.TimeoutExpired as err:
        raise Unavailable(f"git {args[0]} timed out after {GIT_TIMEOUT} s") from err
    if done.returncode != 0:
        reason = done.stderr.decode("utf-8", "replace").strip() or "failed"
        raise Unavailable(f"git {' '.join(args)}: {reason}")
    return done.stdout


def _git(root: Path, *args: str) -> str:
    """Text output with universal newlines, as `subprocess.run(text=True)` reads it."""
    text = _git_bytes(root, *args).decode("utf-8")
    return text.replace("\r\n", "\n").replace("\r", "\n")


def merge_base(root: Path, ref: str) -> str:
    """Merge base of HEAD and `ref`."""
    _git(root, "rev-parse", "--is-inside-work-tree")
    _git(root, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
    return _git(root, "merge-base", "HEAD", ref).strip()


def definition_at(root: Path, ref: str, path: Path) -> Api:
    """The definition committed at `ref`."""
    try:
        text = _git(root, "show", f"{ref}:{path.as_posix()}")
    except Unavailable as err:
        raise Unavailable(f"no {path.as_posix()} at {ref}: {err}") from err
    return parse(tomllib.loads(text))


def files_at(root: Path, ref: str, directory: str, suffix: str) -> dict[str, str]:
    """{path: text} of the files directly under `directory` at `ref` ending in `suffix`."""
    listing = _git(root, "ls-tree", "--name-only", ref, "--", directory.rstrip("/") + "/")
    paths = [p for p in listing.splitlines() if p.endswith(suffix)]
    return {path: _git(root, "show", f"{ref}:{path}") for path in paths}


def tree_at(root: Path, ref: str, directory: str, dest: Path) -> Path:
    """Extract `directory` as committed at `ref` into `dest`; its path there."""
    data = _git_bytes(root, "archive", "--format=tar", ref, "--", directory)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        archive.extractall(dest, filter="data")
    return dest / directory
