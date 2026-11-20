#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Shared isolated repository fixture and helpers for check_bandit_new tests."""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_bandit_new as check
from scripts.ci.tests.scanner_test_support import (
    isolate_pre_commit_environment,
    run_push_entry,
)
from scripts.lib import git_env
from scripts.lib.safe_subprocess import run as run_command

BASE = "from pathlib import Path\n\nexe = Path('tool')\n"
UNSAFE = "import subprocess\n\nsubprocess.run([exe], check=False, timeout=5)  # noqa: S603\n"
SAFE = "from scripts.lib.safe_subprocess import run\n\nrun([exe], allowed_executables=(exe,))\n"


class BanditNewTestCase(unittest.TestCase):
    def setUp(self) -> None:
        """Require Bandit and create an isolated repository with a baseline and change branch."""
        isolate_pre_commit_environment(self)
        self.assertIsNotNone(shutil.which("bandit"), "bandit is not on PATH")
        temp = tempfile.TemporaryDirectory(prefix="vmafx-bandit-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.git("init", "-q", "-b", "master")
        (self.root / "harness.py").write_text(BASE, encoding="utf-8", newline="\n")
        (self.root / "bandit.yml").write_bytes(check.CONFIG.read_bytes())
        self.git("add", "harness.py", "bandit.yml")
        self.git("commit", "-q", "-m", "base")
        self.git("switch", "-q", "-c", "change")

    def git(self, *args: str, input_text: str | None = None) -> str:
        """Run a fixture repository command, assert success, and return standard output."""
        git = shutil.which("git")
        assert git is not None
        # Drop the caller's repository variables (a hook runs with GIT_DIR and
        # GIT_INDEX_FILE set) so the fixture never touches the caller's repository.
        env = {
            **git_env.hermetic(),
            "GIT_AUTHOR_NAME": "t",
            "GIT_AUTHOR_EMAIL": "t@example.invalid",
            "GIT_COMMITTER_NAME": "t",
            "GIT_COMMITTER_EMAIL": "t@example.invalid",
        }
        proc = run_command(
            [
                git,
                "-c",
                "commit.gpgsign=false",
                "-c",
                f"core.hooksPath={self.root / 'no-hooks'}",
                *args,
            ],
            allowed_executables=(git,),
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            timeout_seconds=60,
            input_data=input_text,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout

    def run_check(self, name: str, source: str) -> tuple[int, str, str]:
        """Write fixture source and return the check's status, standard output, and errors."""
        (self.root / name).write_text(source, encoding="utf-8", newline="\n")
        out, err = io.StringIO(), io.StringIO()
        cwd = Path.cwd()
        os.chdir(self.root)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                status = check.main(["--base-branch", "master", name])
        finally:
            os.chdir(cwd)
        return status, out.getvalue(), err.getvalue()

    def invoke(self, *names: str, base: str | None = "master") -> tuple[int, str, str]:
        """Check selected fixture paths and return status, standard output, and errors."""
        argv = [*(["--base-branch", base] if base else []), *names]
        out, err = io.StringIO(), io.StringIO()
        with (
            contextlib.chdir(self.root),
            contextlib.redirect_stdout(out),
            contextlib.redirect_stderr(err),
        ):
            status = check.main(argv)
        return status, out.getvalue(), err.getvalue()

    def commit_base(self) -> None:
        """Commit fixture changes and make that revision the comparison baseline."""
        self.git("add", ".")
        self.git("commit", "-q", "-m", "baseline")
        self.git("branch", "-f", "master", "HEAD")

    def outgoing_entry(
        self,
        *,
        from_ref: str | None = None,
        to_ref: str | None = None,
        extra: tuple[str, ...] = (),
    ) -> tuple[int, str, str]:
        """Invoke the parsed push entry using the fixture's default comparison branch."""
        self.git("branch", "-f", "origin/master", "master")
        return run_push_entry(self.root, "bandit", from_ref=from_ref, to_ref=to_ref, extra=extra)

    def commit_outgoing(self, name: str, source: str) -> None:
        """Commit one source fixture without staging the copied hook implementation."""
        (self.root / name).write_text(source, encoding="utf-8", newline="\n")
        self.git("add", name)
        self.git("commit", "-q", "-m", "outgoing source")

    @contextlib.contextmanager
    def count_bandit_runs(self) -> Any:
        """Wrap the real `run_command` in `check_bandit_new` and record `(cwd, argv)` per call."""
        calls: list[tuple[Path, tuple[str, ...]]] = []

        def wrapped(argv: Any, *args: Any, **kwargs: Any) -> Any:
            """Record the working directory and argument vector before delegating."""
            cwd = Path(kwargs["cwd"]) if kwargs.get("cwd") is not None else Path.cwd()
            calls.append((cwd, tuple(str(part) for part in argv)))
            return run_command(argv, *args, **kwargs)

        with mock.patch.object(check, "run_command", side_effect=wrapped):
            yield calls
