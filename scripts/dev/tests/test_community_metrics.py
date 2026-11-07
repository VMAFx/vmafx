#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Fixture-based controls for the community-metrics report (issue #2554).

Every case runs on recorded API response shapes and never touches the
network: ``run_gh`` is replaced by a dispatcher that answers each query from
the fixtures below.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

ROOT = Path(__file__).resolve().parents[3]
_script = ROOT / "scripts" / "dev" / "community_metrics.py"
assert _script.is_file(), f"community_metrics.py not found at {_script}"
_spec = importlib.util.spec_from_file_location("community_metrics", _script)
community_metrics = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(community_metrics)

MAINTAINERS = ["Lusoris"]
SINCE, UNTIL = "2026-10-01", "2026-10-31"
_UNANSWERED_NUMBER, _POST_CLOSE_NUMBER = 2, 3


def _author(login: str) -> dict:
    return {"login": login}


def _gh_row(number: int, login: str, created: str, closed: str = "", labels=(), title="") -> dict:
    return {
        "number": number,
        "title": title or f"item {number}",
        "author": _author(login),
        "createdAt": created,
        "closedAt": closed,
        "labels": [{"name": label} for label in labels],
    }


# The recorded window: an answered issue, an unanswered one, a pull request
# answered only after it was closed, and a bot-opened issue.
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
]
PRS_CLOSED = [
    _gh_row(3, "outsider", "2026-10-06T00:00:00Z", "2026-10-20T00:00:00Z", title="a pr"),
]
# Whole merged history (limit far above the count): the earliest merge of
# "newcomer" falls inside the window although GitHub would style the merged
# pull request CONTRIBUTOR (author_association is computed at query time —
# the tonydzi caveat the derivation exists for); "early" merged before the
# window; "late" only after it.
MERGED = [
    {"author": _author("newcomer"), "mergedAt": "2026-10-08T00:00:00Z"},
    {"author": _author("newcomer"), "mergedAt": "2026-10-20T00:00:00Z"},
    {"author": _author("early"), "mergedAt": "2026-08-30T00:00:00Z"},
    {"author": _author("late"), "mergedAt": "2026-11-15T00:00:00Z"},
    {"author": _author("dependabot[bot]"), "mergedAt": "2026-10-09T00:00:00Z"},
    {"author": _author("Lusoris"), "mergedAt": "2026-10-10T00:00:00Z"},
]
COMMENTS = {
    1: [{"author": "Lusoris", "createdAt": "2026-10-03T12:00:00Z"}],  # 1.5 d
    2: [],  # no response
    4: [{"author": "renovate[bot]", "createdAt": "2026-10-04T01:00:00Z"}],  # bot: not a response
}
REVIEWS = {
    3: [{"author": "Lusoris", "createdAt": "2026-10-25T00:00:00Z"}],  # after close
}
OPEN_LABELS = {"good first issue": 2, "help wanted": 1}


class GhFixture:
    """Answer every gh query the pipeline makes, from the recorded shapes."""

    def __init__(self) -> None:
        self.calls: list[list[str]] = []

    def __call__(self, args: list[str]) -> list | dict:
        self.calls.append(args)
        text = " ".join(args)
        if "--state merged" in text:
            return MERGED
        if "created:2026-10-01..2026-10-31" in text:
            return ISSUES_CREATED if args[0] == "issue" else PRS_CREATED
        if "closed:2026-10-01..2026-10-31" in text:
            return ISSUES_CLOSED if args[0] == "issue" else PRS_CLOSED
        if args[0] == "api" and "/comments" in args[1]:
            return COMMENTS.get(int(args[1].split("/")[-2]), [])
        if args[0] == "api" and "/reviews" in args[1]:
            return REVIEWS.get(int(args[1].split("/")[-2]), [])
        if "--label" in text:
            return [
                {"number": n} for n in range(OPEN_LABELS.get(args[args.index("--label") + 1], 0))
            ]
        raise AssertionError(f"unexpected gh query: {text}")


class PipelineTest(unittest.TestCase):
    def run_pipeline(self) -> dict:
        fixture = GhFixture()
        self_patched = community_metrics
        original = self_patched.run_gh
        self_patched.run_gh = fixture
        try:
            window_items = self_patched.fetch_window_items("VMAFx/vmafx", SINCE, UNTIL)
            merged = self_patched.fetch_merged_pull_requests("VMAFx/vmafx")
            responses = {}
            for item in window_items:
                if not item["opened_in_window"]:
                    continue
                comments, reviews = self_patched.fetch_first_response_inputs("VMAFx/vmafx", item)
                responses[int(item["number"])] = self_patched.first_response(
                    item, comments, reviews, set(MAINTAINERS)
                )
            return self_patched.compute_report(
                repo="VMAFx/vmafx",
                since=SINCE,
                until=UNTIL,
                now=__import__("datetime").datetime(
                    2026, 11, 8, tzinfo=__import__("datetime").timezone.utc
                ),
                window_items=window_items,
                merged=merged,
                responses=responses,
                maintainers=MAINTAINERS,
                open_labels=OPEN_LABELS,
            )
        finally:
            self_patched.run_gh = original

    def test_positive_expected_medians_and_counts(self) -> None:
        report = self.run_pipeline()
        self.assertEqual(report["opened"], {"issues": 3, "pull_requests": 1})
        self.assertEqual(report["closed"], {"issues": 2, "pull_requests": 1})
        first = report["first_response"]
        self.assertEqual(first["population"], 4)
        self.assertEqual(first["answered"], 2)
        self.assertEqual(first["no_response_yet"], 2)
        # answered: 1.5 d (issue 1) and 19 d (pr 3, post-close); the median of
        # the two, and the linear-interpolated p90: 1.5*0.1 + 19*0.9
        self.assertEqual(first["median_days"], 10.25)
        self.assertEqual(first["p90_days"], 17.25)
        self.assertEqual(first["post_close"], 1)
        self.assertEqual(report["open_by_label"], OPEN_LABELS)
        self.assertEqual([c["login"] for c in report["first_time_contributors"]], ["newcomer"])
        self.assertIn("renovate[bot]", report["bots_seen"])
        self.assertIn("dependabot[bot]", report["bots_seen"])

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
        report = community_metrics.compute_report(
            repo="VMAFx/vmafx",
            since="2026-09-01",
            until="2026-09-02",
            now=__import__("datetime").datetime(
                2026, 11, 8, tzinfo=__import__("datetime").timezone.utc
            ),
            window_items=[],
            merged=MERGED,
            responses={},
            maintainers=MAINTAINERS,
            open_labels={},
        )
        self.assertEqual(report["opened"], {"issues": 0, "pull_requests": 0})
        self.assertIsNone(report["first_response"]["median_days"])
        self.assertIn("none", community_metrics.render_markdown(report))

    def test_single_item_is_its_own_median(self) -> None:
        item = {
            **_gh_row(9, "a", "2026-10-02T00:00:00Z"),
            "is_pr": False,
            "opened_in_window": True,
            "closed_in_window": False,
        }
        answer = {"responder_seconds": 86400.0 * 3, "post_close": False}
        report = community_metrics.compute_report(
            repo="r",
            since=SINCE,
            until=UNTIL,
            now=__import__("datetime").datetime(
                2026, 11, 8, tzinfo=__import__("datetime").timezone.utc
            ),
            window_items=[item],
            merged=[],
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
        window_start = __import__("datetime").datetime(
            2026, 10, 1, tzinfo=__import__("datetime").timezone.utc
        )
        window_end = __import__("datetime").datetime(
            2026, 10, 31, 23, 59, 59, tzinfo=__import__("datetime").timezone.utc
        )
        first = community_metrics.first_time_contributors(
            MERGED, window_start, window_end, set(MAINTAINERS)
        )
        self.assertEqual([row["login"] for row in first], ["newcomer"])

    def test_merged_history_at_the_limit_fails_instead_of_truncating(self) -> None:
        original = community_metrics.run_gh
        community_metrics.run_gh = lambda args: MERGED[:2]
        try:
            with self.assertRaises(RuntimeError):
                community_metrics.fetch_merged_pull_requests("VMAFx/vmafx", limit=2)
        finally:
            community_metrics.run_gh = original

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
