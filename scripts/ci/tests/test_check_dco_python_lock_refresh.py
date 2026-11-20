#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Path-exact Python lock refresh exemptions, including planted and boundary cases."""

from __future__ import annotations

import datetime as dt
import json
import shutil
import sys
import tempfile
import tomllib
import unittest
from collections.abc import Callable
from contextlib import redirect_stderr
from functools import partial
from io import StringIO
from pathlib import Path
from unittest import mock

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
    """Write independent exceptions and a fixture manifest under ``exc_root``."""
    if rule:
        write_exception_list(
            exc_root, paths=tuple(lock_checker.manifest_outputs(ROOT)), expires=expires, rule=RULE
        )
    target = exc_root / MANIFEST
    target.parent.mkdir(parents=True, exist_ok=True)
    if manifest is None:
        shutil.copyfile(ROOT / MANIFEST, target)
    else:
        target.write_text(manifest, encoding="utf-8")


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
        self.assertEqual(len(outputs), 30)
        self.lock_commit(outputs)
        rc, out = self.gate()
        self.assertEqual(rc, 0, out)

    def test_positive_each_listed_path_alone_passes(self) -> None:
        """Each of the 30 independently granted paths passes alone."""
        for path in lock_checker.manifest_outputs(ROOT):  # Bound: 30 manifest outputs.
            with self.subTest(path=path):
                tmp, repo = self.fresh()
                self.addCleanup(tmp.cleanup)
                commit_paths(repo, (path,), RELEASE_BOT, message=MESSAGE)
                rc, out = self.gate(repo)
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
            "Requirements/locks/build.txt",
        )
        for neighbour in neighbours:
            for paths in ((neighbour,), (OUT_IN_LOCKS, neighbour)):
                with self.subTest(paths=paths):
                    tmp, repo = self.fresh()
                    self.addCleanup(tmp.cleanup)
                    sha = commit_paths(repo, paths, RELEASE_BOT, message=MESSAGE)
                    rc, out = self.gate(repo)
                    self.assert_refused(rc, out, sha)

    def test_boundary_parent_directory_refused(self) -> None:
        """A parent directory is not an exact path grant."""
        mod = fx.load_gate("check_dco_python_parent")
        self.addCleanup(sys.modules.pop, "check_dco_python_parent", None)
        with mock.patch.object(mod, "commit_changed_paths", return_value=["requirements/locks"]):
            self.assertFalse(
                mod.is_exempt_opener_commit(
                    "unused",
                    *RELEASE_BOT,
                    fx.BOT_LOGIN,
                    "Bot",
                    root=self.exc_root,
                    subject=SUBJECT,
                )
            )

    def test_planted_mixed_listed_and_unlisted_paths_refused(self) -> None:
        """One unlisted path refuses the entire commit."""
        sha = self.lock_commit((OUT_IN_LOCKS, "other.txt"))
        rc, out = self.gate()
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

    def test_planted_manifest_output_without_exception_refused(self) -> None:
        """Adding an output to the manifest grants no exception."""
        data = json.loads((ROOT / MANIFEST).read_text(encoding="utf-8"))
        entry = dict(data["locks"][1], output="requirements/locks/extra.txt")
        data["locks"].append(entry)
        write_class(self.exc_root, manifest=json.dumps(data))
        sha = self.lock_commit(("requirements/locks/extra.txt",))
        rc, out = self.gate()
        self.assert_refused(rc, out, sha)

    def test_boundary_one_expired_entry_refuses_only_its_path(self) -> None:
        """One expired entry leaves the other 29 paths live."""
        outputs = tuple(lock_checker.manifest_outputs(ROOT))
        write_exception_list(self.exc_root, paths=outputs[1:], expires=EXPIRES, rule=RULE)
        target = self.exc_root / ".config/lint-exceptions.d" / f"{RULE}.toml"
        with target.open("a", encoding="utf-8") as file:
            file.write(
                f'\n[[exception]]\npath = "{outputs[0]}"\n'
                'reason = "fixture"\nexpires = 2026-11-17\n'
            )
        sha = self.lock_commit((outputs[0],))
        rc, out = self.gate()
        self.assert_refused(rc, out, sha)
        tmp, repo = self.fresh()
        self.addCleanup(tmp.cleanup)
        commit_paths(repo, outputs[1:], RELEASE_BOT, message=MESSAGE)
        rc, out = self.gate(repo)
        self.assertEqual(rc, 0, out)

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

    def test_repository_wiring_has_30_exact_category_d_entries(self) -> None:
        """Every entry is unique, exact, and has the granted dates."""
        path = ROOT / ".config/lint-exceptions.d" / f"{RULE}.toml"
        entries = tomllib.loads(path.read_text(encoding="utf-8"))["exception"]
        paths = [entry["path"] for entry in entries]
        self.assertEqual(len(paths), 30)
        self.assertEqual(len(set(paths)), 30)
        for entry in entries:  # Bound: 30 entries, asserted above.
            self.assertFalse(set(entry["path"]) & set("*?["))
            self.assertEqual(entry["category"], "D")
            self.assertEqual(entry["entered"], dt.date(2026, 11, 18))
            self.assertEqual(entry["expires"], dt.date(2027, 3, 18))
            self.assertIn("vmafx-renovate-python-lock-refresh", entry["reason"])
            self.assertIn("ADR-3560", entry["reason"])

    def test_drift_new_lock_output_needs_new_operator_grant(self) -> None:
        """Manifest and grants must agree; a new output needs a new grant."""
        path = ROOT / ".config/lint-exceptions.d" / f"{RULE}.toml"
        entries = tomllib.loads(path.read_text(encoding="utf-8"))["exception"]
        self.assertEqual(
            {entry["path"] for entry in entries},
            set(lock_checker.manifest_outputs(ROOT)),
            "A new lock output needs a new operator grant before an entry is added.",
        )


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

    def fake_fetch(self, squash: str, case: str) -> Callable[[str], object]:
        """Provide the requested forge responses without any network access."""
        if case in {"missing token", "missing repository"}:
            env = {"GITHUB_REPOSITORY": "example/repo"} if case == "missing token" else {}
            configured_fetch: Callable[[str], object] = self.mod.landed_forge_fetch(env)
            return configured_fetch
        if case == "unexpected request":
            return mock.Mock(side_effect=AssertionError("unexpected forge request"))

        bot_sha, opener_sha = "a" * 40, "b" * 40
        pulls = f"/commits/{squash}/pulls?per_page=100&page=1"
        commits = "/pulls/3501/commits?per_page=100&page=1"
        files = f"/commits/{bot_sha}?per_page=100"
        pr = {"number": 3501, "merge_commit_sha": squash, "merged_at": "2026-11-18T12:00:00Z"}
        opener = {
            "sha": opener_sha,
            "commit": {"author": {"name": self.renovate[0], "email": self.renovate[1]}},
        }
        bot = {
            "sha": bot_sha,
            "commit": {"author": {"name": RELEASE_BOT[0], "email": RELEASE_BOT[1]}},
        }
        responses: dict[str, object] = {
            pulls: [pr],
            commits: [opener, bot],
            files: {"files": [{"filename": OUT_IN_LOCKS}]},
        }
        overrides: dict[str, tuple[str, object]] = {
            "forge error": (pulls, OSError("forge unavailable")),
            "no PR": (pulls, []),
            "multiple PRs": (pulls, [pr, dict(pr, number=3502)]),
            "wrong merge SHA": (pulls, [dict(pr, merge_commit_sha=opener_sha)]),
            "unmerged PR": (pulls, [dict(pr, merged_at=None)]),
            "malformed author": (commits, [opener, bot, {"sha": "c" * 40, "commit": {}}]),
            "no release-bot commit": (commits, [opener]),
            "missing files": (files, {}),
            "full file page": (files, {"files": [{"filename": OUT_IN_LOCKS}] * 100}),
            "bot input": (files, {"files": [{"filename": OUT_IN_LOCKS}, {"filename": INPUT}]}),
            "rename from input": (
                files,
                {"files": [{"filename": OUT_IN_LOCKS, "previous_filename": INPUT}]},
            ),
        }
        if case == "later bot input":
            later_sha = "d" * 40
            responses[commits] = [dict(opener, sha=f"{i:040x}") for i in range(99)] + [bot]
            responses["/pulls/3501/commits?per_page=100&page=2"] = [dict(bot, sha=later_sha)]
            responses[f"/commits/{later_sha}?per_page=100"] = {"files": [{"filename": INPUT}]}
        elif case != "valid":
            path, response = overrides[case]
            responses[path] = response

        def fetch(path: str) -> object:
            response = responses[path]
            if isinstance(response, OSError):
                raise response
            return response

        return fetch

    def judge(
        self,
        paths: tuple[str, ...],
        ident: tuple[str, str] | None = None,
        message: str | None = None,
        today: str = TODAY,
        *,
        forge_case: str = "valid",
    ) -> str | None:
        """Commit ``paths`` and judge the commit in landed mode."""
        with tempfile.TemporaryDirectory() as directory:
            repo = fx.Repo(Path(directory))
            sha = commit_paths(
                repo, paths, ident or self.renovate, message=message or self.msg, signoff=False
            )
            judge = partial(self.mod.judge_landed, fetch=self.fake_fetch(sha, forge_case))
            with mock.patch.object(self.mod, "judge_landed", judge):
                return judge_landed_in_repo(self.mod, repo, sha, self.exc_root, today=today)

    def assert_refused(self, reason: str | None) -> None:
        """Assert the co-author trailer was not stripped."""
        self.assertIsNotNone(reason)
        assert reason is not None
        self.assertIn("disallowed Co-authored-by trailer", reason)

    def test_positive_squash_changing_an_output_passes(self) -> None:
        """Listed paths pass on the first and last live day."""
        for paths in ((OUT_IN_LOCKS,), (OUT_IN_LOCKS, OUT_OUTSIDE), (OUT_OUTSIDE,)):
            for day in (TODAY, EXPIRES):
                with self.subTest(paths=paths, today=day):
                    self.assertIsNone(self.judge(paths, today=day))

    def test_positive_squash_with_input_and_output_passes(self) -> None:
        """The forge attributes the output alone to the release bot."""
        self.assertIsNone(self.judge((INPUT, OUT_IN_LOCKS)))

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

    def test_landed_unlisted_path_refuses_mixed_commit(self) -> None:
        """Unlisted paths must be manifest inputs for the strip to apply."""
        self.assert_refused(
            self.judge((OUT_IN_LOCKS, "other.txt"), forge_case="unexpected request")
        )
        self.assert_refused(self.judge(("other.txt",)))

    def test_planted_release_bot_input_refused_in_both_modes(self) -> None:
        """A manifest input remains outside the release bot's grant."""
        sha = commit_paths(self.repo, (INPUT,), RELEASE_BOT, message=MESSAGE)
        result = run_gate(
            self.repo,
            "--author",
            fx.BOT_LOGIN,
            "--author-type",
            "Bot",
            exc_root=self.exc_root,
            today=TODAY,
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(sha[:10], result.stdout + result.stderr)
        self.assertIn("is not the branch opener", result.stdout + result.stderr)
        self.assert_refused(self.judge((INPUT, OUT_IN_LOCKS), forge_case="bot input"))

    def test_planted_forge_failures_keep_trailer(self) -> None:
        """A mixed squash needs a complete, unambiguous forge read."""
        cases = (
            "forge error",
            "no PR",
            "multiple PRs",
            "wrong merge SHA",
            "unmerged PR",
            "malformed author",
            "missing files",
            "full file page",
            "missing token",
            "missing repository",
            "rename from input",
            "no release-bot commit",
            "later bot input",
        )
        for case in cases:
            with self.subTest(case=case):
                self.assert_refused(self.judge((INPUT, OUT_IN_LOCKS), forge_case=case))

    def test_forge_error_emits_one_notice(self) -> None:
        """The refusal explains why the forge could not establish eligibility."""
        output = StringIO()
        with redirect_stderr(output):
            self.assert_refused(self.judge((INPUT, OUT_IN_LOCKS), forge_case="forge error"))
        (notice,) = output.getvalue().splitlines()
        self.assertTrue(notice.startswith("dco: "))
        self.assertIn("forge unavailable", notice)

    def test_boundary_expired_or_absent_class_refuses(self) -> None:
        """Past expiry or without the rule nothing is stripped."""
        self.assert_refused(self.judge((OUT_IN_LOCKS,), today="2027-03-19"))
        write_class(self.exc_root)
        (self.exc_root / ".config/lint-exceptions.d" / f"{RULE}.toml").unlink()
        self.assert_refused(self.judge((OUT_IN_LOCKS,)))

    def test_boundary_intel_neo_class_unchanged_beside_the_new_one(self) -> None:
        """The NEO path passes under its own rule, and not under the Python rule alone."""
        self.assert_refused(self.judge((LOCK_PATH,)))
        write_exception_list(
            self.exc_root, paths=(LOCK_PATH,), expires=LANDED_EXPIRES, rule=LANDED_RULE
        )
        self.assertIsNone(
            self.judge(("build-config.env", LOCK_PATH), forge_case="unexpected request")
        )
        self.assertIsNone(self.judge((OUT_IN_LOCKS,)))

    def test_boundary_commit_without_coauthor_line_still_passes(self) -> None:
        """A Renovate squash without the co-author line is judged as before."""
        message = f"chore(deps): update python (#3501)\n\n{self.msg.splitlines()[2]}"
        self.assertIsNone(self.judge((INPUT,), message=message))


if __name__ == "__main__":
    unittest.main()
