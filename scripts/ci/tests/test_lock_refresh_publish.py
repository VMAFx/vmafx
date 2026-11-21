#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Cases for scripts/ci/lock_refresh_publish.py (ADR-3555, ADR-3560).

Each case runs the script against a stub work repository and a local bare repository that
stands in for GitHub: no network, no real push, no ``gh``.

Positive: a changed lock is committed with the release-bot identity and sign-off and pushed;
only the changed paths are in the commit; the commit message equals the one the Intel NEO
workflow wrote before the script existed.
Negative: a non-Renovate ref, a stray path outside the allowed list, an app slug that is not
the release-bot, and a remote that moved meanwhile each refuse, leave the remote untouched
and (but for the moved remote) leave no commit behind.
Boundary: a path next to an allowed path (``.bak``, another directory), a directory, a
pattern and a path with pathspec magic never widen the allowed set; an unchanged lock is rc 0
with no commit; a missing environment variable is rc 2.
"""

from __future__ import annotations

import secrets
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci.tests import check_dco_fixture as fx  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult, run  # noqa: E402

SCRIPT = ROOT / "scripts" / "ci" / "lock_refresh_publish.py"
BOT = "vmafx-release-bot[bot]"
BOT_EMAIL = "340976456+vmafx-release-bot[bot]@users.noreply.github.com"
BRANCH = "renovate/lockfiles"
REF = f"refs/heads/{BRANCH}"
LOCK_A = "requirements/locks/a.txt"
LOCK_B = "docs/requirements-lock.txt"
OTHER = "requirements/locks/a.in"
SUBJECT = "chore(deps): refresh Python dependency locks"
BODY = ("Regenerate the manifest lock outputs.", "(ADR-3560).")


class PublishTestCase(unittest.TestCase):
    """A work repository on a renovate branch with a bare remote that holds the same tip."""

    def setUp(self) -> None:
        """Create the bare remote, the work repository and the first pushed commit."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.remote = self.base / "remote.git"
        self.runner_temp = self.base / "runner-temp"
        self.runner_temp.mkdir()
        fx.init_bare(self.remote)
        (self.base / "work").mkdir()
        self.repo = fx.Repo(self.base / "work")
        for rel in (LOCK_A, LOCK_B, OTHER):
            target = self.repo.path / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("first\n", encoding="utf-8")
        self.repo.git(["add", "--", LOCK_A, LOCK_B, OTHER])
        self.repo.git(["commit", "-q", "-m", "locks", "-s"])
        self.repo.git(["checkout", "-q", "-b", BRANCH])
        self.repo.git(["push", "-q", str(self.remote), f"HEAD:{REF}"])
        self.tip = self.remote_tip()

    @staticmethod
    def run_git_at(cwd: Path, args: list[str]) -> str:
        """Run isolated git in ``cwd``."""
        done = run(
            [fx.GIT, *args],
            allowed_executables=(fx.GIT,),
            cwd=cwd,
            env=fx.clean_env(),
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=fx.GIT_TIMEOUT_S,
        )
        return done.stdout

    def remote_tip(self) -> str:
        """The commit the bare remote holds for the Renovate branch."""
        return self.run_git_at(self.remote, ["rev-parse", REF]).strip()

    def work_head(self) -> str:
        """The work repository HEAD commit."""
        return self.repo.git(["rev-parse", "HEAD"]).strip()

    def edit(self, rel: str, text: str = "second\n") -> None:
        """Write a file in the work repository, creating directories."""
        target = self.repo.path / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def env(self, **override: str | None) -> dict[str, str]:
        """The isolated environment of one workflow step; ``None`` removes a variable."""
        env = fx.clean_env()
        env.update(
            GH_TOKEN=secrets.token_urlsafe(8),
            APP_SLUG="vmafx-release-bot",
            GITHUB_REF=REF,
            RUNNER_TEMP=str(self.runner_temp),
        )
        for key, value in override.items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value
        return env

    def call(self, args: list[str], env: dict[str, str] | None = None) -> TextCommandResult:
        """Run the script with ``args`` in the work repository."""
        return run(
            [sys.executable, "-I", str(SCRIPT), *args],
            allowed_executables=(sys.executable,),
            cwd=self.repo.path,
            env=env if env is not None else self.env(),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=fx.SCRIPT_TIMEOUT_S,
        )

    def publish(
        self,
        paths: tuple[str, ...] = (LOCK_A, LOCK_B),
        env: dict[str, str] | None = None,
        *,
        remote: bool = True,
    ) -> TextCommandResult:
        """Run ``publish`` for ``paths`` against the bare remote."""
        args = ["publish", "--label", "lock-refresh", "--subject", SUBJECT]
        for line in BODY:
            args += ["--body-line", line]
        if remote:
            args += ["--remote-url", str(self.remote)]
        return self.call([*args, "--", *paths], env)

    def assert_untouched(self, res: TextCommandResult, rc: int) -> None:
        """Assert exit code ``rc`` with the remote and the work HEAD unchanged."""
        self.assertEqual(res.returncode, rc, res.stdout + res.stderr)
        self.assertEqual(self.remote_tip(), self.tip)
        self.assertEqual(self.work_head(), self.tip)


class PublishPositiveTest(PublishTestCase):
    """The changed locks are committed as the release-bot and pushed."""

    def test_changed_lock_is_committed_signed_off_and_pushed(self) -> None:
        """One changed path of two: the commit holds that path only, as the bot, signed off."""
        self.edit(LOCK_A)
        res = self.publish()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        tip = self.remote_tip()
        self.assertNotEqual(tip, self.tip)
        self.assertEqual(tip, self.work_head())
        fmt = "%an|%ae|%cn|%ce|%B"
        shown = self.run_git_at(self.remote, ["log", "-1", f"--format={fmt}", REF])
        self.assertTrue(shown.startswith(f"{BOT}|{BOT_EMAIL}|{BOT}|{BOT_EMAIL}|{SUBJECT}\n"))
        self.assertIn(f"\n\nSigned-off-by: {BOT} <{BOT_EMAIL}>\n", shown)
        files = self.run_git_at(
            self.remote, ["diff-tree", "--no-commit-id", "--name-only", "-r", tip]
        )
        self.assertEqual(files.split(), [LOCK_A])

    def test_every_allowed_path_changed_all_committed(self) -> None:
        """Two changed allowed paths, one a new untracked file outside the first directory."""
        self.edit(LOCK_A)
        self.edit(LOCK_B)
        res = self.publish()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        files = self.run_git_at(
            self.remote, ["diff-tree", "--no-commit-id", "--name-only", "-r", self.remote_tip()]
        )
        self.assertEqual(sorted(files.split()), sorted([LOCK_A, LOCK_B]))

    def test_commit_message_equals_the_intel_neo_workflow_message(self) -> None:
        """The script writes byte for byte what the Intel NEO workflow's printf wrote."""
        self.edit(LOCK_A)
        args = [
            "publish",
            "--label",
            "intel-neo-lock-refresh",
            "--subject",
            "chore(deps): refresh Intel NEO and Level Zero asset lock",
            "--body-line",
            f"Refresh {LOCK_A} for NEO 25.1",
            "--body-line",
            "and Level Zero 1.2 (ADR-3555).",
            "--remote-url",
            str(self.remote),
            "--",
            LOCK_A,
        ]
        res = self.call(args)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        message = self.run_git_at(self.remote, ["log", "-1", "--format=%B", REF])
        expected = (
            "chore(deps): refresh Intel NEO and Level Zero asset lock\n\n"
            f"Refresh {LOCK_A} for NEO 25.1\n"
            "and Level Zero 1.2 (ADR-3555).\n\n"
            f"Signed-off-by: {BOT} <{BOT_EMAIL}>\n"
        )
        self.assertEqual(message, expected + "\n")

    def test_guard_subcommand_accepts_a_renovate_ref(self) -> None:
        """``refuse-non-renovate`` exits 0 on refs/heads/renovate/*."""
        res = self.call(["refuse-non-renovate", "--label", "lock-refresh"])
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)


class PublishNegativeTest(PublishTestCase):
    """Planted refusals: each leaves the remote and the work HEAD untouched."""

    def test_non_renovate_ref_is_refused(self) -> None:
        """Branches, pull request refs and tags outside renovate/ refuse with rc 1."""
        self.edit(LOCK_A)
        for ref in ("refs/heads/main", "refs/pull/7/merge", "refs/tags/renovate/x", "renovate/x"):
            with self.subTest(ref=ref):
                res = self.publish(env=self.env(GITHUB_REF=ref))
                self.assert_untouched(res, 1)
                self.assertIn("refusing non-Renovate ref", res.stderr)

    def test_guard_subcommand_refuses_a_non_renovate_ref(self) -> None:
        """``refuse-non-renovate`` exits 1 on refs/heads/main."""
        res = self.call(
            ["refuse-non-renovate", "--label", "lock-refresh"],
            self.env(GITHUB_REF="refs/heads/main"),
        )
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("::error::lock-refresh: refusing non-Renovate ref", res.stderr)

    def test_stray_path_outside_the_allowed_list_is_refused(self) -> None:
        """A changed tracked file outside the allowed list refuses the whole refresh."""
        self.edit(LOCK_A)
        self.edit(OTHER)
        res = self.publish()
        self.assert_untouched(res, 1)
        self.assertIn("outside the allowed paths", res.stderr)
        self.assertIn(OTHER, res.stderr)

    def test_untracked_stray_file_is_refused(self) -> None:
        """A new untracked file outside the allowed list refuses too."""
        self.edit(LOCK_A)
        self.edit("requirements/locks/new.txt")
        res = self.publish()
        self.assert_untouched(res, 1)
        self.assertIn("requirements/locks/new.txt", res.stderr)

    def test_app_slug_mismatch_is_refused(self) -> None:
        """Any app other than the release-bot refuses before a commit exists."""
        self.edit(LOCK_A)
        for slug in ("renovate", "vmafx-release-bot-2", "x"):
            with self.subTest(slug=slug):
                res = self.publish(env=self.env(APP_SLUG=slug))
                self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
                self.assertIn("expected app", res.stderr)
                self.assertEqual(self.remote_tip(), self.tip)
                self.assertEqual(self.work_head(), self.tip)

    def test_rewound_remote_is_not_restored(self) -> None:
        """A rewind remains in place, even though an ordinary push would fast-forward."""
        self.repo.git(["commit", "--allow-empty", "-q", "-m", "checkout tip"])
        self.repo.git(["push", "-q", str(self.remote), f"HEAD:{REF}"])
        checked_out = self.work_head()
        self.run_git_at(self.remote, ["update-ref", REF, self.tip])
        self.edit(LOCK_A)
        raw = run(
            [
                fx.GIT,
                "push",
                f"--force-with-lease={REF}:{checked_out}",
                str(self.remote),
                f"HEAD:{REF}",
            ],
            allowed_executables=(fx.GIT,),
            cwd=self.repo.path,
            env=fx.clean_env(),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=fx.GIT_TIMEOUT_S,
        )
        self.assertNotEqual(raw.returncode, 0)
        self.assertIn("stale info", raw.stderr)
        res = self.publish()
        self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
        self.assertIn("push refused or failed", res.stderr)
        self.assertEqual(self.remote_tip(), self.tip)

    def test_push_error_does_not_expose_destination(self) -> None:
        """A failed Git push cannot print its destination, even when it contains a secret."""
        self.edit(LOCK_A)
        destination = str(self.base / "fake-secret-remote.git")
        args = [
            "publish",
            "--label",
            "lock-refresh",
            "--subject",
            SUBJECT,
            "--remote-url",
            destination,
            "--",
            LOCK_A,
        ]
        res = self.call(args)
        self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
        self.assertIn("push refused or failed", res.stderr)
        self.assertNotIn(destination, res.stdout + res.stderr)
        self.assertNotIn("fake-secret", res.stdout + res.stderr)

    def test_deleted_remote_is_not_recreated(self) -> None:
        """A missing remote branch fails the lease and stays absent."""
        self.run_git_at(self.remote, ["update-ref", "-d", REF])
        self.edit(LOCK_A)
        res = self.publish()
        self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
        self.assertIn("push refused or failed", res.stderr)
        self.assertEqual(
            self.run_git_at(self.remote, ["for-each-ref", "--format=%(refname)", REF]), ""
        )

    def test_remote_that_moved_is_not_overwritten(self) -> None:
        """An unrelated remote tip causes a refusal and remains unchanged."""
        self.edit(LOCK_A)
        other = self.base / "other"
        fx.clone_repo(self.remote, other)
        self.run_git_at(other, ["checkout", "-q", BRANCH])
        (other / "moved.txt").write_text("moved\n", encoding="utf-8")
        self.run_git_at(other, ["add", "moved.txt"])
        self.run_git_at(
            other,
            ["-c", "user.name=A", "-c", "user.email=a@example.org", "commit", "-q", "-m", "moved"],
        )
        self.run_git_at(other, ["push", "-q", "origin", f"HEAD:{REF}"])
        tree = self.run_git_at(other, ["rev-parse", "HEAD^{tree}"]).strip()
        moved = self.run_git_at(
            self.remote,
            [
                "-c",
                "user.name=A",
                "-c",
                "user.email=a@example.org",
                "commit-tree",
                tree,
                "-m",
                "unrelated",
            ],
        ).strip()
        self.run_git_at(self.remote, ["update-ref", REF, moved])
        self.assertNotEqual(moved, self.tip)
        res = self.publish()
        self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
        self.assertIn("push refused or failed", res.stderr)
        self.assertNotIn(str(self.remote), res.stderr)
        self.assertEqual(self.remote_tip(), moved)


class PublishBoundaryTest(PublishTestCase):
    """The allowed set is exactly the named files."""

    def test_unchanged_lock_makes_no_commit(self) -> None:
        """A clean tree is rc 0 with the current-message and no commit."""
        res = self.publish()
        self.assert_untouched(res, 0)
        self.assertIn("lock outputs are already current", res.stdout)

    def test_stray_without_changed_lock_is_refused_when_requested(self) -> None:
        """The Python workflow refuses stray output even if its locks are unchanged."""
        self.edit(OTHER)
        args = [
            "publish",
            "--label",
            "python-lock-refresh",
            "--subject",
            SUBJECT,
            "--check-strays-when-unchanged",
            "--remote-url",
            str(self.remote),
            "--",
            LOCK_A,
        ]
        res = self.call(args)
        self.assert_untouched(res, 1)
        self.assertIn(OTHER, res.stderr)

    def test_neighbouring_paths_are_stray(self) -> None:
        """``<output>.bak``, the same name in another directory and a sub directory refuse."""
        neighbours = (
            f"{LOCK_A}.bak",
            "other/a.txt",
            "requirements/locks/sub/a.txt",
            "requirements/locks/a.txt.tmp",
            f"{LOCK_B}.orig",
        )
        for neighbour in neighbours:
            with self.subTest(neighbour=neighbour):
                self.edit(LOCK_A)
                self.edit(neighbour)
                res = self.publish()
                self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
                self.assertIn(neighbour, res.stderr)
                self.assertEqual(self.remote_tip(), self.tip)
                self.run_git_at(self.repo.path, ["checkout", "-q", "--", LOCK_A])
                (self.repo.path / neighbour).unlink()

    def test_unusable_allowed_paths_are_a_usage_error(self) -> None:
        """A pattern, a directory, an escape, an absolute path or pathspec magic exits 2."""
        self.edit(LOCK_A)
        bad = (
            "requirements/locks/*.txt",
            "requirements/locks/",
            "requirements/locks",
            "../a.txt",
            "/etc/passwd",
            ":(top)a.txt",
            "-a.txt",
            "",
        )
        for path in bad:
            with self.subTest(path=path):
                res = self.publish(paths=(LOCK_A, path))
                self.assert_dirty_usage_error(res)

    def assert_dirty_usage_error(self, res: TextCommandResult) -> None:
        """Assert rc 2 and nothing published (the edit stays in the work tree)."""
        self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
        self.assertEqual(self.remote_tip(), self.tip)
        self.assertEqual(self.work_head(), self.tip)

    def test_missing_environment_is_a_usage_error(self) -> None:
        """Each required variable unset exits 2 before any git call."""
        self.edit(LOCK_A)
        for name in ("GH_TOKEN", "APP_SLUG", "GITHUB_REF", "RUNNER_TEMP"):
            with self.subTest(name=name):
                res = self.publish(env=self.env(**{name: None}))
                self.assert_dirty_usage_error(res)
                self.assertIn(name, res.stderr)

    def test_without_remote_url_the_repository_variable_is_required(self) -> None:
        """No ``--remote-url`` and no ``GITHUB_REPOSITORY`` exits 2 and never contacts GitHub."""
        self.edit(LOCK_A)
        res = self.publish(remote=False, env=self.env(GITHUB_REPOSITORY=None))
        self.assert_dirty_usage_error(res)
        self.assertIn("GITHUB_REPOSITORY", res.stderr)


if __name__ == "__main__":
    unittest.main()
