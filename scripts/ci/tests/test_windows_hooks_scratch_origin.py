# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Windows Lefthook job's scratch clone must see origin/master on a pull request.

The `windows-hooks` job of `.github/workflows/standards-gate.yml` runs the
pre-commit hooks in `git clone --shared . "$RUNNER_TEMP/scratch"`. A clone maps the
checkout's local branches to `origin/*`; a pull-request checkout is a detached merge
commit with no local `master`, so hooks that resolve `origin/master` failed on every
pull request. These tests run the step's Git commands, as written in the workflow,
against a checkout shaped like a pull-request checkout.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github" / "workflows" / "standards-gate.yml"
STEP_NAME = "- name: Test lefthook pre-commit in a scratch clone"
GIT = shutil.which("git")
BASH = shutil.which("bash")


def scratch_git_lines(text: str) -> list[str]:
    """The step's Git commands up to and including the `cd` into the scratch clone."""
    lines = text.splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip() == STEP_NAME)
    commands: list[str] = []
    for line in lines[start + 1 : start + 40]:
        stripped = line.strip()
        if stripped.startswith("git "):
            commands.append(stripped)
        if stripped.startswith('cd "$RUNNER_TEMP/scratch"'):
            return commands
    raise AssertionError("the scratch-clone step has no cd into the scratch clone")


def hook_free_environment() -> dict[str, str]:
    """The process environment without GIT_* (a hook's GIT_DIR / GIT_INDEX_FILE would point
    every git command, fixture setup and workflow step included, at the caller's repository)."""
    return {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


def git(cwd: Path, *args: str) -> str:
    assert GIT is not None
    env = {
        **hook_free_environment(),
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
    }
    return subprocess.run(  # noqa: S603 -- fixed git argv from the test's own fixture paths
        [GIT, "-C", str(cwd), "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
        check=True,
        capture_output=True,
        text=True,
        env=env,
        timeout=60,
    ).stdout.strip()


def pull_request_checkout(tmp: Path) -> Path:
    """A checkout with origin/master and a detached HEAD, but no local master."""
    upstream = tmp / "upstream"
    upstream.mkdir()
    git(upstream, "init", "-q", "-b", "master")
    (upstream / "f.txt").write_text("one\n")
    git(upstream, "add", "f.txt")
    git(upstream, "commit", "-q", "-m", "one")
    checkout = tmp / "checkout"
    git(tmp, "clone", "-q", str(upstream), str(checkout))
    git(checkout, "checkout", "-q", "--detach", "origin/master")
    git(checkout, "branch", "-q", "-D", "master")
    return checkout


def resolves_origin_master(scratch: Path) -> bool:
    assert GIT is not None
    result = subprocess.run(  # noqa: S603 -- fixed git argv on the test's scratch clone
        [GIT, "-C", str(scratch), "rev-parse", "--verify", "-q", "origin/master^{commit}"],
        capture_output=True,
        check=False,
        env=hook_free_environment(),
        timeout=60,
    )
    return result.returncode == 0


def run_step(checkout: Path, runner_temp: Path, commands: list[str]) -> Path:
    assert BASH is not None
    script = "set -euo pipefail\n" + "\n".join(commands) + "\n"
    subprocess.run(  # noqa: S603 -- runs the workflow step's own git lines in a temp fixture
        [BASH, "-c", script],
        cwd=checkout,
        check=True,
        capture_output=True,
        env={**hook_free_environment(), "RUNNER_TEMP": str(runner_temp)},
        timeout=120,
    )
    return runner_temp / "scratch"


@unittest.skipIf(GIT is None or BASH is None, "needs git and bash")
class ScratchCloneSeesOriginMaster(unittest.TestCase):
    def setUp(self) -> None:
        self.commands = scratch_git_lines(WORKFLOW.read_text(encoding="utf-8"))

    def test_step_fetches_the_checkouts_remote_tracking_refs(self) -> None:
        self.assertTrue(
            any("refs/remotes/origin/*:refs/remotes/origin/*" in c for c in self.commands),
            self.commands,
        )

    def test_pull_request_checkout_gives_origin_master_in_scratch(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            checkout = pull_request_checkout(tmp)
            (tmp / "runner").mkdir()
            scratch = run_step(checkout, tmp / "runner", self.commands)
            self.assertTrue(resolves_origin_master(scratch))

    def test_without_the_fetch_origin_master_is_missing(self) -> None:
        """Planted defect: the step as it was before the fix."""
        without = [c for c in self.commands if "refs/remotes/origin/*" not in c]
        self.assertNotEqual(without, self.commands)
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            checkout = pull_request_checkout(tmp)
            (tmp / "runner").mkdir()
            scratch = run_step(checkout, tmp / "runner", without)
            self.assertFalse(resolves_origin_master(scratch))


if __name__ == "__main__":
    unittest.main()
