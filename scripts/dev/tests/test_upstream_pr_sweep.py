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

from scripts.dev import upstream_pr_sweep as sweep_mod
from scripts.lib.safe_subprocess import TextCommandResult
from scripts.lib.safe_subprocess import run as run_command


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

        git_run(self.setup_clone, "checkout", "fix/pr-1")
        (self.setup_clone / "stale_advance.txt").write_text("racing push\n", encoding="utf-8")
        git_run(self.setup_clone, "add", "stale_advance.txt")
        git_run(self.setup_clone, "commit", "-m", "racing commit on remote")
        git_run(self.setup_clone, "push", str(self.fork_bare), "fix/pr-1:fix/pr-1")
        remote_racing_sha = git_run(self.fork_bare, "rev-parse", "fix/pr-1")

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
        sample_json = '[{"number": 42, "headRefName": "fix/answer", "headRepositoryOwner": {"login": "testowner"}}]'
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


if __name__ == "__main__":
    unittest.main()
