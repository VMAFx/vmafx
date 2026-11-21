#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The docs accessibility gate has a failing fixture and a dated exception list (issue 2552)."""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
import unittest
from datetime import date, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.docs import check_accessibility  # noqa: E402


class Fixture(unittest.TestCase):
    def test_fixture_carries_both_planted_defects(self) -> None:
        text = (ROOT / "scripts/docs/accessibility-fixture.html").read_text(encoding="utf-8")
        self.assertIn("<img", text)
        self.assertNotIn("alt=", text)
        self.assertIn("prefers-color-scheme: dark", text)
        self.assertIn("color: #333", text)

    def test_page_set_covers_chart_backend_adr_and_search(self) -> None:
        pages = " ".join(check_accessibility.PAGES)
        for required in ("index.html", "backends/", "development/netflix", "adr/", "search/"):
            self.assertIn(required, pages)


class Allowlist(unittest.TestCase):
    def write(self, exceptions: list[dict[str, str]]) -> Path:
        directory = Path(tempfile.mkdtemp(prefix="a11y-allow-"))
        path = directory / "allowlist.json"
        path.write_text(
            json.dumps({"schema_version": 1, "exceptions": exceptions}), encoding="utf-8"
        )
        self.addCleanup(shutil.rmtree, directory)
        return path

    def test_an_expired_entry_is_refused(self) -> None:
        path = self.write(
            [
                {
                    "page": "index.html",
                    "rule": "color-contrast",
                    "selector": "body",
                    "reason": "test",
                    "expires": (date.today() - timedelta(days=1)).isoformat(),
                }
            ]
        )
        with self.assertRaises(check_accessibility.AccessibilityError) as caught:
            check_accessibility.load_allowlist(path)
        self.assertIn("expired", str(caught.exception))

    def test_a_valid_entry_is_returned(self) -> None:
        path = self.write(
            [
                {
                    "page": "index.html",
                    "rule": "color-contrast",
                    "selector": "body",
                    "reason": "test",
                    "expires": (date.today() + timedelta(days=30)).isoformat(),
                }
            ]
        )
        self.assertEqual(len(check_accessibility.load_allowlist(path)), 1)

    def test_a_matching_issue_is_retained(self) -> None:
        exception = {
            "page": "index.html",
            "rule": "color-contrast",
            "selector": "body",
            "reason": "test",
            "expires": (date.today() + timedelta(days=30)).isoformat(),
        }
        used: set[int] = set()
        findings = check_accessibility.filter_issues(
            {"file:///site/index.html": [{"code": "color-contrast", "selector": "body"}]},
            ("index.html",),
            [exception],
            used,
        )
        self.assertEqual(findings, [])
        self.assertEqual(used, {0})

    def test_an_unlisted_issue_is_a_finding(self) -> None:
        used: set[int] = set()
        findings = check_accessibility.filter_issues(
            {"file:///site/index.html": [{"code": "image-alt", "selector": "img"}]},
            ("index.html",),
            [],
            used,
        )
        self.assertIn("image-alt", findings[0])


if __name__ == "__main__":
    unittest.main()
