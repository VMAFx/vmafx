# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The definition as committed at another revision, read through git.

`Unavailable` carries the reason a comparison cannot run (no git, not a
checkout, an unknown ref, a shallow clone without the merge base, no
definition at that revision); the CLI reports it and exits 77, Meson's skip
code, so a gate that did not run is never reported as passing.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import tomllib

from .loader import parse
from .model import Api

GIT_TIMEOUT = 60  # seconds; HISS-02 bounds every external call


class Unavailable(RuntimeError):
    """The earlier definition cannot be read; the message says why."""


def _git(root: Path, *args: str) -> str:
    git = shutil.which("git")
    if git is None:
        raise Unavailable("git is not on PATH")
    try:
        done = subprocess.run(  # noqa: S603 -- resolved git, fixed argv
            [git, "-C", str(root), *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=GIT_TIMEOUT,
        )
    except subprocess.TimeoutExpired as err:
        raise Unavailable(f"git {args[0]} timed out after {GIT_TIMEOUT} s") from err
    if done.returncode != 0:
        raise Unavailable(f"git {' '.join(args)}: {done.stderr.strip() or 'failed'}")
    return done.stdout


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
