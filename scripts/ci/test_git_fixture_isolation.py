#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run Git fixture helpers under caller redirection without touching a real repo."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"
VARIABLES = (
    "GIT_DIR",
    "GIT_COMMON_DIR",
    "GIT_WORK_TREE",
    "GIT_INDEX_FILE",
    "GIT_CONFIG_PARAMETERS",
)


def snapshot(directory: Path) -> dict[str, bytes]:
    """Capture config, refs, object store, index and work without refreshing Git."""
    return {
        path.relative_to(directory).as_posix(): path.read_bytes()
        for path in directory.rglob("*")
        if path.is_file()
    }


class GitFixtureIsolation(unittest.TestCase):
    def exercise(self, helper: str, variables: tuple[str, ...]) -> None:
        with tempfile.TemporaryDirectory(prefix="git-fixture-isolation-") as directory:
            root = Path(directory)
            caller = root / "caller"
            caller.mkdir()
            temporary = root / "scratch"
            temporary.mkdir()
            # Even the adversarial test's setup must never inherit a real
            # caller's repository, index, object store or config overrides.
            clean = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
            clean.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, LC_ALL="C")

            def git(*args: str) -> None:
                subprocess.run(  # noqa: S603 -- disposable caller, isolated environment
                    [GIT, "-C", str(caller), *args],
                    env=clean,
                    text=True,
                    capture_output=True,
                    check=True,
                )

            git("init", "-q", "-b", "master")
            git("config", "user.name", "Isolation Test")
            git("config", "user.email", "fixture@example.invalid")
            (caller / "tracked").write_text("committed caller work\n")
            git("add", "tracked")
            git("commit", "-qm", "test: caller baseline")
            (caller / "staged").write_text("unique staged caller work\n")
            git("add", "staged")
            (caller / "tracked").write_text("unique unstaged caller work\n")
            poison = {
                "GIT_DIR": str(caller / ".git"),
                "GIT_COMMON_DIR": str(caller / ".git"),
                "GIT_WORK_TREE": str(caller),
                "GIT_INDEX_FILE": str(caller / ".git/index"),
                "GIT_CONFIG_PARAMETERS": "'core.bare=true' 'fixture.marker=inherited'",
            }
            environment = {
                **clean,
                **{key: poison[key] for key in variables},
                "TMPDIR": str(temporary),
            }
            before = snapshot(caller)
            executable = sys.executable if helper.endswith(".py") else BASH
            result = subprocess.run(  # noqa: S603 -- shipped fixture; poison points only inside temporary root
                [executable, str(ROOT / helper)],
                cwd=root,
                env=environment,
                text=True,
                capture_output=True,
                timeout=90,
                check=False,
            )
            after = snapshot(caller)
            changed = sorted(
                key for key in before.keys() | after.keys() if before.get(key) != after.get(key)
            )
            self.assertEqual(
                changed, [], f"{helper} changed caller files under {variables}: {changed}"
            )
            self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-2000:])

    def check_helper(self, helper: str) -> None:
        for variables in (*((variable,) for variable in VARIABLES), VARIABLES):
            with self.subTest(helper=helper, variables=variables):
                self.exercise(helper, variables)

    def test_replay_fixture(self) -> None:
        self.check_helper("scripts/ci/test_ffmpeg_patch_stack.py")

    def test_smoke_fixture(self) -> None:
        self.check_helper("scripts/ci/test_ffmpeg_patch_smoke_safety.py")

    def test_cleanup_fixture(self) -> None:
        self.check_helper("scripts/dev/test-cleanup-agent-state.sh")

    def test_dependency_classifier_fixture(self) -> None:
        self.check_helper("scripts/ci/test-classify-dependency-pr.sh")

    def test_local_and_required_ci_hook_registration(self) -> None:
        config = (ROOT / ".pre-commit-config.yaml").read_text()
        hook = config.split("      - id: test-git-fixture-isolation\n", 1)[1].split(
            "      - id:", 1
        )[0]
        self.assertIn("entry: python3 scripts/ci/test_git_fixture_isolation.py", hook)
        self.assertIn("stages: [pre-commit, pre-push]", hook)
        self.assertIn("pass_filenames: false", hook)
        pattern = hook.split("files: '", 1)[1].split("'", 1)[0]
        for path in (
            "scripts/ci/test_git_fixture_isolation.py",
            "scripts/ci/test_ffmpeg_patch_stack.py",
            "scripts/ci/test_ffmpeg_patch_smoke_safety.py",
            "scripts/dev/test-cleanup-agent-state.sh",
            "scripts/ci/test-classify-dependency-pr.sh",
            ".pre-commit-config.yaml",
        ):
            self.assertIsNotNone(re.search(pattern, path), path)
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text()
        self.assertIn("name: Pre-Commit", workflow)
        self.assertIn("pre-commit run --show-diff-on-failure --color=always --all-files", workflow)


if __name__ == "__main__":
    unittest.main()
