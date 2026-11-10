#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/ci/readme_badges.py."""

from __future__ import annotations

import base64
import email.message
import io
import json
import re
import sys
import tempfile
import unittest
import urllib.error
from collections.abc import Callable
from contextlib import redirect_stderr, redirect_stdout
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any, ClassVar
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import ci_tier
from scripts.ci import readme_badges as rb
from scripts.lib.safe_subprocess import CommandValidationError

ROOT = Path(__file__).resolve().parents[3]
NOW = datetime(2026, 11, 10, 12, 0, tzinfo=UTC)
REPO = "VMAFx/vmafx"
RAW = re.compile(
    r"\[!\[(?P<alt>[^\]]+)\]\(https://raw\.githubusercontent\.com/VMAFx/vmafx/badges/"
    r"(?P<slug>[a-z0-9-]+)\.svg\)\]"
)


def runs(*conclusions: object) -> dict[str, object]:
    return {"workflow_runs": [{"conclusion": c} for c in conclusions]}


def commit_answering(answer: object, call: rb.Call) -> rb.Call:
    """``call``, except that creating a commit answers ``answer``."""

    def answered(method: str, path: str, payload: object) -> object:
        if path.endswith("/git/commits"):
            return answer
        return call(method, path, payload)

    return answered


def http_error(code: int) -> urllib.error.HTTPError:
    return urllib.error.HTTPError(
        "https://api.github.com/x", code, "x", email.message.Message(), None
    )


def entry(
    message: str = "passing", color: str = rb.GREEN, fetched: str | None = None
) -> dict[str, str | None]:
    return {"message": message, "color": color, "fetched_at": fetched or rb.stamp(NOW)}


class WorkflowValue(unittest.TestCase):
    def test_success_is_passing(self) -> None:
        self.assertEqual(rb.workflow_value(runs("success")), rb.PASSING)

    def test_every_failing_conclusion_is_failing(self) -> None:
        for conclusion in ("failure", "timed_out", "startup_failure"):
            with self.subTest(conclusion=conclusion):
                self.assertEqual(rb.workflow_value(runs(conclusion, "success")), rb.FAILING)

    def test_an_unfinished_or_cancelled_run_does_not_mask_a_failure(self) -> None:
        # Planted case of the red-reader miss: the newest run had no conclusion yet.
        answer = runs(None, "cancelled", "skipped", "action_required", "failure", "success")
        self.assertEqual(rb.workflow_value(answer), rb.FAILING)

    def test_no_decisive_run_is_unknown(self) -> None:
        self.assertEqual(rb.workflow_value(runs()), ("unknown", rb.GREY))
        self.assertEqual(rb.workflow_value(runs("cancelled", None)), ("unknown", rb.GREY))

    def test_a_row_that_is_not_an_object_is_skipped(self) -> None:
        answer = {"workflow_runs": ["x", 3, {"conclusion": "success"}]}
        self.assertEqual(rb.workflow_value(answer), rb.PASSING)

    def test_an_answer_without_a_run_list_is_refused(self) -> None:
        answers: tuple[object, ...] = ({}, [], None, {"workflow_runs": {}})
        for answer in answers:
            with self.subTest(answer=answer), self.assertRaises(rb.BadgeError):
                rb.workflow_value(answer)


class ScorecardValue(unittest.TestCase):
    def test_colour_boundaries(self) -> None:
        cases = {
            10: ("10.0 / 10", rb.GREEN),
            7.0: ("7.0 / 10", rb.GREEN),
            6.96: ("7.0 / 10", rb.GREEN),
            6.9: ("6.9 / 10", rb.AMBER),
            4: ("4.0 / 10", rb.AMBER),
            3.9: ("3.9 / 10", rb.RED),
            0: ("0.0 / 10", rb.RED),
        }
        for score, want in cases.items():
            with self.subTest(score=score):
                self.assertEqual(rb.scorecard_value({"score": score}), want)

    def test_a_score_outside_zero_to_ten_or_not_a_number_is_refused(self) -> None:
        for score in (True, False, "8.9", None, -0.1, 10.01, float("nan"), float("inf")):
            with self.subTest(score=score), self.assertRaises(rb.BadgeError):
                rb.scorecard_value({"score": score})

    def test_an_answer_that_is_not_an_object_is_refused(self) -> None:
        with self.assertRaises(rb.BadgeError):
            rb.scorecard_value([8.9])


class BestPracticesValue(unittest.TestCase):
    URL = "https://github.com/VMAFx/vmafx"

    def rows(self, *levels: str) -> list[dict[str, str]]:
        return [{"repo_url": self.URL, "badge_level": level} for level in levels]

    def test_every_known_level(self) -> None:
        for level, want in rb.BEST_PRACTICES.items():
            with self.subTest(level=level):
                self.assertEqual(rb.best_practices_value(self.rows(level), self.URL), want)

    def test_exactly_one_project_must_list_the_repository(self) -> None:
        other = [{"repo_url": "https://github.com/Netflix/vmaf", "badge_level": "passing"}]
        for rows in ([], other, self.rows("passing", "gold"), {"repo_url": self.URL}, None):
            with self.subTest(rows=rows), self.assertRaises(rb.BadgeError):
                rb.best_practices_value(rows, self.URL)

    def test_an_unknown_level_is_refused(self) -> None:
        for level in ("platinum", None, ""):
            with self.subTest(level=level), self.assertRaises(rb.BadgeError):
                rb.best_practices_value([{"repo_url": self.URL, "badge_level": level}], self.URL)


class CodacyValue(unittest.TestCase):
    def test_every_grade_letter(self) -> None:
        for letter, color in rb.CODACY.items():
            with self.subTest(letter=letter):
                want = (letter, color)
                self.assertEqual(rb.codacy_value({"data": {"gradeLetter": letter}}), want)
        self.assertEqual(rb.CODACY["B"], rb.GREEN)
        self.assertEqual(rb.CODACY["C"], rb.AMBER)
        self.assertEqual(rb.CODACY["D"], rb.RED)

    def test_anything_but_a_letter_from_a_to_f_is_refused(self) -> None:
        docs: tuple[object, ...] = (
            {"data": {"gradeLetter": "G"}},
            {"data": {"gradeLetter": "a"}},
            {"data": {}},
            {},
            [],
        )
        for doc in docs:
            with self.subTest(doc=doc), self.assertRaises(rb.BadgeError):
                rb.codacy_value(doc)


class ReadValue(unittest.TestCase):
    def github(self, paths: list[str], branch: object = "master") -> rb.GitHub:
        def fetch(path: str) -> object:
            paths.append(path)
            return {"default_branch": branch} if path == f"/repos/{REPO}" else runs("success")

        return fetch

    def test_a_workflow_badge_reads_completed_push_runs_on_the_default_branch(self) -> None:
        paths: list[str] = []
        badge = rb.Badge("lint", "Lint", "workflow", "lint-and-format.yml")
        value = rb.read_value(badge, REPO, self.github(paths), self.fail)
        self.assertEqual(value, rb.PASSING)
        query = paths[1].split("?", 1)[1]
        self.assertIn("/actions/workflows/lint-and-format.yml/runs?", paths[1])
        self.assertEqual(query, "branch=master&event=push&status=completed&per_page=30")

    def test_a_repository_without_a_default_branch_is_refused(self) -> None:
        badge = rb.Badge("lint", "Lint", "workflow", "lint-and-format.yml")
        for branch in ("", None, 3):
            with self.subTest(branch=branch), self.assertRaises(rb.BadgeError):
                rb.read_value(badge, REPO, self.github([], branch), lambda url: {})

    def test_public_sources_get_their_own_urls_and_no_github_call(self) -> None:
        answers = {
            "https://api.scorecard.dev/projects/github.com/VMAFx/vmafx": {"score": 8.9},
            "https://www.bestpractices.dev/projects.json?url=https%3A%2F%2Fgithub.com%2FVMAFx%2Fvmafx": [
                {"repo_url": "https://github.com/VMAFx/vmafx", "badge_level": "passing"}
            ],
            "https://app.codacy.com/api/v3/analysis/organizations/gh/VMAFx/repositories/vmafx": {
                "data": {"gradeLetter": "A"}
            },
        }
        public = answers.__getitem__
        for badge in rb.BADGES:
            if badge.kind != "workflow":
                with self.subTest(badge=badge.slug):
                    rb.read_value(badge, REPO, self.fail, public)

    def test_an_unknown_kind_is_refused(self) -> None:
        with self.assertRaises(rb.BadgeError):
            rb.read_value(rb.Badge("x", "X", "rss"), REPO, lambda p: {}, lambda u: {})


class Usable(unittest.TestCase):
    def test_an_entry_this_script_writes_is_usable(self) -> None:
        self.assertEqual(rb.usable(entry()), entry())
        self.assertEqual(
            rb.usable({"message": "x" * 40, "color": rb.RED}),
            {"message": "x" * 40, "color": rb.RED, "fetched_at": None},
        )

    def test_a_malformed_entry_is_not(self) -> None:
        for bad in (
            entry(message=""),
            entry(message="x" * 41),
            entry(message="café"),
            entry(color="red"),
            entry(color="#1F883D"),
            {**entry(), "fetched_at": 3},
            "passing",
            None,
        ):
            with self.subTest(bad=bad):
                self.assertIsNone(rb.usable(bad))


class Carried(unittest.TestCase):
    def test_a_value_is_kept_up_to_exactly_six_hours(self) -> None:
        fetched = rb.stamp(NOW - rb.STALE_AFTER)
        self.assertEqual(rb.carried(entry(fetched=fetched), NOW)["message"], "passing")

    def test_one_second_past_six_hours_reads_stale(self) -> None:
        fetched = rb.stamp(NOW - rb.STALE_AFTER - timedelta(seconds=1))
        got = rb.carried(entry(fetched=fetched), NOW)
        self.assertEqual(got, {"message": "stale", "color": rb.GREY, "fetched_at": fetched})

    def test_a_future_or_unreadable_time_reads_stale(self) -> None:
        for fetched in (rb.stamp(NOW + timedelta(seconds=1)), "yesterday"):
            with self.subTest(fetched=fetched):
                self.assertEqual(rb.carried(entry(fetched=fetched), NOW)["message"], "stale")

    def test_no_previous_value_reads_stale(self) -> None:
        self.assertEqual(rb.carried(None, NOW)["message"], "stale")
        self.assertEqual(
            rb.carried({"message": "passing", "fetched_at": None}, NOW)["message"], "stale"
        )


class Refresh(unittest.TestCase):
    BADGE = (rb.Badge("go", "Go", "workflow", "go-ci.yml"),)

    def test_a_source_that_answers_gets_a_new_time(self) -> None:
        entries, failures = rb.refresh(self.BADGE, {}, NOW, lambda b: rb.FAILING)
        self.assertEqual(
            entries["go"], {"message": "failing", "color": rb.RED, "fetched_at": rb.stamp(NOW)}
        )
        self.assertEqual(failures, [])

    def test_a_failing_source_keeps_its_last_value_and_is_reported(self) -> None:
        previous = {"go": entry(fetched=rb.stamp(NOW - timedelta(hours=1)))}
        for error in (
            rb.BadgeError("b"),
            OSError("o"),
            ValueError("v"),
            KeyError("k"),
            TypeError("t"),
            ci_tier.TierError("too large"),
        ):
            with self.subTest(error=type(error).__name__):

                def read(badge: rb.Badge, error: Exception = error) -> rb.Value:
                    raise error

                entries, failures = rb.refresh(self.BADGE, previous, NOW, read)
                self.assertEqual(entries["go"]["message"], "passing")
                self.assertEqual(len(failures), 1)
                self.assertTrue(failures[0].startswith("go: "))

    def test_an_unexpected_error_is_not_swallowed(self) -> None:
        def read(badge: rb.Badge) -> rb.Value:
            raise RuntimeError("bug")

        with self.assertRaises(RuntimeError):
            rb.refresh(self.BADGE, {}, NOW, read)

    def test_the_render_list_keeps_readme_order(self) -> None:
        entries = {b.slug: entry() for b in rb.BADGES}
        rendered = rb.render_list(rb.BADGES, entries)
        self.assertEqual([r["slug"] for r in rendered], [b.slug for b in rb.BADGES])
        self.assertEqual(set(rendered[0]), {"slug", "label", "message", "color"})


class PreviousState(unittest.TestCase):
    def fetch(self, answer: object) -> rb.GitHub:
        def github(path: str) -> object:
            self.assertEqual(path, f"/repos/{REPO}/contents/state.json?ref=badges")
            if isinstance(answer, Exception):
                raise answer
            return answer

        return github

    def test_a_new_branch_has_no_state_and_no_problem(self) -> None:
        self.assertEqual(rb.previous_state(self.fetch(http_error(404)), REPO), ({}, None))

    def test_a_handled_error_is_closed(self) -> None:
        for code in (404, 500):
            with self.subTest(code=code):
                error = http_error(code)
                rb.previous_state(self.fetch(error), REPO)
                self.assertTrue(error.fp.closed)

    def test_the_badges_of_the_state_file_are_read(self) -> None:
        text = json.dumps({"badges": {"go": entry()}}).encode()
        doc = {"content": base64.b64encode(text).decode()}
        self.assertEqual(rb.previous_state(self.fetch(doc), REPO), ({"go": entry()}, None))

    def test_an_unreadable_state_is_a_problem_not_a_crash(self) -> None:
        unreadable = (http_error(500), OSError("down"), ci_tier.TierError("too large"))
        for answer in (*unreadable, {}, {"content": "!!"}, {"content": ""}, {"content": 7}):
            with self.subTest(answer=answer):
                badges, problem = rb.previous_state(self.fetch(answer), REPO)
                self.assertEqual(badges, {})
                self.assertIsNotNone(problem)

    def test_a_badges_field_that_is_not_an_object_is_empty(self) -> None:
        doc = {"content": base64.b64encode(b'{"badges": []}').decode()}
        self.assertEqual(rb.previous_state(self.fetch(doc), REPO), ({}, None))


class Field(unittest.TestCase):
    def test_a_nested_value_is_returned(self) -> None:
        self.assertEqual(rb.field({"a": {"b": "c"}}, "a", "b"), "c")

    def test_no_keys_returns_the_document(self) -> None:
        self.assertEqual(rb.field([1], *()), [1])

    def test_a_missing_or_non_object_level_is_refused(self) -> None:
        docs: tuple[object, ...] = ({}, {"a": {}}, {"a": "b"}, {"a": None}, [], None)
        for doc in docs:
            with self.subTest(doc=doc), self.assertRaisesRegex(rb.BadgeError, "no a.b"):
                rb.field(doc, "a", "b")


class PublicJson(unittest.TestCase):
    def test_a_public_source_gets_no_token_and_the_badge_bounds(self) -> None:
        with mock.patch.object(ci_tier, "https_json", return_value={"score": 7}) as sent:
            self.assertEqual(rb.public_json("https://api.scorecard.dev/x"), {"score": 7})
        method, url, headers, data, timeout, limit = sent.call_args.args
        self.assertEqual((method, url, data), ("GET", "https://api.scorecard.dev/x", None))
        self.assertEqual(headers, {"Accept": "application/json"})
        self.assertEqual((timeout, limit), (rb.PUBLIC_TIMEOUT_S, rb.MAX_RESPONSE_BYTES))

    def test_a_non_https_source_is_refused_before_any_request(self) -> None:
        with mock.patch.object(ci_tier.HTTPS_OPENER, "open") as sent:
            with self.assertRaises(ci_tier.TierError):
                rb.public_json("http://api.scorecard.dev/x")
        sent.assert_not_called()


class Collect(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        for badge in rb.BADGES:
            (self.dir / f"{badge.slug}.svg").write_text('<svg xmlns="x"></svg>\n', encoding="utf-8")
        (self.dir / "state.json").write_text("{}\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_one_svg_per_badge_and_the_state_file(self) -> None:
        self.assertEqual(len(rb.collect(self.dir, rb.BADGES)), len(rb.BADGES) + 1)

    def test_a_missing_or_unexpected_file_is_refused(self) -> None:
        (self.dir / "extra.svg").write_text("<svg ></svg>", encoding="utf-8")
        with self.assertRaisesRegex(rb.BadgeError, "unexpected"):
            rb.collect(self.dir, rb.BADGES)
        (self.dir / "extra.svg").unlink()
        (self.dir / "go.svg").unlink()
        with self.assertRaisesRegex(rb.BadgeError, "missing"):
            rb.collect(self.dir, rb.BADGES)

    def test_a_symlink_a_large_file_or_a_non_svg_is_refused(self) -> None:
        target = self.dir / "go.svg"

        def relink() -> None:
            target.unlink()
            target.symlink_to(self.dir / "lint.svg")

        cases: tuple[Callable[[], object], ...] = (
            relink,
            lambda: target.write_text("<svg " + "x" * rb.MAX_FILE_BYTES, encoding="utf-8"),
            lambda: target.write_text("<html></html>", encoding="utf-8"),
        )
        for plant in cases:
            with self.subTest(plant=plant):
                target.unlink()
                target.write_text("<svg ></svg>", encoding="utf-8")
                plant()
                with self.assertRaises(rb.BadgeError):
                    rb.collect(self.dir, rb.BADGES)

    def test_a_state_file_that_is_not_json_is_refused(self) -> None:
        (self.dir / "state.json").write_text("{", encoding="utf-8")
        with self.assertRaises(ValueError):
            rb.collect(self.dir, rb.BADGES)


class Publish(unittest.TestCase):
    FILES: ClassVar[dict[str, bytes]] = {"go.svg": b"<svg >go</svg>", "state.json": b"{}"}
    OLD = rb.stamp(NOW - rb.QUIET_WINDOW)

    def fake(
        self,
        head: str | None,
        date: str = OLD,
        verified: bool = True,
        tree_files: dict[str, bytes] | None = None,
    ) -> tuple[rb.Call, list[tuple[str, str, Any]]]:
        calls: list[tuple[str, str, Any]] = []
        tree_files = self.FILES if tree_files is None else tree_files

        def call(method: str, path: str, payload: object) -> object:
            calls.append((method, path, payload))
            if path.endswith("/git/ref/heads/badges"):
                if head is None:
                    raise http_error(404)
                return {"object": {"sha": head}}
            if "/git/commits/" in path:
                return {"tree": {"sha": "t0"}, "committer": {"date": date}}
            if "/git/trees/" in path:
                return {"tree": [{"path": n, "sha": rb.blob_sha(d)} for n, d in tree_files.items()]}
            if path.endswith("/git/trees"):
                return {"sha": "t1"}
            if path.endswith("/git/commits"):
                return {"sha": "c1", "verification": {"verified": verified}}
            return {}

        return call, calls

    def test_the_first_publish_creates_the_branch(self) -> None:
        call, calls = self.fake(None)
        self.assertEqual(rb.publish(call, REPO, self.FILES, NOW), "c1")
        self.assertEqual(calls[2][2]["parents"], [])
        self.assertEqual(
            calls[-1],
            ("POST", f"/repos/{REPO}/git/refs", {"ref": "refs/heads/badges", "sha": "c1"}),
        )

    def test_an_old_head_gets_a_new_commit_moved_forward_only(self) -> None:
        call, calls = self.fake("c0")
        rb.publish(call, REPO, self.FILES, NOW)
        commit = next(c for c in calls if c[1].endswith("/git/commits"))
        self.assertEqual(commit[2]["parents"], ["c0"])
        self.assertEqual(
            calls[-1],
            ("PATCH", f"/repos/{REPO}/git/refs/heads/badges", {"sha": "c1", "force": False}),
        )

    def test_an_unsigned_commit_leaves_the_branch_alone(self) -> None:
        call, calls = self.fake("c0", verified=False)
        with self.assertRaisesRegex(rb.BadgeError, "not signed"):
            rb.publish(call, REPO, self.FILES, NOW)
        self.assertFalse([c for c in calls if "/git/refs" in c[1]])

    def test_a_commit_without_a_true_verification_is_not_signed(self) -> None:
        for answer in ({"sha": "c1"}, {"sha": "c1", "verification": {"verified": "true"}}):
            with self.subTest(answer=answer):
                call, calls = self.fake("c0")
                with self.assertRaisesRegex(rb.BadgeError, "c1 is not signed"):
                    rb.publish(commit_answering(answer, call), REPO, self.FILES, NOW)
                self.assertFalse([c for c in calls if "/git/refs" in c[1]])

    def test_a_tree_answer_without_a_list_is_refused(self) -> None:
        young = rb.stamp(NOW - timedelta(minutes=5))
        call, _ = self.fake("c0", date=young)

        def answered(method: str, path: str, payload: object) -> object:
            return {"tree": {}} if "/git/trees/" in path else call(method, path, payload)

        with self.assertRaisesRegex(rb.BadgeError, "no tree list"):
            rb.publish(answered, REPO, self.FILES, NOW)

    def test_same_svgs_inside_the_quiet_window_commit_nothing(self) -> None:
        young = rb.stamp(NOW - rb.QUIET_WINDOW + timedelta(seconds=1))
        call, calls = self.fake("c0", date=young)
        self.assertEqual(rb.publish(call, REPO, self.FILES, NOW), "unchanged at c0")
        self.assertFalse([c for c in calls if c[0] != "GET"])

    def test_a_changed_svg_inside_the_quiet_window_is_committed(self) -> None:
        young = rb.stamp(NOW - timedelta(minutes=5))
        call, _ = self.fake(
            "c0", date=young, tree_files={"go.svg": b"<svg >old</svg>", "state.json": b"{}"}
        )
        self.assertEqual(rb.publish(call, REPO, self.FILES, NOW), "c1")

    def test_a_ref_lookup_error_other_than_404_is_raised(self) -> None:
        def call(method: str, path: str, payload: object) -> object:
            raise http_error(500)

        with self.assertRaises(urllib.error.HTTPError) as raised:
            rb.publish(call, REPO, self.FILES, NOW)
        raised.exception.close()

    def test_a_missing_branch_closes_its_404(self) -> None:
        error = http_error(404)

        def call(method: str, path: str, payload: object) -> object:
            raise error

        self.assertIsNone(rb.branch_head(call, REPO))
        self.assertTrue(error.fp.closed)

    def test_blob_sha_is_the_git_object_id(self) -> None:
        # `printf 'hi\n' | git hash-object --stdin`
        self.assertEqual(rb.blob_sha(b"hi\n"), "45b983be36b73c0788dc9cbcb76cbb80fc7bb057")

    def test_the_empty_blob_has_the_well_known_id(self) -> None:
        # `git hash-object --stdin </dev/null`
        self.assertEqual(rb.blob_sha(b""), "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391")

    def test_a_failing_or_garbled_git_is_refused(self) -> None:
        answers = (
            mock.Mock(returncode=128, stdout=b"", stderr=b"fatal: no"),
            mock.Mock(
                returncode=1, stdout=b"45b983be36b73c0788dc9cbcb76cbb80fc7bb057\n", stderr=b""
            ),
            mock.Mock(returncode=0, stdout=b"not-an-id\n", stderr=b""),
            mock.Mock(returncode=0, stdout=b"a" * 64 + b"\n", stderr=b""),
        )
        for answer in answers:
            with (
                self.subTest(stdout=answer.stdout),
                mock.patch.object(rb, "run_command", return_value=answer),
                self.assertRaisesRegex(rb.BadgeError, "git hash-object exited"),
            ):
                rb.blob_sha(b"hi\n")

    def test_a_missing_git_is_a_badge_error(self) -> None:
        missing = CommandValidationError("required executable not found: git")
        with mock.patch.object(rb, "run_command", side_effect=missing):
            with self.assertRaisesRegex(rb.BadgeError, "not found: git"):
                rb.blob_sha(b"hi\n")


class Verdict(unittest.TestCase):
    def verdict(self, doc: object) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            state = Path(tmp) / "state.json"
            state.write_text(json.dumps(doc), encoding="utf-8")
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(io.StringIO()):
                rc = rb.main(["verdict", "--state", str(state)], env={})
        return rc, out.getvalue()

    def test_no_failure_passes(self) -> None:
        self.assertEqual(self.verdict({"failures": []})[0], 0)

    def test_a_failure_fails_the_run_with_an_annotation(self) -> None:
        rc, out = self.verdict({"failures": ["codacy: down"]})
        self.assertEqual(rc, 1)
        self.assertIn("::error title=Badge source failed::codacy: down", out)

    def test_a_state_without_a_failures_list_is_an_error(self) -> None:
        self.assertEqual(self.verdict({"failures": "none"})[0], 2)


class Main(unittest.TestCase):
    def test_spec_and_publish_need_the_repository_and_a_token(self) -> None:
        for argv in (["spec", "--spec", "s", "--state", "t"], ["publish", "--dir", "d"]):
            err = io.StringIO()
            with self.subTest(argv=argv[0]), redirect_stderr(err):
                self.assertEqual(rb.main(argv, env={"GITHUB_REPOSITORY": REPO}), 2)
            self.assertIn("set GITHUB_TOKEN", err.getvalue())


SHIELDS = "https://img.shields.io/badge/C-23-3b82f6?style=for-the-badge&labelColor=111a35"
GOVERNANCE = "https://github.com/vmafx/vmafx/actions/workflows/praetor-docs.yml/badge.svg"
BLOCK = (
    "<!-- praetor:readme-governance:start -->\n"
    "[![Documentation Governance][docs-badge]][docs-runs]\n"
    "\n"
    f"[docs-badge]: {GOVERNANCE}\n"
    "[docs-runs]: https://github.com/vmafx/vmafx/actions/workflows/praetor-docs.yml\n"
    "<!-- praetor:readme-governance:end -->\n"
)


def image(url: str) -> str:
    return f"[![x]({url})](https://example.org)\n"


class ImageFault(unittest.TestCase):
    def test_the_one_design_passes(self) -> None:
        allowed = [
            f"https://raw.githubusercontent.com/{REPO}/badges/{b.slug}.svg" for b in rb.BADGES
        ]
        allowed += [
            SHIELDS,
            "https://img.shields.io/static/v1?label=release&message=v1&style=for-the-badge&color=3b82f6&labelColor=111a35",
            "docs/assets/vmafx-readme-banner.svg",
        ]
        for url in allowed:
            with self.subTest(url=url):
                self.assertIsNone(rb.image_fault(url))

    def test_another_design_or_host_is_a_fault(self) -> None:
        refused = {
            "https://img.shields.io/badge/C-23-3b82f6?style=flat-square&labelColor=111a35": "style=for-the-badge",
            "https://img.shields.io/badge/C-23-3b82f6?style=for-the-badge": "labelColor=111a35",
            "https://img.shields.io/badge/C-23-3b82f6": "style=for-the-badge and labelColor=111a35",
            f"https://github.com/{REPO}/actions/workflows/lint-and-format.yml/badge.svg": "not a rendered",
            "https://www.bestpractices.dev/projects/14549/badge": "not a rendered",
            "http://img.shields.io/badge/C-23-3b82f6?style=for-the-badge&labelColor=111a35": "not a rendered",
            f"https://raw.githubusercontent.com/{REPO}/badges/coverage.svg": "no rendered badge is named 'coverage'",
            f"https://raw.githubusercontent.com/{REPO}/master/tests.svg": "not a rendered",
            "/docs/assets/banner.svg": "not a rendered",
            "../banner.svg": "not a rendered",
            "docs/../../banner.svg": "not a rendered",
            "docs/assets/..": "not a rendered",
            "//cdn.example.org/badge.svg": "not a rendered",
        }
        for url, reason in refused.items():
            with self.subTest(url=url):
                self.assertIn(reason, rb.image_fault(url) or "")

    def test_a_repeated_or_empty_design_parameter_is_a_fault(self) -> None:
        for query in (
            "style=for-the-badge&style=flat&labelColor=111a35",
            "style=for-the-badge&labelColor=",
            "style=FOR-THE-BADGE&labelColor=111a35",
            "style=for-the-badge&labelcolor=111a35",
        ):
            with self.subTest(query=query):
                self.assertIsNotNone(
                    rb.image_fault(f"https://img.shields.io/badge/C-23-3b82f6?{query}")
                )


class DesignFaults(unittest.TestCase):
    def test_the_readme_has_the_one_design(self) -> None:
        self.assertEqual(rb.design_faults((ROOT / "README.md").read_text(encoding="utf-8")), [])

    def test_an_empty_readme_has_no_fault(self) -> None:
        self.assertEqual(rb.design_faults(""), [])

    def test_a_fault_names_its_line(self) -> None:
        readme = "# Title\n\n" + image(SHIELDS.replace("for-the-badge", "flat-square"))
        self.assertEqual(
            rb.design_faults(readme),
            [
                f"3: {SHIELDS.replace('for-the-badge', 'flat-square')}: a shields.io badge needs style=for-the-badge"
            ],
        )

    def test_the_governance_block_may_hold_its_own_badge(self) -> None:
        self.assertEqual(rb.design_faults("# Title\n\n" + BLOCK), [])

    def test_the_block_exception_covers_only_the_governance_badge(self) -> None:
        flat = BLOCK.replace(GOVERNANCE, SHIELDS.replace("for-the-badge", "flat-square"))
        self.assertEqual(len(rb.design_faults(flat)), 1)
        unresolved = BLOCK.replace("[docs-badge]:", "[other]:")
        self.assertIn("without an inline address", rb.design_faults(unresolved)[0])

    def test_the_governance_badge_outside_the_block_is_a_fault(self) -> None:
        for readme in (
            image(GOVERNANCE),
            BLOCK.replace("<!-- praetor:readme-governance:end -->\n", ""),
            BLOCK.replace("<!-- praetor:readme-governance:start -->\n", ""),
        ):
            with self.subTest(readme=readme[:40]):
                self.assertNotEqual(rb.design_faults(readme), [])

    def test_a_reference_style_image_outside_the_block_is_a_fault(self) -> None:
        for readme in (
            f"![C][c]\n\n[c]: {SHIELDS}\n",
            f"![c][]\n\n[c]: {SHIELDS}\n",
            f"![c]\n\n[c]: {SHIELDS}\n",
        ):
            with self.subTest(readme=readme.splitlines()[0]):
                self.assertEqual(
                    rb.design_faults(readme),
                    ["1: an image without an inline address this check can read"],
                )

    def test_code_is_not_an_image(self) -> None:
        flat = SHIELDS.replace("for-the-badge", "flat-square")
        for readme in (
            f"```markdown\n![x]({flat})\n```\n",
            f'~~~\n<img src="{flat}">\n~~~\n',
            f"Write `![x]({flat})` for a badge.\n",
        ):
            with self.subTest(readme=readme.splitlines()[0]):
                self.assertEqual(rb.design_faults(readme), [])

    def test_an_unclosed_fence_hides_nothing(self) -> None:
        flat = SHIELDS.replace("for-the-badge", "flat-square")
        self.assertEqual(len(rb.design_faults(f"```\n![x]({flat})\n")), 1)

    def test_an_html_image_must_be_a_local_file(self) -> None:
        passing = (
            '<img src="docs/assets/vmafx-readme-banner.svg" alt="VMAFx" height="120" />\n',
            '<picture><source media="(prefers-color-scheme: dark)" srcset="docs/a.svg 2x, docs/b.svg"></picture>\n',
        )
        for readme in passing:
            with self.subTest(readme=readme):
                self.assertEqual(rb.design_faults(readme), [])
        failing = (
            f'<img src="{SHIELDS}">\n',
            f'<IMG SRC="{GOVERNANCE}">\n',
            f'<source srcset="docs/a.svg 1x, {SHIELDS} 2x">\n',
            '<img alt="no source">\n',
            f"<img src='{SHIELDS}'>\n",
        )
        for readme in failing:
            with self.subTest(readme=readme):
                self.assertEqual(len(rb.design_faults(readme)), 1)

    def test_faults_come_in_line_order(self) -> None:
        readme = f'<img src="{SHIELDS}">\n' + image(GOVERNANCE) + f'<img src="{SHIELDS}">\n'
        lines = [int(fault.split(":", 1)[0]) for fault in rb.design_faults(readme)]
        self.assertEqual(lines, [1, 2, 3])


class CheckReadme(unittest.TestCase):
    def check(self, text: str | None) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            readme = Path(tmp) / "README.md"
            if text is not None:
                readme.write_text(text, encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = rb.main(["check-readme", "--readme", str(readme)], env={})
        return rc, out.getvalue(), err.getvalue()

    def test_the_one_design_passes_without_a_token(self) -> None:
        rc, out, _ = self.check(image(SHIELDS))
        self.assertEqual(rc, 0)
        self.assertIn("every badge has the one design", out)

    def test_a_fault_fails_with_file_and_line(self) -> None:
        rc, _, err = self.check("\n" + image(GOVERNANCE))
        self.assertEqual(rc, 1)
        self.assertRegex(err, r"README\.md:2: https://github\.com/")
        self.assertIn("1 README images break the one badge design", err)

    def test_a_missing_readme_is_an_error(self) -> None:
        self.assertEqual(self.check(None)[0], 2)


class Consistency(unittest.TestCase):
    def test_render_mjs_allows_the_same_text_and_colours(self) -> None:
        source = (ROOT / "tools/badges/render.mjs").read_text(encoding="utf-8")
        self.assertIn(f"const TEXT = /^{rb.TEXT.pattern}$/", source)
        self.assertIn(f"const COLOR = /^{rb.COLOR.pattern}$/", source)

    def test_every_workflow_badge_names_a_workflow_by_its_name(self) -> None:
        for badge in (b for b in rb.BADGES if b.kind == "workflow"):
            path = ROOT / ".github/workflows" / badge.workflow
            with self.subTest(badge=badge.slug):
                first = re.search(r"^name: (.+)$", path.read_text(encoding="utf-8"), re.M)
                self.assertEqual(first.group(1).strip() if first else None, badge.label)

    def test_the_readme_shows_every_badge_once_with_its_label(self) -> None:
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        shown = [(m["slug"], m["alt"]) for m in RAW.finditer(readme)]
        self.assertEqual(shown, [(b.slug, b.label) for b in rb.BADGES])


if __name__ == "__main__":
    unittest.main()
