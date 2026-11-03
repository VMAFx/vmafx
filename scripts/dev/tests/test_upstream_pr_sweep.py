# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regressions for upstream PR sweep: offline git rebase and test harness."""

from __future__ import annotations

import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.run_meson_test import CREDENTIAL_ENV_VARS
from scripts.dev import upstream_pr_sweep as sweep_mod
from scripts.lib.safe_subprocess import TextCommandResult
from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[3]


def setUpModule() -> None:
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
    clean = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
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


class HookEnvironmentIsolationTest(unittest.TestCase):
    """A hook exports GIT_DIR; neither the fixture nor the tool may act on that repository."""

    def test_sentinel_repository_is_untouched_by_fixture_and_tool_git(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch).resolve()
            sentinel = root / "sentinel.git"
            git_bin = shutil.which("git")
            assert git_bin is not None
            clean = fixture_environment(root)
            run_command(
                [git_bin, "init", "--quiet", str(root / "sentinel")],
                allowed_executables=(git_bin,),
                env=clean,
                check=True,
                timeout_seconds=60.0,
            )
            (root / "sentinel" / ".git").rename(sentinel)
            before = (sentinel / "config").read_bytes()
            target = root / "target"
            target.mkdir()
            hook = dict(clean, GIT_DIR=str(sentinel))
            with patch.dict(os.environ, hook, clear=True):
                git_run(target, "init", "--bare", "--quiet", str(target / "bare.git"))
                git_run(target, "init", "--quiet")
                rc, _out, err = sweep_mod.git_cmd(target, ["config", "core.bare", "true"])
                self.assertEqual(rc, 0, err)
            self.assertEqual((sentinel / "config").read_bytes(), before)
            self.assertTrue((target / "bare.git" / "HEAD").is_file())
            self.assertTrue((target / ".git" / "HEAD").is_file())

    def test_caller_config_override_does_not_reach_tool_git(self) -> None:
        # `git -c core.bare=true ...` exports GIT_CONFIG_PARAMETERS to its hooks.
        with tempfile.TemporaryDirectory() as scratch:
            root = Path(scratch).resolve()
            target = root / "target"
            target.mkdir()
            caller = dict(fixture_environment(root), GIT_CONFIG_PARAMETERS="'core.bare'='true'")
            with patch.dict(os.environ, caller, clear=True):
                rc, _out, err = sweep_mod.git_cmd(target, ["init", "--quiet"])
                self.assertEqual(rc, 0, err)
                rc, out, err = sweep_mod.git_cmd(target, ["config", "--get", "core.bare"])
            self.assertEqual(rc, 0, err)
            self.assertEqual(out.strip(), "false")

    def test_git_environment_keeps_settings_and_drops_locations(self) -> None:
        caller = {
            "GIT_DIR": "/x/.git",
            "GIT_WORK_TREE": "/x",
            "GIT_INDEX_FILE": "/x/.git/index",
            "GIT_OBJECT_DIRECTORY": "/x/.git/objects",
            "GIT_COMMON_DIR": "/x/.git",
            "GIT_ALTERNATE_OBJECT_DIRECTORIES": "/y/objects",
            "GIT_CONFIG_PARAMETERS": "'core.bare'='true'",
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "user.name",
            "GIT_CONFIG_VALUE_0": "fixture",
            "GIT_AUTHOR_NAME": "a",
            "GIT_SSH_COMMAND": "ssh",
            "HOME": "/home/x",
        }
        with patch.dict(os.environ, caller, clear=True):
            self.assertEqual(
                sweep_mod.git_environment(),
                {
                    "GIT_CONFIG_COUNT": "1",
                    "GIT_CONFIG_KEY_0": "user.name",
                    "GIT_CONFIG_VALUE_0": "fixture",
                    "GIT_AUTHOR_NAME": "a",
                    "GIT_SSH_COMMAND": "ssh",
                    "HOME": "/home/x",
                },
            )


class UpstreamPrSweepTest(unittest.TestCase):
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

    def test_positive_dry_run_then_apply_and_push(self) -> None:
        parser = sweep_mod.build_parser()
        args_dry = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
            ]
        )
        rows_dry = sweep_mod.sweep(args_dry)
        self.assertEqual(len(rows_dry), 1)
        self.assertEqual(rows_dry[0].status, "PLANNED")
        self.assertEqual(rows_dry[0].old_sha[:9], self.pr_old_sha[:9])
        self.assertEqual(rows_dry[0].new_sha, "-")

        fork_head = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
        self.assertEqual(fork_head, self.pr_old_sha)

        args_apply = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
                "--apply",
                "--push",
            ]
        )
        rows_apply = sweep_mod.sweep(args_apply)
        self.assertEqual(len(rows_apply), 1)
        self.assertEqual(rows_apply[0].status, "OK")
        self.assertEqual(rows_apply[0].unsigned, 0)
        self.assertEqual(rows_apply[0].detail, "pushed")

        fork_new_sha = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
        self.assertNotEqual(fork_new_sha, self.pr_old_sha)
        self.assertEqual(fork_new_sha[:9], rows_apply[0].new_sha[:9])

        parent = git_run(self.fork_bare, "rev-parse", "fix/pr-1^")
        self.assertEqual(parent, self.upstream_master_sha)

    def test_negative_head_owner_not_ours(self) -> None:
        bad_prs = self.root / "bad_prs.txt"
        bad_prs.write_text("1 fix/pr-1 otherowner\n", encoding="utf-8")
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(bad_prs),
                "--apply",
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "REFUSED")
        self.assertIn("not ours: head_owner 'otherowner' != 'testfork'", rows[0].detail)
        fork_head = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
        self.assertEqual(fork_head, self.pr_old_sha)

    def test_negative_origin_url_not_fork(self) -> None:
        wrong_prs = self.root / "wrong_fork_prs.txt"
        wrong_prs.write_text("1 fix/pr-1 wrongowner\n", encoding="utf-8")
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "wrongowner/vmaf",
                "--prs",
                str(wrong_prs),
                "--apply",
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "REFUSED")
        self.assertIn("origin remote URL", rows[0].detail)
        fork_head = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
        self.assertEqual(fork_head, self.pr_old_sha)

    def test_negative_dirty_worktree(self) -> None:
        wt = self.worktree_root / "netflix-vmaf-contributions-1"
        git_run(self.repo, "worktree", "add", "--detach", str(wt), self.pr_old_sha)
        (wt / "dirty_file.txt").write_text("untracked dirty content\n", encoding="utf-8")

        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
                "--apply",
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "REFUSED")
        self.assertIn("dirty", rows[0].detail)
        fork_head = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
        self.assertEqual(fork_head, self.pr_old_sha)

    def test_negative_conflicting_pr(self) -> None:
        conflict_branch = "fix/conflict-pr"
        git_run(self.setup_clone, "checkout", "-b", conflict_branch, self.pr_old_sha)
        (self.setup_clone / "base.txt").write_text("pr conflicting change\n", encoding="utf-8")
        git_run(self.setup_clone, "add", "base.txt")
        git_run(self.setup_clone, "commit", "-m", "pr conflicting change")
        git_run(
            self.setup_clone, "push", str(self.fork_bare), f"{conflict_branch}:{conflict_branch}"
        )
        conflict_old_sha = git_run(self.setup_clone, "rev-parse", "HEAD")

        git_run(self.setup_clone, "checkout", "master")
        (self.setup_clone / "base.txt").write_text(
            "upstream conflicting change\n", encoding="utf-8"
        )
        git_run(self.setup_clone, "add", "base.txt")
        git_run(self.setup_clone, "commit", "-m", "upstream conflicting change")
        git_run(self.setup_clone, "push", str(self.upstream_bare), "master:master")

        git_run(self.repo, "fetch", "upstream", "master")
        git_run(self.repo, "fetch", "origin", conflict_branch)

        conflict_prs = self.root / "conflict_prs.txt"
        conflict_prs.write_text(f"2 {conflict_branch} testfork\n", encoding="utf-8")

        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(conflict_prs),
                "--apply",
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "CONFLICT")
        self.assertIn("files=base.txt", rows[0].detail)

        fork_head = git_run(self.fork_bare, "rev-parse", conflict_branch)
        self.assertEqual(fork_head, conflict_old_sha)

        wt = self.worktree_root / "netflix-vmaf-contributions-2"
        self.assertTrue(wt.exists())
        self.assertEqual(git_run(wt, "status", "--porcelain"), "")
        self.assertEqual(git_run(wt, "rev-parse", "HEAD"), conflict_old_sha)

    def test_negative_rebase_failure_without_conflict(self) -> None:
        status, detail = sweep_mod.run_rebase(self.repo, "0" * 40)
        self.assertEqual(status, "REBASE-FAIL")
        self.assertNotIn("files=", detail)
        self.assertNotEqual(detail, "")
        self.assertEqual(git_run(self.repo, "status", "--porcelain"), "")

    def test_negative_local_only_branch_refused(self) -> None:
        git_run(self.repo, "branch", "fix/local-only", "HEAD")
        local_prs = self.root / "local_prs.txt"
        local_prs.write_text("3 fix/local-only testfork\n", encoding="utf-8")
        args = sweep_mod.build_parser().parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(local_prs),
                "--apply",
                "--push",
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual([row.status for row in rows], ["REFUSED"])
        self.assertIn("cannot resolve origin/fix/local-only", rows[0].detail)
        rc, _out, _err = sweep_mod.git_cmd(
            self.fork_bare, ["rev-parse", "--verify", "fix/local-only"]
        )
        self.assertNotEqual(rc, 0)

    def test_boundary_dry_run_leaves_refs_and_worktrees_unchanged(self) -> None:
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "PLANNED")

        wt = self.worktree_root / "netflix-vmaf-contributions-1"
        self.assertFalse(wt.exists())
        fork_head = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
        self.assertEqual(fork_head, self.pr_old_sha)

    def test_boundary_empty_pr_list(self) -> None:
        empty_prs = self.root / "empty_prs.txt"
        empty_prs.write_text("", encoding="utf-8")
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(empty_prs),
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(rows, [])

        buffer = io.StringIO()
        with redirect_stdout(buffer):
            exit_code = sweep_mod.main(
                [
                    "--repo",
                    str(self.repo),
                    "--worktree-root",
                    str(self.worktree_root),
                    "--fork",
                    "testfork/vmaf",
                    "--prs",
                    str(empty_prs),
                ]
            )
        self.assertEqual(exit_code, 0)
        self.assertEqual(buffer.getvalue(), "")

    def test_boundary_stale_lease_push_refused(self) -> None:
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
                "--apply",
                "--push",
            ]
        )
        real_rebase = sweep_mod.run_rebase
        remote_racing_sha = ""

        def rebase_with_concurrent_remote_push(wt: Path, base_sha: str) -> tuple[str | None, str]:
            nonlocal remote_racing_sha
            res = real_rebase(wt, base_sha)
            git_run(self.setup_clone, "checkout", "fix/pr-1")
            (self.setup_clone / "stale_advance.txt").write_text("racing push\n", encoding="utf-8")
            git_run(self.setup_clone, "add", "stale_advance.txt")
            git_run(self.setup_clone, "commit", "-m", "racing commit on remote")
            git_run(self.setup_clone, "push", str(self.fork_bare), "fix/pr-1:fix/pr-1")
            remote_racing_sha = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
            return res

        with patch.object(sweep_mod, "run_rebase", side_effect=rebase_with_concurrent_remote_push):
            rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "REFUSED")
        self.assertIn("push failed", rows[0].detail)

        fork_head = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
        self.assertEqual(fork_head, remote_racing_sha)

    @unittest.skipUnless(
        shutil.which("meson") and shutil.which("ninja"),
        "meson and ninja are needed to build the dummy project",
    )
    def test_build_and_test_clean(self) -> None:
        wt = self.worktree_root / "build-test-wt"
        libvmaf = wt / "libvmaf"
        libvmaf.mkdir(parents=True)
        (libvmaf / "meson.build").write_text(
            "project('dummy', 'c')\ntest('dummy', find_program('true'))\n"
        )
        (libvmaf / "meson_options.txt").write_text(
            "option('enable_cuda', type: 'boolean', value: false)\n"
            "option('enable_nvcc', type: 'boolean', value: false)\n"
        )
        status, detail = sweep_mod.build_and_test(wt, 99, "fix/pr-99", "fix/cuda-", 2)
        self.assertEqual(status, "OK")
        self.assertEqual(detail, "build=ok test=ok")
        self.assertTrue((self.worktree_root / "99-setup.log").exists())
        self.assertTrue((self.worktree_root / "99-build.log").exists())
        self.assertTrue((self.worktree_root / "99-test.log").exists())

    def test_discover_prs_mocked(self) -> None:
        sample_json = (
            '[{"number": 42, "headRefName": "fix/answer", '
            '"headRepositoryOwner": {"login": "testowner"}, '
            '"headRepository": {"name": "vmaf"}}]'
        )
        with (
            patch.object(shutil, "which", return_value="/usr/bin/gh"),
            patch.object(sweep_mod, "run_command") as mock_run,
        ):
            mock_run.return_value = TextCommandResult(
                argv=("gh",), returncode=0, stdout=sample_json, stderr=""
            )
            targets = sweep_mod.discover_prs(self.repo)
            self.assertEqual(len(targets), 1)
            self.assertEqual(targets[0].number, 42)
            self.assertEqual(targets[0].branch, "fix/answer")
            self.assertEqual(targets[0].head_owner, "testowner")

    def test_negative_unsigned_commit_rejected(self) -> None:
        empty_signers = self.root / "empty_signers"
        empty_signers.write_text("", encoding="utf-8")
        index = next(
            key[len("GIT_CONFIG_KEY_") :]
            for key, value in os.environ.items()
            if key.startswith("GIT_CONFIG_KEY_") and value == "gpg.ssh.allowedSignersFile"
        )
        os.environ[f"GIT_CONFIG_VALUE_{index}"] = str(empty_signers)

        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
                "--apply",
                "--push",
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "UNSIGNED")
        self.assertGreater(rows[0].unsigned, 0)
        self.assertIn("unsigned commits", rows[0].detail)

        fork_head = git_run(self.fork_bare, "rev-parse", "fix/pr-1")
        self.assertEqual(fork_head, self.pr_old_sha)

    def test_push_without_apply_refusal(self) -> None:
        buffer = io.StringIO()
        err_buffer = io.StringIO()
        with redirect_stdout(buffer), redirect_stderr(err_buffer):
            exit_code = sweep_mod.main(
                [
                    "--repo",
                    str(self.repo),
                    "--worktree-root",
                    str(self.worktree_root),
                    "--fork",
                    "testfork/vmaf",
                    "--prs",
                    str(self.prs_file),
                    "--push",
                ]
            )
        self.assertEqual(exit_code, 1)

    def test_T00_missing_new_sha_returns_structured_failure(self) -> None:
        target = sweep_mod.PRTarget(number=1, branch="fix/pr-1", head_owner="testfork")
        for missing_val in (None, ""):
            with patch.object(sweep_mod, "_execute_rebase", return_value=(None, missing_val)):
                row = sweep_mod.process_pr(
                    target,
                    repo=self.repo,
                    worktree_root=self.worktree_root,
                    fork="testfork/vmaf",
                    base_sha=self.upstream_master_sha,
                    apply=True,
                    build=False,
                    push=False,
                    cuda_prefix="fix/cuda-",
                    jobs=2,
                )
            self.assertEqual(row.status, "REBASE-FAIL")
            self.assertIn("missing rebased HEAD SHA", row.detail)

    def test_T01_missing_worktree_or_old_sha_returns_refused_row(self) -> None:
        target = sweep_mod.PRTarget(number=1, branch="fix/pr-1", head_owner="testfork")
        for bad_tuple in ((None, None, None), (None, self.worktree_root / "wt", "")):
            with patch.object(sweep_mod, "_validate_pr_preconditions", return_value=bad_tuple):
                row = sweep_mod.process_pr(
                    target,
                    repo=self.repo,
                    worktree_root=self.worktree_root,
                    fork="testfork/vmaf",
                    base_sha=self.upstream_master_sha,
                    apply=True,
                    build=False,
                    push=False,
                    cuda_prefix="fix/cuda-",
                    jobs=2,
                )
            self.assertEqual(row.status, "REFUSED")
            self.assertIn("missing worktree or pre-rebase SHA", row.detail)

    def test_T02_origin_url_canonical_owner_repo_parsing(self) -> None:
        valid_urls = (
            "git@github.com:TestFork/vmaf.git",
            "ssh://git@github.com/testfork/vmaf.git",
            "https://github.com/TestFork/VMAF",
            "https://github.com/testfork/vmaf.git",
        )
        for url in valid_urls:
            git_run(self.repo, "remote", "set-url", "origin", url)
            ok, detail = sweep_mod.check_origin_url(self.repo, "testfork/vmaf")
            self.assertTrue(ok, f"expected {url} to match: {detail}")

        invalid_urls = (
            "https://github.com/testfork/vmaf-extra.git",
            "https://github.com/evil/testfork/vmaf.git",
            "git@github.com:testfork/vmaf-other.git",
            "https://github.com/other/repo.git?fork=testfork/vmaf",
        )
        for url in invalid_urls:
            git_run(self.repo, "remote", "set-url", "origin", url)
            ok, detail = sweep_mod.check_origin_url(self.repo, "testfork/vmaf")
            self.assertFalse(ok, f"expected {url} to be rejected: {detail}")

    def test_T03_discover_prs_uses_configured_upstream_repo(self) -> None:
        sample_json = (
            '[{"number": 7, "headRefName": "fix/pr-1", '
            '"headRepositoryOwner": {"login": "testfork"}, '
            '"headRepository": {"name": "vmaf"}}]'
        )
        with (
            patch.object(shutil, "which", return_value="/usr/bin/gh"),
            patch.object(sweep_mod, "run_command") as mock_run,
        ):
            mock_run.return_value = TextCommandResult(
                argv=("gh",), returncode=0, stdout=sample_json, stderr=""
            )
            targets = sweep_mod.discover_prs(self.repo, upstream_repo="CustomOrg/custom-vmaf")
            self.assertEqual(len(targets), 1)
            called_argv = list(mock_run.call_args.args[0])
            repo_flag_idx = called_argv.index("-R")
            self.assertEqual(called_argv[repo_flag_idx + 1], "CustomOrg/custom-vmaf")

    def test_T04_foreign_author_commit_refused_unless_allowed(self) -> None:
        foreign_branch = "fix/foreign-pr"
        git_run(self.setup_clone, "checkout", "-b", foreign_branch, self.pr_old_sha)
        (self.setup_clone / "foreign.txt").write_text("untrusted code\n", encoding="utf-8")
        git_run(self.setup_clone, "add", "foreign.txt")
        foreign_env = dict(
            os.environ,
            GIT_AUTHOR_EMAIL="contributor@untrusted.example",
            GIT_COMMITTER_EMAIL="contributor@untrusted.example",
        )
        with patch.dict(os.environ, foreign_env, clear=True):
            git_run(self.setup_clone, "commit", "--no-gpg-sign", "-m", "foreign commit")
        git_run(self.setup_clone, "push", str(self.fork_bare), f"{foreign_branch}:{foreign_branch}")
        git_run(self.repo, "fetch", "origin", foreign_branch)

        foreign_prs = self.root / "foreign_prs.txt"
        foreign_prs.write_text(f"4 {foreign_branch} testfork\n", encoding="utf-8")
        parser = sweep_mod.build_parser()
        args_refuse = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(foreign_prs),
                "--apply",
            ]
        )
        rows = sweep_mod.sweep(args_refuse)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "REFUSED")
        self.assertIn("foreign commit identity", rows[0].detail)

        args_allow = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(foreign_prs),
                "--apply",
                "--allow-foreign-authors",
            ]
        )
        rows_allowed = sweep_mod.sweep(args_allow)
        self.assertEqual(len(rows_allowed), 1)
        self.assertEqual(rows_allowed[0].status, "OK")

    def test_T05_meson_and_ninja_strip_caller_credentials(self) -> None:
        vmaf_dir = self.root / "dummy_vmaf"
        vmaf_dir.mkdir()
        log_path = self.root / "dummy.log"
        secret_env = {
            "PATH": os.environ.get("PATH", "/usr/bin"),
            "HOME": str(self.root),
            "GH_TOKEN": "ghp_secret_token",
            "GITHUB_TOKEN": "ghs_secret_token",
            "AWS_SECRET_ACCESS_KEY": "aws_super_secret",
            "MY_CUSTOM_SECRET": "custom_secret",
            "SSH_AUTH_SOCK": str(self.root / "ssh-agent.sock"),
            "GIT_ASKPASS": "/bin/echo",
        }
        with (
            patch.dict(os.environ, secret_env, clear=True),
            patch.object(shutil, "which", return_value="/usr/bin/true"),
            patch.object(sweep_mod, "run_command") as mock_run,
        ):
            mock_run.return_value = TextCommandResult(
                argv=("true",), returncode=0, stdout="", stderr=""
            )
            ok_setup, _ = sweep_mod._run_meson_setup(vmaf_dir, log_path, False)
            ok_ninja, _ = sweep_mod._run_ninja(vmaf_dir, log_path, 2)
            self.assertTrue(ok_setup)
            self.assertTrue(ok_ninja)
            self.assertEqual(mock_run.call_count, 2)
            for call in mock_run.call_args_list:
                passed_env = call.kwargs["env"]
                for forbidden in (
                    "GH_TOKEN",
                    "GITHUB_TOKEN",
                    "AWS_SECRET_ACCESS_KEY",
                    "MY_CUSTOM_SECRET",
                    "SSH_AUTH_SOCK",
                    "GIT_ASKPASS",
                ):
                    self.assertNotIn(forbidden, passed_env)
                self.assertIn("PATH", passed_env)

    def test_T06_worktree_with_wrong_origin_or_substring_repo_refused(self) -> None:
        wt = self.worktree_root / "netflix-vmaf-contributions-1"
        git_run(self.repo, "worktree", "add", "--detach", str(wt), self.pr_old_sha)
        git_run(self.repo, "remote", "set-url", "origin", "https://github.com/testfork/vmaf-2.git")
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "REFUSED")
        self.assertIn("does not name fork", rows[0].detail)

    def test_T07_divergent_origin_pushurl_refused(self) -> None:
        git_run(
            self.repo,
            "remote",
            "set-url",
            "--push",
            "origin",
            "https://github.com/otherowner/otherrepo.git",
        )
        ok, detail = sweep_mod.check_origin_url(self.repo, "testfork/vmaf")
        self.assertFalse(ok)
        self.assertIn("does not name fork", detail)

        push_ok, push_detail = sweep_mod.push_branch(
            self.repo, "fix/pr-1", self.pr_old_sha, fork="testfork/vmaf"
        )
        self.assertFalse(push_ok)
        self.assertIn("does not name fork", push_detail)

    def test_T08_rejected_origin_url_redacts_credentials(self) -> None:
        remote_with_userinfo = (
            "https://user:supersecret_token_123@github.com/other/repo.git?token=leak456"
        )
        git_run(self.repo, "remote", "set-url", "origin", remote_with_userinfo)
        ok, detail = sweep_mod.check_origin_url(self.repo, "testfork/vmaf")
        self.assertFalse(ok)
        self.assertNotIn("supersecret_token_123", detail)
        self.assertNotIn("leak456", detail)
        self.assertIn("<redacted>@github.com/other/repo.git", detail)

    def test_T09_unregistered_worktree_directory_refused(self) -> None:
        rogue_wt = self.worktree_root / "netflix-vmaf-contributions-1"
        rogue_wt.mkdir(parents=True)
        git_run(rogue_wt, "init", "--initial-branch=master")
        (rogue_wt / "file.txt").write_text("unrelated repo\n", encoding="utf-8")
        git_run(rogue_wt, "add", "file.txt")
        git_run(rogue_wt, "commit", "-m", "unrelated initial")
        rogue_head_before = git_run(rogue_wt, "rev-parse", "HEAD")

        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
                "--apply",
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "REFUSED")
        self.assertIn("not a registered worktree", rows[0].detail)
        self.assertEqual(git_run(rogue_wt, "rev-parse", "HEAD"), rogue_head_before)

    def test_T10_branch_checked_out_in_main_repo_still_rebases_in_detached_worktree(self) -> None:
        git_run(self.repo, "checkout", "-B", "fix/pr-1", self.pr_old_sha)
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
                "--apply",
                "--push",
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "OK")
        self.assertEqual(rows[0].detail, "pushed")

    def test_T11_apply_fetches_latest_upstream_and_origin_tips(self) -> None:
        git_run(self.setup_clone, "checkout", "master")
        (self.setup_clone / "upstream_v2.txt").write_text("upstream v2\n", encoding="utf-8")
        git_run(self.setup_clone, "add", "upstream_v2.txt")
        git_run(self.setup_clone, "commit", "-m", "upstream v2")
        git_run(self.setup_clone, "push", str(self.upstream_bare), "master:master")
        new_upstream_sha = git_run(self.setup_clone, "rev-parse", "HEAD")

        git_run(self.setup_clone, "checkout", "fix/pr-1")
        (self.setup_clone / "pr_v2.txt").write_text("pr v2\n", encoding="utf-8")
        git_run(self.setup_clone, "add", "pr_v2.txt")
        git_run(self.setup_clone, "commit", "-m", "pr v2")
        git_run(self.setup_clone, "push", str(self.fork_bare), "fix/pr-1:fix/pr-1")
        new_pr_sha = git_run(self.setup_clone, "rev-parse", "HEAD")

        parser = sweep_mod.build_parser()
        args_dry = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
            ]
        )
        rows_dry = sweep_mod.sweep(args_dry)
        self.assertEqual(rows_dry[0].detail, "dry-run (local refs)")
        self.assertEqual(rows_dry[0].old_sha[:9], self.pr_old_sha[:9])

        args_apply = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
                "--apply",
                "--push",
            ]
        )
        rows_apply = sweep_mod.sweep(args_apply)
        self.assertEqual(rows_apply[0].status, "OK")
        self.assertEqual(rows_apply[0].old_sha[:9], new_pr_sha[:9])
        self.assertEqual(git_run(self.fork_bare, "rev-parse", "fix/pr-1~2"), new_upstream_sha)

    def test_T12_concurrent_worktree_lock_contention_refused(self) -> None:
        wt = sweep_mod.pr_worktree_path(self.worktree_root, 1)
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(self.prs_file),
            ]
        )
        with sweep_mod.worktree_lock(wt) as (acquired, _):
            self.assertTrue(acquired)
            rows_busy = sweep_mod.sweep(args)
            self.assertEqual(len(rows_busy), 1)
            self.assertEqual(rows_busy[0].status, "REFUSED")
            self.assertIn("worktree lock busy", rows_busy[0].detail)

        rows_free = sweep_mod.sweep(args)
        self.assertEqual(rows_free[0].status, "PLANNED")

    def test_T13_missing_or_malformed_pr_file_raises_and_exits_nonzero(self) -> None:
        with self.assertRaises(RuntimeError):
            sweep_mod.parse_pr_file(self.root / "missing.txt")

        malformed = self.root / "malformed.txt"
        malformed.write_text("# comment\n42\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, r"malformed\.txt:2:"):
            sweep_mod.parse_pr_file(malformed)

        bad_num = self.root / "bad_num.txt"
        bad_num.write_text("notanint fix/pr-1 testfork\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, r"bad_num\.txt:1:"):
            sweep_mod.parse_pr_file(bad_num)

        err_buf = io.StringIO()
        with redirect_stderr(err_buf):
            rc = sweep_mod.main(
                [
                    "--repo",
                    str(self.repo),
                    "--worktree-root",
                    str(self.worktree_root),
                    "--fork",
                    "testfork/vmaf",
                    "--prs",
                    str(malformed),
                ]
            )
        self.assertEqual(rc, 1)

    def test_T14_optional_owner_column_defaults_to_fork_owner(self) -> None:
        two_col = self.root / "two_col.txt"
        two_col.write_text("1 fix/pr-1\n", encoding="utf-8")
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--prs",
                str(two_col),
            ]
        )
        rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "PLANNED")

    def test_T15_discover_prs_passes_limit_and_refuses_truncation(self) -> None:
        two_items = (
            '[{"number": 1, "headRefName": "b1", "headRepositoryOwner": {"login": "o"}, '
            '"headRepository": {"name": "r"}}, '
            '{"number": 2, "headRefName": "b2", "headRepositoryOwner": {"login": "o"}, '
            '"headRepository": {"name": "r"}}]'
        )
        with (
            patch.object(shutil, "which", return_value="/usr/bin/gh"),
            patch.object(sweep_mod, "run_command") as mock_run,
        ):
            mock_run.return_value = TextCommandResult(
                argv=("gh",), returncode=0, stdout=two_items, stderr=""
            )
            with self.assertRaisesRegex(RuntimeError, "truncated discovery"):
                sweep_mod.discover_prs(self.repo, limit=2)
            called_argv = list(mock_run.call_args.args[0])
            limit_idx = called_argv.index("--limit")
            self.assertEqual(called_argv[limit_idx + 1], "2")

    def test_T16_same_owner_different_head_repository_refused(self) -> None:
        sample_json = (
            '[{"number": 1, "headRefName": "fix/pr-1", '
            '"headRepositoryOwner": {"login": "testfork"}, '
            '"headRepository": {"name": "unrelated-repo"}}]'
        )
        parser = sweep_mod.build_parser()
        args = parser.parse_args(
            [
                "--repo",
                str(self.repo),
                "--worktree-root",
                str(self.worktree_root),
                "--fork",
                "testfork/vmaf",
                "--discover",
            ]
        )
        with (
            patch.object(shutil, "which", return_value="/usr/bin/gh"),
            patch.object(sweep_mod, "run_command") as mock_run,
        ):
            mock_run.return_value = TextCommandResult(
                argv=("gh",), returncode=0, stdout=sample_json, stderr=""
            )
            rows = sweep_mod.sweep(args)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].status, "REFUSED")
        self.assertIn("head_repo 'testfork/unrelated-repo' != 'testfork/vmaf'", rows[0].detail)

    def test_T17_local_ref_named_origin_branch_cannot_impersonate_remote_tip(self) -> None:
        git_run(self.repo, "branch", "origin/fix/shadowed", "HEAD")
        self.assertIsNone(sweep_mod.resolve_old_sha(self.repo, "fix/shadowed"))

    def test_T18_tsv_escapes_tabs_newlines_carriage_returns_and_backslashes(self) -> None:
        row = sweep_mod.SweepRow(
            1,
            "fix/pr-1",
            "REFUSED",
            "1234567890abcdef",
            "-",
            0,
            "err line 1\nerr line 2\twith\\slash\rand_cr",
        )
        tsv = row.to_tsv()
        self.assertNotIn("\n", tsv)
        self.assertNotIn("\r", tsv)
        fields = tsv.split("\t")
        self.assertEqual(len(fields), 7)
        self.assertEqual(fields[6], "err line 1\\nerr line 2\\twith\\\\slash\\rand_cr")

    def test_T19_git_command_failures_in_rebase_and_count_unsigned_reported_distinctly(
        self,
    ) -> None:
        def fake_git_abort_fail(
            _cwd: Path, args: list[str], **_kwargs: object
        ) -> tuple[int, str, str]:
            if "rebase" in args and "--gpg-sign" in args:
                return 1, "", "rebase failed"
            if args[:1] == ["diff"]:
                return 0, "base.txt\n", ""
            if args == ["rebase", "--abort"]:
                return 1, "", "abort failed"
            return 0, "", ""

        with patch.object(sweep_mod, "git_cmd", side_effect=fake_git_abort_fail):
            status, detail = sweep_mod.run_rebase(self.repo, self.upstream_master_sha)
        self.assertEqual(status, "REBASE-ABORT-FAIL")
        self.assertIn("abort failed", detail)

        with patch.object(sweep_mod, "git_cmd", return_value=(128, "", "fatal: bad revision")):
            self.assertEqual(sweep_mod.count_unsigned(self.repo, self.upstream_master_sha), -1)

        target = sweep_mod.PRTarget(number=1, branch="fix/pr-1", head_owner="testfork")
        with (
            patch.object(sweep_mod, "check_commit_authors", return_value=(True, "")),
            patch.object(sweep_mod, "prepare_worktree", return_value=(True, "")),
            patch.object(sweep_mod, "run_rebase", return_value=(None, "")),
            patch.object(sweep_mod, "git_cmd", return_value=(128, "", "rev-parse failed")),
        ):
            fail_row, new_sha = sweep_mod._execute_rebase(
                self.repo,
                self.worktree_root / "wt",
                target,
                self.pr_old_sha,
                self.upstream_master_sha,
            )
        self.assertIsNone(new_sha)
        assert fail_row is not None
        self.assertEqual(fail_row.status, "REBASE-FAIL")
        self.assertIn("rev-parse HEAD failed", fail_row.detail)

    def test_T20_research_digest_exists_and_has_required_sections(self) -> None:
        digest = REPO_ROOT / "docs" / "research" / "3266-upstream-pr-sweep-tool.md"
        self.assertTrue(digest.is_file(), f"missing {digest}")
        text = digest.read_text(encoding="utf-8")
        for heading in (
            "# Research-3266:",
            "## Question",
            "## Sources",
            "## Findings",
            "## Alternatives explored",
            "## Open questions",
            "## Related",
        ):
            self.assertIn(heading, text)

    def test_T21_rebase_notes_fragment_exists_and_passes_lint_contract(self) -> None:
        fragment = REPO_ROOT / "docs" / "rebase-notes.d" / "upstream-pr-sweep-tool.md"
        self.assertTrue(fragment.is_file(), f"missing {fragment}")
        lines = fragment.read_text(encoding="utf-8").splitlines()
        self.assertGreaterEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith("## "), f"bad first line: {lines[0]!r}")
        self.assertNotEqual(lines[-1].strip(), "", "fragment must not end with a blank line")

    def test_T22_docs_pr_list_format_includes_head_owner_example(self) -> None:
        doc_path = REPO_ROOT / "docs" / "development" / "upstream-pr-sweep.md"
        doc_text = doc_path.read_text(encoding="utf-8")
        self.assertIn("<number> <branch> <head_owner>", doc_text)
        self.assertIn("1707 fix/cambi-heatmaps VMAFx", doc_text)

    def test_T23_docs_test_link_uses_github_repository_url(self) -> None:
        doc_path = REPO_ROOT / "docs" / "development" / "upstream-pr-sweep.md"
        doc_text = doc_path.read_text(encoding="utf-8")
        self.assertNotIn("../../scripts/dev/tests/test_upstream_pr_sweep.py", doc_text)
        self.assertIn(
            "https://github.com/VMAFx/vmafx/blob/master/scripts/dev/tests/test_upstream_pr_sweep.py",
            doc_text,
        )

    def test_T24_missing_prs_file_fails_cli_with_exit_code_1(self) -> None:
        err_buf = io.StringIO()
        with redirect_stderr(err_buf):
            rc = sweep_mod.main(
                [
                    "--repo",
                    str(self.repo),
                    "--worktree-root",
                    str(self.worktree_root),
                    "--fork",
                    "testfork/vmaf",
                    "--prs",
                    str(self.root / "nonexistent-prs.txt"),
                ]
            )
        self.assertEqual(rc, 1)
        self.assertIn("PR file not found", err_buf.getvalue())

    def test_T25_build_environment_uses_sanitize_process_environment(self) -> None:
        cred_env = {name: f"secret_{name}" for name in CREDENTIAL_ENV_VARS}
        cred_env["PATH"] = "/usr/bin"
        with patch.dict(os.environ, cred_env, clear=True):
            sanitized = sweep_mod.build_environment()
        for name in CREDENTIAL_ENV_VARS:
            self.assertNotIn(name, sanitized)
        self.assertEqual(sanitized.get("PATH"), "/usr/bin")

    def test_T26_per_pr_exception_recorded_as_error_row_preserving_other_prs(self) -> None:
        two_prs = self.root / "two_prs.txt"
        two_prs.write_text("1 fix/pr-1 testfork\n2 fix/pr-1 testfork\n", encoding="utf-8")
        real_process = sweep_mod.process_pr

        def flaky_process(target: sweep_mod.PRTarget, **kwargs: object) -> sweep_mod.SweepRow:
            if target.number == 1:
                raise OSError("simulated disk error")
            return real_process(target, **kwargs)  # type: ignore[arg-type]

        out_buf = io.StringIO()
        with (
            patch.object(sweep_mod, "process_pr", side_effect=flaky_process),
            redirect_stdout(out_buf),
        ):
            rc = sweep_mod.main(
                [
                    "--repo",
                    str(self.repo),
                    "--worktree-root",
                    str(self.worktree_root),
                    "--fork",
                    "testfork/vmaf",
                    "--prs",
                    str(two_prs),
                ]
            )
        self.assertEqual(rc, 1)
        out_lines = out_buf.getvalue().strip().splitlines()
        self.assertEqual(len(out_lines), 2)
        self.assertIn("1\tfix/pr-1\tERROR\t-\t-\t0\tOSError: simulated disk error", out_lines[0])
        self.assertIn("2\tfix/pr-1\tPLANNED\t", out_lines[1])

    def test_T27_framework_hooks_preserves_four_var_env_unset_on_install_hooks(self) -> None:
        hook_script = (REPO_ROOT / "scripts" / "git-hooks" / "framework-hooks.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            'env -u GIT_INDEX_FILE -u GIT_DIR -u GIT_WORK_TREE -u GIT_OBJECT_DIRECTORY "$fw" install-hooks || exit 1',
            hook_script,
        )


if __name__ == "__main__":
    unittest.main()
