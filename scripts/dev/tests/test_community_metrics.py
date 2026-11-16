#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Fixture-based controls for the community-metrics report (issue #2554).

Every case runs on recorded API response shapes and never touches the
network: ``run_gh`` is replaced by a dispatcher that answers each query from
the fixtures below. The recorded shapes are the raw REST payloads (the
pipeline maps them itself), and the dispatcher pins the endpoint of every
query — the conversation comments must come from ``issues/{n}/comments`` (a
pull request's ``pulls/{n}/comments`` only holds inline review comments, so
a maintainer's plain reply would be invisible).
"""

from __future__ import annotations

import datetime
import importlib.util
import sys
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

ROOT = Path(__file__).resolve().parents[3]
_script = ROOT / "scripts" / "dev" / "community_metrics.py"
assert _script.is_file(), f"community_metrics.py not found at {_script}"
_spec = importlib.util.spec_from_file_location("community_metrics", _script)
assert _spec is not None and _spec.loader is not None, f"cannot load {_script}"
community_metrics = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(community_metrics)

MAINTAINERS = ["Lusoris"]
SINCE, UNTIL = "2026-10-01", "2026-10-31"
_UNANSWERED_NUMBER, _POST_CLOSE_NUMBER = 2, 3
# Pull request 5 is the conversation-comment case (see COMMENTS/REVIEWS).
_CONVERSATION_NUMBER = 5


def _author(login: str) -> dict[str, str]:
    return {"login": login}


def _gh_row(
    number: int,
    login: str,
    created: str,
    closed: str = "",
    labels: tuple[str, ...] = (),
    title: str = "",
) -> dict[str, Any]:
    return {
        "number": number,
        "title": title or f"item {number}",
        "author": _author(login),
        "createdAt": created,
        "closedAt": closed,
        "labels": [{"name": label} for label in labels],
    }


def _comment(login: str, created: str) -> dict[str, Any]:
    return {"user": {"login": login}, "created_at": created}


# The recorded window: an answered issue, an unanswered one, a pull request
# answered only after it was closed by a review, a pull request answered by
# a plain conversation comment before its close, and a bot-opened issue.
ISSUES_CREATED = [
    _gh_row(1, "outsider", "2026-10-02T00:00:00Z", "2026-10-05T00:00:00Z"),
    _gh_row(2, "other", "2026-10-03T00:00:00Z"),
    _gh_row(4, "renovate[bot]", "2026-10-04T00:00:00Z"),
]
ISSUES_CLOSED = [
    _gh_row(1, "outsider", "2026-10-02T00:00:00Z", "2026-10-05T00:00:00Z"),
    _gh_row(4, "renovate[bot]", "2026-10-04T00:00:00Z", "2026-10-06T00:00:00Z"),
]
PRS_CREATED = [
    _gh_row(3, "outsider", "2026-10-06T00:00:00Z", "2026-10-20T00:00:00Z", title="a pr"),
    _gh_row(5, "other", "2026-10-07T00:00:00Z", "2026-10-09T00:00:00Z", title="another pr"),
]
PRS_CLOSED = [
    _gh_row(3, "outsider", "2026-10-06T00:00:00Z", "2026-10-20T00:00:00Z", title="a pr"),
    _gh_row(5, "other", "2026-10-07T00:00:00Z", "2026-10-09T00:00:00Z", title="another pr"),
]
# Whole merged history (limit far above the count): the earliest merge of
# "newcomer" falls inside the window although GitHub would style the merged
# pull request CONTRIBUTOR (author_association is computed at query time —
# the tonydzi caveat the derivation exists for); "early" merged before the
# window; "late" only after it.
MERGED: list[dict[str, Any]] = [
    {"author": _author("newcomer"), "mergedAt": "2026-10-08T00:00:00Z"},
    {"author": _author("newcomer"), "mergedAt": "2026-10-20T00:00:00Z"},
    {"author": _author("early"), "mergedAt": "2026-08-30T00:00:00Z"},
    {"author": _author("late"), "mergedAt": "2026-11-15T00:00:00Z"},
    {"author": _author("dependabot[bot]"), "mergedAt": "2026-10-09T00:00:00Z"},
    {"author": _author("Lusoris"), "mergedAt": "2026-10-10T00:00:00Z"},
]
# Open pull requests: the aging metric is window-independent, so one PR
# created long before the window must surface in it and a recent one must
# not.
OPEN_PULL_REQUESTS: list[dict[str, Any]] = [
    _gh_row(11, "oldcomer", "2026-09-01T00:00:00Z", title="an aging pr"),
    _gh_row(12, "recent", "2026-10-20T00:00:00Z", title="a fresh pr"),
]
# Raw REST payloads, page-shaped: the pipeline maps them itself and skips
# rows without a timestamp (a PENDING review carries none). PR 3's review
# lands after its close (post-close); PR 5's plain conversation comment —
# served by the issues endpoint — is its first response.
COMMENTS: dict[int, list[dict[str, Any]]] = {
    1: [_comment("Lusoris", "2026-10-03T12:00:00Z")],  # 1.5 d
    2: [],  # no response
    4: [_comment("renovate[bot]", "2026-10-04T01:00:00Z")],  # bot: not a response
    5: [_comment("Lusoris", "2026-10-08T00:00:00Z")],  # 1.0 d, conversation comment
}
REVIEWS: dict[int, list[dict[str, Any]]] = {
    3: [{"user": {"login": "Lusoris"}, "submitted_at": "2026-10-25T00:00:00Z"}],  # after close
    5: [{"user": {"login": "Lusoris"}, "submitted_at": None}],  # PENDING: not a response
}
OPEN_LABELS = {"good first issue": 2, "help wanted": 1}


class GhFixture:
    """Answer every gh query the pipeline makes, from the recorded shapes."""

    def __init__(self) -> None:
        self.calls: list[list[str]] = []

    def __call__(self, args: list[str]) -> list[Any] | dict[str, Any]:
        self.calls.append(args)
        text = " ".join(args)
        if "--state merged" in text:
            return MERGED
        if "--state open" in text and args[0] == "pr":
            return OPEN_PULL_REQUESTS
        if "created:2026-10-01..2026-10-31" in text:
            return ISSUES_CREATED if args[0] == "issue" else PRS_CREATED
        if "closed:2026-10-01..2026-10-31" in text:
            return ISSUES_CLOSED if args[0] == "issue" else PRS_CLOSED
        if args[0] == "api":
            assert (
                "--paginate" in args and "--slurp" in args
            ), f"the conversation queries must paginate, got {args}"
            return self._api_pages(args[-1])
        if "--label" in text:
            return [
                {"number": n} for n in range(OPEN_LABELS.get(args[args.index("--label") + 1], 0))
            ]
        raise AssertionError(f"unexpected gh query: {text}")

    def _api_pages(self, url: str) -> list[list[dict[str, Any]]]:
        """The page shape ``--paginate --slurp`` yields, endpoint-pinned."""
        if url.endswith("/comments"):
            assert (
                "/issues/" in url
            ), f"conversation comments must come from the issues endpoint, got {url}"
            return [COMMENTS.get(int(url.split("/")[-2]), [])]
        if url.endswith("/reviews"):
            return [REVIEWS.get(int(url.split("/")[-2]), [])]
        raise AssertionError(f"unexpected api query: {url}")


class PipelineTest(unittest.TestCase):
    def run_pipeline(self) -> dict[str, Any]:
        with mock.patch.object(community_metrics, "run_gh", GhFixture()):
            window_items = community_metrics.fetch_window_items("VMAFx/vmafx", SINCE, UNTIL)
            merged = community_metrics.fetch_merged_pull_requests("VMAFx/vmafx")
            open_pull_requests = community_metrics.fetch_open_pull_requests("VMAFx/vmafx")
            responses: dict[int, dict[str, Any] | None] = {}
            for item in window_items:
                if not item["opened_in_window"]:
                    continue
                comments, reviews = community_metrics.fetch_first_response_inputs(
                    "VMAFx/vmafx", item
                )
                responses[int(item["number"])] = community_metrics.first_response(
                    item, comments, reviews, set(MAINTAINERS)
                )
            report: dict[str, Any] = community_metrics.compute_report(
                repo="VMAFx/vmafx",
                since=SINCE,
                until=UNTIL,
                now=datetime.datetime(2026, 11, 8, tzinfo=datetime.timezone.utc),
                window_items=window_items,
                merged=merged,
                open_pull_requests=open_pull_requests,
                responses=responses,
                maintainers=MAINTAINERS,
                open_labels=OPEN_LABELS,
            )
        return report

    def test_positive_expected_medians_and_counts(self) -> None:
        report = self.run_pipeline()
        self.assertEqual(report["opened"], {"issues": 3, "pull_requests": 2})
        self.assertEqual(report["closed"], {"issues": 2, "pull_requests": 2})
        first = report["first_response"]
        self.assertEqual(first["population"], len(ISSUES_CREATED) + len(PRS_CREATED))
        self.assertEqual(first["answered"], 3)
        self.assertEqual(first["no_response_yet"], 2)
        # answered: 1.0 d (pr 5, conversation comment), 1.5 d (issue 1) and
        # 19 d (pr 3, post-close review); the median of the three, and the
        # linear-interpolated p90: 1.5 + 0.8 * (19 - 1.5)
        self.assertEqual(first["median_days"], 1.5)
        self.assertEqual(first["p90_days"], 15.5)
        self.assertEqual(first["post_close"], 1)
        self.assertEqual(report["open_by_label"], OPEN_LABELS)
        self.assertEqual([c["login"] for c in report["first_time_contributors"]], ["newcomer"])
        self.assertIn("renovate[bot]", report["bots_seen"])
        self.assertIn("dependabot[bot]", report["bots_seen"])

    def test_pr_conversation_comment_is_the_first_response(self) -> None:
        # The maintainer answered pull request 5 with a plain comment — the
        # issues endpoint's payload — 1.0 day in, before its close.
        report = self.run_pipeline()
        row = next(
            r for r in report["first_response"]["items"] if r["number"] == _CONVERSATION_NUMBER
        )
        self.assertEqual(row["response"]["responder_seconds"], 86400.0)
        self.assertFalse(row["response"]["post_close"])

    def test_aging_pull_requests_are_window_independent(self) -> None:
        report = self.run_pipeline()
        aging = {row["number"]: row["age_days"] for row in report["aging_pull_requests"]}
        self.assertIn(11, aging)  # created 2026-09-01, outside the window
        self.assertNotIn(12, aging)  # created 2026-10-20, 19 days old
        self.assertGreater(aging[11], 30)

    def test_unanswered_is_a_stated_row_never_zero(self) -> None:
        report = self.run_pipeline()
        row = next(
            r for r in report["first_response"]["items"] if r["number"] == _UNANSWERED_NUMBER
        )
        self.assertIsNone(row["response"])
        text = community_metrics.render_markdown(report)
        self.assertIn("no response yet", text)

    def test_answered_after_close_counts_and_is_marked(self) -> None:
        report = self.run_pipeline()
        row = next(
            r for r in report["first_response"]["items"] if r["number"] == _POST_CLOSE_NUMBER
        )
        self.assertTrue(row["response"]["post_close"])
        self.assertIn("(post-close)", community_metrics.render_markdown(report))

    def test_empty_window_reports_zeroes_and_no_median(self) -> None:
        report: dict[str, Any] = community_metrics.compute_report(
            repo="VMAFx/vmafx",
            since="2026-09-01",
            until="2026-09-02",
            now=datetime.datetime(2026, 11, 8, tzinfo=datetime.timezone.utc),
            window_items=[],
            merged=MERGED,
            open_pull_requests=[],
            responses={},
            maintainers=MAINTAINERS,
            open_labels={},
        )
        self.assertEqual(report["opened"], {"issues": 0, "pull_requests": 0})
        self.assertIsNone(report["first_response"]["median_days"])
        self.assertIn("none", community_metrics.render_markdown(report))

    def test_single_item_is_its_own_median(self) -> None:
        item: dict[str, Any] = {
            **_gh_row(9, "a", "2026-10-02T00:00:00Z"),
            "is_pr": False,
            "opened_in_window": True,
            "closed_in_window": False,
        }
        answer: dict[str, Any] = {"responder_seconds": 86400.0 * 3, "post_close": False}
        report: dict[str, Any] = community_metrics.compute_report(
            repo="r",
            since=SINCE,
            until=UNTIL,
            now=datetime.datetime(2026, 11, 8, tzinfo=datetime.timezone.utc),
            window_items=[item],
            merged=[],
            open_pull_requests=[],
            responses={9: answer},
            maintainers=MAINTAINERS,
            open_labels={},
        )
        self.assertEqual(report["first_response"]["median_days"], 3.0)
        self.assertEqual(report["first_response"]["p90_days"], 3.0)

    def test_first_merge_derivation_ignores_author_association(self) -> None:
        # The recorded merged row carries no author_association at all — GitHub
        # would style the merged pull request CONTRIBUTOR; the earliest
        # mergedAt inside the window decides, bots and maintainers out.
        window_start = datetime.datetime(2026, 10, 1, tzinfo=datetime.timezone.utc)
        window_end = datetime.datetime(2026, 10, 31, 23, 59, 59, tzinfo=datetime.timezone.utc)
        first = community_metrics.first_time_contributors(
            MERGED, window_start, window_end, set(MAINTAINERS)
        )
        self.assertEqual([row["login"] for row in first], ["newcomer"])

    def test_merged_history_at_the_limit_fails_instead_of_truncating(self) -> None:
        with mock.patch.object(community_metrics, "run_gh", lambda args: MERGED[:2]):
            with self.assertRaises(RuntimeError):
                community_metrics.fetch_merged_pull_requests("VMAFx/vmafx", limit=2)

    def test_search_listing_fails_at_the_result_cap(self) -> None:
        # The search API stops at 1000 results whatever --limit asks for; a
        # window that reaches the cap must fail loudly, not undercount.
        cap = min(community_metrics.LIST_LIMIT, community_metrics.SEARCH_RESULT_CAP)
        rows = [{"number": n} for n in range(cap)]
        with mock.patch.object(community_metrics, "run_gh", lambda args: rows):
            with self.assertRaises(RuntimeError):
                community_metrics.fetch_window_items("VMAFx/vmafx", SINCE, UNTIL)

    def test_open_pull_request_list_fails_at_its_limit(self) -> None:
        rows = [{"number": n} for n in range(community_metrics.LIST_LIMIT)]
        with mock.patch.object(community_metrics, "run_gh", lambda args: rows):
            with self.assertRaises(RuntimeError):
                community_metrics.fetch_open_pull_requests("VMAFx/vmafx")

    def test_label_counts_fail_at_the_fetch_limit(self) -> None:
        rows = [{"number": n} for n in range(community_metrics.LIST_LIMIT)]
        with mock.patch.object(community_metrics, "run_gh", lambda args: rows):
            with self.assertRaises(RuntimeError):
                community_metrics.fetch_open_label_counts("VMAFx/vmafx")

    def test_percentile_boundaries(self) -> None:
        self.assertIsNone(community_metrics.percentile([], 90))
        self.assertEqual(community_metrics.percentile([7.0], 90), 7.0)
        self.assertEqual(community_metrics.percentile([0.0, 10.0], 90), 9.0)

    def test_markdown_reports_the_bot_rule(self) -> None:
        text = community_metrics.render_markdown(self.run_pipeline())
        self.assertIn("renovate[bot]", text)
        self.assertIn("dependabot[bot]", text)


if __name__ == "__main__":
    unittest.main()
