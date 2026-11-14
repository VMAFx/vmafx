# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Harness fixtures and helpers for upstream PR sweep tests."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.dev import upstream_pr_sweep as sweep_mod
from scripts.lib import git_env
from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[3]


def require_fixture_tools() -> None:
    if shutil.which("ssh-keygen") is None:
        raise unittest.SkipTest("ssh-keygen is required for signed commit fixture")
    if shutil.which("git") is None:
        raise unittest.SkipTest("git is required for repository fixtures")


def git_run(cwd: Path, *args: str) -> str:
    git_bin = shutil.which("git")
    assert git_bin is not None
    res = run_command(
        [git_bin, *args],
        allowed_executables=(git_bin,),
        cwd=cwd,
        env=sweep_mod.git_environment(),
        capture_output=True,
        text=True,
        timeout_seconds=60.0,
    )
    if res.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {res.stderr}")
    return res.stdout.strip()


FIXTURE_SETTINGS = (
    ("gpg.format", "ssh"),
    ("commit.gpgsign", "true"),
    ("init.defaultBranch", "master"),
)


def fixture_environment(root: Path) -> dict[str, str]:
    """Process environment for one test: no caller GIT_*, identity and signing by env only."""
    clean = git_env.drop()
    settings = (
        *FIXTURE_SETTINGS,
        ("user.signingkey", str(root / "id_ed25519")),
        ("gpg.ssh.allowedSignersFile", str(root / "allowed_signers")),
    )
    clean.update(
        GIT_CONFIG_GLOBAL=os.devnull,
        GIT_CONFIG_NOSYSTEM="1",
        GIT_CONFIG_COUNT=str(len(settings)),
        GIT_AUTHOR_NAME="Test User",
        GIT_AUTHOR_EMAIL="test@example.com",
        GIT_COMMITTER_NAME="Test User",
        GIT_COMMITTER_EMAIL="test@example.com",
    )
    for index, (key, value) in enumerate(settings):
        clean[f"GIT_CONFIG_KEY_{index}"] = key
        clean[f"GIT_CONFIG_VALUE_{index}"] = value
    return clean


class SweepFixtureCase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp_dir.cleanup)
        self.root = Path(self.tmp_dir.name).resolve()
        self.key_path = self.root / "id_ed25519"
        self.allowed_signers = self.root / "allowed_signers"
        # The fixture identity and signing setup live in the environment only: nothing is
        # written to any git config, and the caller's GIT_* (a hook's GIT_DIR) is gone.
        environment = patch.dict(os.environ, fixture_environment(self.root), clear=True)
        environment.start()
        self.addCleanup(environment.stop)

        ssh_keygen = shutil.which("ssh-keygen")
        assert ssh_keygen is not None
        run_command(
            [ssh_keygen, "-t", "ed25519", "-N", "", "-f", str(self.key_path)],
            allowed_executables=(ssh_keygen,),
            capture_output=True,
            text=True,
            timeout_seconds=30.0,
        )
        pub_key = (self.root / "id_ed25519.pub").read_text(encoding="utf-8").strip()
        self.allowed_signers.write_text(f"test@example.com {pub_key}\n", encoding="utf-8")
        self._create_repositories()

    def _create_repositories(self) -> None:
        """Build upstream and fork remotes, a pull request branch and the work clone."""
        self.upstream_bare = self.root / "upstream.git"
        git_run(self.root, "init", "--bare", "--initial-branch=master", str(self.upstream_bare))

        fork_dir = self.root / "testfork"
        fork_dir.mkdir(parents=True)
        self.fork_bare = fork_dir / "vmaf.git"
        git_run(fork_dir, "init", "--bare", "--initial-branch=master", str(self.fork_bare))

        self.setup_clone = self.root / "setup_clone"
        self.setup_clone.mkdir()
        git_run(self.setup_clone, "init", "--initial-branch=master")

        (self.setup_clone / "base.txt").write_text("initial base content\n", encoding="utf-8")
        git_run(self.setup_clone, "add", "base.txt")
        git_run(self.setup_clone, "commit", "-m", "initial commit")
        git_run(self.setup_clone, "push", str(self.upstream_bare), "master:master")
        git_run(self.setup_clone, "push", str(self.fork_bare), "master:master")

        git_run(self.setup_clone, "checkout", "-b", "fix/pr-1")
        (self.setup_clone / "pr_change.txt").write_text("pr contribution\n", encoding="utf-8")
        git_run(self.setup_clone, "add", "pr_change.txt")
        git_run(self.setup_clone, "commit", "-m", "feature: add pr change")
        git_run(self.setup_clone, "push", str(self.fork_bare), "fix/pr-1:fix/pr-1")
        self.pr_old_sha = git_run(self.setup_clone, "rev-parse", "HEAD")

        git_run(self.setup_clone, "checkout", "master")
        (self.setup_clone / "upstream_change.txt").write_text("up advance\n", encoding="utf-8")
        git_run(self.setup_clone, "add", "upstream_change.txt")
        git_run(self.setup_clone, "commit", "-m", "upstream advance")
        git_run(self.setup_clone, "push", str(self.upstream_bare), "master:master")
        self.upstream_master_sha = git_run(self.setup_clone, "rev-parse", "HEAD")

        self.repo = self.root / "work_repo"
        git_run(self.root, "clone", str(self.fork_bare), str(self.repo))
        git_run(self.repo, "remote", "add", "upstream", str(self.upstream_bare))
        git_run(self.repo, "fetch", "upstream", "master")
        git_run(self.repo, "fetch", "origin", "fix/pr-1")

        self.worktree_root = self.root / "worktrees"
        self.worktree_root.mkdir()

        self.prs_file = self.root / "prs.txt"
        self.prs_file.write_text("1 fix/pr-1 testfork\n", encoding="utf-8")
