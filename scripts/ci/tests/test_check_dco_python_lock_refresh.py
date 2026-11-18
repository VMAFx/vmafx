#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exemption coverage for the release-bot Python lock refresh commit (ADR-3560).

Positive: a release-bot commit with the exact subject, every path a manifest lock output (in
and outside ``requirements/locks/``), in a PR opened by ``renovate[bot]`` passes; ``--landed``
accepts the single release-bot co-author line on a Renovate squash that changes an output.
Negative: one extra path (an input, another file, the Intel NEO lock), another opener, another
bot identity, a different subject, a missing sign-off, an expired or absent exception and a
manifest that cannot be read each refuse.
Boundary: a path next to an output (``.bak``, same name in another directory) refuses; an
output removed from the manifest refuses; the exemption lapses the day after its expiry; the
pre-push mode never exempts; the Intel NEO class keeps its behaviour beside the new one.
"""

from __future__ import annotations

import datetime as dt
import json
import shutil
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci import check_python_dependency_locks as lock_checker  # noqa: E402
from scripts.ci.tests import check_dco_fixture as fx  # noqa: E402
from scripts.ci.tests.test_check_dco_lock_refresh import (  # noqa: E402
    ACTIONS_BOT,
    DEPENDABOT,
    LANDED_EXPIRES,
    LANDED_RULE,
    LOCK_PATH,
    RELEASE_BOT,
    commit_paths,
    judge_landed_in_repo,
    run_gate,
    write_exception_list,
)

RULE = "dco-python-lock-refresh"
MANIFEST = "requirements/locks/manifest.json"
SUBJECT = "chore(deps): refresh Python dependency locks"
MESSAGE = f"{SUBJECT}\n\nRegenerate the manifest lock outputs.\n"
TODAY = "2026-11-18"
EXPIRES = "2027-03-18"
OUT_IN_LOCKS = "requirements/locks/build.txt"
OUT_OUTSIDE = "python/requirements-lock.txt"
INPUT = "requirements/locks/build.in"
RENOVATE_PATHS = ("requirements/locks/build.in",)


def write_class(
    exc_root: Path, *, expires: str = EXPIRES, manifest: str | None = None, rule: bool = True
) -> None:
    """Write the live exception and the manifest it names under ``exc_root``."""
    if rule:
        write_exception_list(exc_root, paths=(MANIFEST,), expires=expires, rule=RULE)
    target = exc_root / MANIFEST
    target.parent.mkdir(parents=True, exist_ok=True)
    if manifest is None:
        shutil.copyfile(ROOT / MANIFEST, target)
    else:
        target.write_text(manifest, encoding="utf-8")


def manifest_without(output: str) -> str:
    """The checked-in manifest with the entry for ``output`` removed."""
    data = json.loads((ROOT / MANIFEST).read_text(encoding="utf-8"))
    data["locks"] = [entry for entry in data["locks"] if entry["output"] != output]
    return json.dumps(data)


class PythonLockOpenerTest(unittest.TestCase):
    """PR-mode ``dco-python-lock-refresh`` behaviour of check-dco.py."""

    def setUp(self) -> None:
        """An isolated repo with the Renovate input commit, and a live exception root."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        base = Path(tmp.name)
        (base / "repo").mkdir()
        self.repo = fx.Repo(base / "repo")
        self.exc_root = base / "exc"
        write_class(self.exc_root)
        commit_paths(self.repo, RENOVATE_PATHS, fx.BOT_IDENT, message="chore(deps): update tox")

    def gate(
        self,
        repo: fx.Repo | None = None,
        *,
        author: str = fx.BOT_LOGIN,
        author_type: str = "Bot",
        today: str = TODAY,
    ) -> tuple[int, str]:
        """Run the gate in PR mode and return the exit code and combined output."""
        res = run_gate(
            repo or self.repo,
            "--author",
            author,
            "--author-type",
            author_type,
            exc_root=self.exc_root,
            today=today,
        )
        return res.returncode, res.stdout + res.stderr

    def lock_commit(
        self,
        paths: tuple[str, ...] = (OUT_IN_LOCKS, OUT_OUTSIDE),
        ident: tuple[str, str] = RELEASE_BOT,
        **kwargs: object,
    ) -> str:
        """Commit ``paths`` with the lock refresh message."""
        kwargs.setdefault("message", MESSAGE)
        return commit_paths(self.repo, paths, ident, **kwargs)  # type: ignore[arg-type]

    def fresh(self) -> tuple[tempfile.TemporaryDirectory[str], fx.Repo]:
        """A second isolated repo with the same Renovate input commit."""
        tmp = tempfile.TemporaryDirectory()
        repo = fx.Repo(Path(tmp.name))
        commit_paths(repo, RENOVATE_PATHS, fx.BOT_IDENT, message="chore(deps): update tox")
        return tmp, repo

    def assert_refused(
        self, rc: int, out: str, sha: str, why: str = "is not the branch opener"
    ) -> None:
        """Assert exit 1 naming the commit and the reason."""
        self.assertEqual(rc, 1, out)
        self.assertIn(sha[:10], out)
        self.assertIn(why, out)

    def test_positive_outputs_in_and_outside_the_locks_directory_pass(self) -> None:
        """Two outputs, one outside requirements/locks/, pass on the first and last live day."""
        self.lock_commit()
        for day in (TODAY, EXPIRES):
            with self.subTest(today=day):
                rc, out = self.gate(today=day)
                self.assertEqual(rc, 0, out)
                self.assertIn("PASS - 2 commit(s)", out)

    def test_positive_every_manifest_output_in_one_commit_passes(self) -> None:
        """The whole manifest output set, read through the checker's loader, passes."""
        outputs = tuple(lock_checker.manifest_outputs(ROOT))
        self.assertGreater(len(outputs), 20)
        self.lock_commit(outputs)
        rc, out = self.gate()
        self.assertEqual(rc, 0, out)

    def test_positive_checked_in_exception_and_manifest_pass(self) -> None:
        """Without ``--exceptions-root`` the checked-in rule file and manifest hold."""
        self.lock_commit()
        res = run_gate(self.repo, "--author", fx.BOT_LOGIN, "--author-type", "Bot", today=TODAY)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_planted_extra_path_refused(self) -> None:
        """One extra path (input, other file, Intel NEO lock) refuses the commit."""
        for extra in (INPUT, "other.txt", LOCK_PATH, MANIFEST):
            with self.subTest(extra=extra):
                tmp, repo = self.fresh()
                self.addCleanup(tmp.cleanup)
                sha = commit_paths(repo, (OUT_IN_LOCKS, extra), RELEASE_BOT, message=MESSAGE)
                rc, out = self.gate(repo)
                self.assert_refused(rc, out, sha)

    def test_boundary_paths_next_to_an_output_refused(self) -> None:
        """``.bak``, a prefix path and the same name elsewhere are not manifest outputs."""
        neighbours = (
            f"{OUT_IN_LOCKS}.bak",
            "requirements/locks/sub/build.txt",
            "other/requirements/locks/build.txt",
            "python/requirements-lock.txt.orig",
            "requirements/locks/build.txt.d/x",
            "requirements/locks/build",
        )
        for neighbour in neighbours:
            for paths in ((neighbour,), (OUT_IN_LOCKS, neighbour)):
                with self.subTest(paths=paths):
                    tmp, repo = self.fresh()
                    self.addCleanup(tmp.cleanup)
                    sha = commit_paths(repo, paths, RELEASE_BOT, message=MESSAGE)
                    rc, out = self.gate(repo)
                    self.assert_refused(rc, out, sha)

    def test_planted_another_opener_refused(self) -> None:
        """The identical commit in a PR opened by another login or type refuses."""
        sha = self.lock_commit()
        for author, author_type in (
            (DEPENDABOT[0], "Bot"),
            ("lusoris", "User"),
            (fx.BOT_LOGIN, "User"),
        ):
            with self.subTest(author=author, author_type=author_type):
                rc, out = self.gate(author=author, author_type=author_type)
                self.assert_refused(rc, out, sha)

    def test_planted_another_bot_identity_refused(self) -> None:
        """Another bot, or the release-bot name with another e-mail, refuses."""
        cases = (ACTIONS_BOT, (RELEASE_BOT[0], "1+vmafx-release-bot[bot]@users.noreply.github.com"))
        for ident in cases:
            with self.subTest(ident=ident):
                tmp, repo = self.fresh()
                self.addCleanup(tmp.cleanup)
                sha = commit_paths(repo, (OUT_IN_LOCKS,), ident, message=MESSAGE)
                rc, out = self.gate(repo)
                self.assert_refused(rc, out, sha)

    def test_planted_different_subject_refused(self) -> None:
        """The subject must match exactly; the Intel NEO subject does not exempt outputs."""
        subjects = (
            "chore(deps): refresh Intel NEO and Level Zero asset lock",
            f"{SUBJECT} again",
            "chore(deps): Refresh Python dependency locks",
            "chore: refresh Python dependency locks",
            f" {SUBJECT}",
        )
        for subject in subjects:
            with self.subTest(subject=subject):
                tmp, repo = self.fresh()
                self.addCleanup(tmp.cleanup)
                sha = commit_paths(repo, (OUT_IN_LOCKS,), RELEASE_BOT, message=f"{subject}\n")
                rc, out = self.gate(repo)
                self.assert_refused(rc, out, sha)

    def test_boundary_expired_or_absent_exception_refused(self) -> None:
        """The class lapses the day after its expiry and without its rule file."""
        sha = self.lock_commit()
        rc, out = self.gate(today="2027-03-19")
        self.assert_refused(rc, out, sha)
        write_class(self.exc_root, rule=False)
        (self.exc_root / ".config/lint-exceptions.d" / f"{RULE}.toml").unlink()
        rc, out = self.gate()
        self.assert_refused(rc, out, sha)

    def test_boundary_unreadable_manifest_refused(self) -> None:
        """A manifest that is missing, not JSON or has no locks array exempts nothing."""
        sha = self.lock_commit()
        for text in ("{", "[]", '{"uv_version": "0.0.1"}', ""):
            with self.subTest(text=text):
                write_class(self.exc_root, manifest=text)
                rc, out = self.gate()
                self.assert_refused(rc, out, sha)
        (self.exc_root / MANIFEST).unlink()
        rc, out = self.gate()
        self.assert_refused(rc, out, sha)

    def test_boundary_output_removed_from_the_manifest_refused(self) -> None:
        """The set is the manifest's: an output the manifest no longer names is refused."""
        sha = self.lock_commit((OUT_IN_LOCKS,))
        write_class(self.exc_root, manifest=manifest_without(OUT_IN_LOCKS))
        rc, out = self.gate()
        self.assert_refused(rc, out, sha)

    def test_boundary_missing_signoff_and_attribution_still_refused(self) -> None:
        """Lifting the opener refusal leaves the sign-off and attribution checks intact."""
        sha = self.lock_commit(signoff=False)
        rc, out = self.gate()
        self.assert_refused(rc, out, sha, "no 'Signed-off-by: Name <email>' trailer")
        self.assertNotIn("is not the branch opener", out)
        tmp, repo = self.fresh()
        self.addCleanup(tmp.cleanup)
        sha = commit_paths(
            repo, (OUT_IN_LOCKS,), RELEASE_BOT, message=f"{SUBJECT}\n\nCo-authored-by: Codex <x@y>"
        )
        rc, out = self.gate(repo)
        self.assert_refused(rc, out, sha, "disallowed Co-authored-by")

    def test_boundary_pre_push_mode_never_exempts(self) -> None:
        """The pre-push mode keeps refusing the release-bot commit."""
        self.repo.git(["checkout", "-q", "-b", "renovate/locks"])
        sha = self.lock_commit()
        res = run_gate(
            self.repo,
            "--merge-base",
            "main",
            "--author",
            fx.BOT_LOGIN,
            "--author-type",
            "Bot",
            exc_root=self.exc_root,
            today=TODAY,
            include_range=False,
        )
        self.assert_refused(res.returncode, res.stdout + res.stderr, sha)

    def test_repository_wiring_names_one_manifest_entry_of_120_days(self) -> None:
        """The checked-in rule file: one entry on the manifest, category D, 120 days."""
        toml_path = fx.ROOT / ".config" / "lint-exceptions.d" / f"{RULE}.toml"
        entries = tomllib.loads(toml_path.read_text(encoding="utf-8"))["exception"]
        self.assertEqual(len(entries), 1)
        entry = entries[0]
        self.assertEqual(entry["path"], MANIFEST)
        self.assertEqual(entry["category"], "D")
        self.assertEqual(entry["expires"] - entry["entered"], dt.timedelta(days=120))
        self.assertTrue(entry["reason"].startswith("Category D, 120-day interval."))
        for token in (fx.BOT_LOGIN, RELEASE_BOT[0], RELEASE_BOT[1], SUBJECT, "ADR-3560"):
            self.assertIn(token, entry["reason"])


class PythonLockLandedTest(unittest.TestCase):
    """``--landed`` strips the release-bot co-author line on a Renovate lock squash."""

    def setUp(self) -> None:
        """An isolated repo, a live exception root and the loaded gate module."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        base = Path(tmp.name)
        (base / "repo").mkdir()
        self.repo = fx.Repo(base / "repo")
        self.exc_root = base / "exc"
        write_class(self.exc_root)
        self.mod = fx.load_gate("check_dco_landed_python_lock")
        self.addCleanup(sys.modules.pop, "check_dco_landed_python_lock", None)
        self.renovate = (self.mod.RENOVATE_BOT_LOGIN, self.mod.RENOVATE_BOT_EMAIL)
        signoff = f"Signed-off-by: {self.renovate[0]} <{self.renovate[1]}>"
        self.msg = (
            f"chore(deps): update python (#3501)\n\n{signoff}\n{self.mod.RELEASE_BOT_COAUTHOR}"
        )

    def judge(
        self,
        paths: tuple[str, ...],
        ident: tuple[str, str] | None = None,
        message: str | None = None,
        today: str = TODAY,
    ) -> str | None:
        """Commit ``paths`` and judge the commit in landed mode."""
        with tempfile.TemporaryDirectory() as directory:
            repo = fx.Repo(Path(directory))
            sha = commit_paths(
                repo, paths, ident or self.renovate, message=message or self.msg, signoff=False
            )
            return judge_landed_in_repo(self.mod, repo, sha, self.exc_root, today=today)

    def assert_refused(self, reason: str | None) -> None:
        """Assert the co-author trailer was not stripped."""
        self.assertIsNotNone(reason)
        assert reason is not None
        self.assertIn("disallowed Co-authored-by trailer", reason)

    def test_positive_squash_changing_an_output_passes(self) -> None:
        """Input plus output, and output alone, pass on the first and last live day."""
        for paths in ((INPUT, OUT_IN_LOCKS), (OUT_OUTSIDE,)):
            for day in (TODAY, EXPIRES):
                with self.subTest(paths=paths, today=day):
                    self.assertIsNone(self.judge(paths, today=day))

    def test_planted_refusals(self) -> None:
        """Input only, no path, a neighbour of an output, another author or another bot refuse."""
        for paths in (
            (INPUT,),
            (),
            (f"{OUT_IN_LOCKS}.bak",),
            ("other/requirements/locks/build.txt",),
        ):
            with self.subTest(paths=paths):
                self.assert_refused(self.judge(paths))
        self.assert_refused(self.judge((OUT_IN_LOCKS,), ident=fx.HUMAN))
        other = (
            f"{self.msg.rsplit(chr(10), 1)[0]}\nCo-authored-by: {DEPENDABOT[0]} <{DEPENDABOT[1]}>"
        )
        self.assert_refused(self.judge((OUT_IN_LOCKS,), message=other))
        two = f"{self.msg}\n{self.mod.RELEASE_BOT_COAUTHOR}"
        self.assert_refused(self.judge((OUT_IN_LOCKS,), message=two))

    def test_boundary_expired_absent_or_unreadable_class_refuses(self) -> None:
        """Past expiry, without the rule, or with an unreadable manifest nothing is stripped."""
        self.assert_refused(self.judge((OUT_IN_LOCKS,), today="2027-03-19"))
        write_class(self.exc_root, manifest="{")
        self.assert_refused(self.judge((OUT_IN_LOCKS,)))
        write_class(self.exc_root)
        (self.exc_root / ".config/lint-exceptions.d" / f"{RULE}.toml").unlink()
        self.assert_refused(self.judge((OUT_IN_LOCKS,)))

    def test_boundary_intel_neo_class_unchanged_beside_the_new_one(self) -> None:
        """The NEO path passes under its own rule, and not under the Python rule alone."""
        self.assert_refused(self.judge((LOCK_PATH,)))
        write_exception_list(
            self.exc_root, paths=(LOCK_PATH,), expires=LANDED_EXPIRES, rule=LANDED_RULE
        )
        self.assertIsNone(self.judge(("build-config.env", LOCK_PATH)))
        self.assertIsNone(self.judge((INPUT, OUT_IN_LOCKS)))

    def test_boundary_commit_without_coauthor_line_still_passes(self) -> None:
        """A Renovate squash without the co-author line is judged as before."""
        message = f"chore(deps): update python (#3501)\n\n{self.msg.splitlines()[2]}"
        self.assertIsNone(self.judge((INPUT,), message=message))


if __name__ == "__main__":
    unittest.main()
