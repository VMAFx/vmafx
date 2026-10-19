#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Markdown Lint job skips every fragment tree the pre-commit hook skips.

`.pre-commit-config.yaml` excludes `docs/rebase-notes.d/` (a fragment is a body-only
`##` section, so MD041 fires on every one). The `Markdown Lint` job of
`lint-and-format.yml` listed only `changelog.d/` and the ADR index fragments, so it
failed on master once a PR added a rebase-note fragment. Both lists must name the same
trees: each `keep -vE` filter of the job names every fragment directory.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FRAGMENT_DIRS = ("docs/rebase-notes.d/", "docs/adr/_index_fragments/")


def exclusion_filters(text: str) -> list[str]:
    """The `keep -vE '^(...)'` filters of the Markdown Lint job."""
    return re.findall(r"keep -vE '\^\((docs/adr/README[^']*)\)'", text)


class MarkdownLintFragmentScope(unittest.TestCase):
    def setUp(self) -> None:
        self.text = (ROOT / ".github/workflows/lint-and-format.yml").read_text(encoding="utf-8")

    def test_every_filter_names_every_fragment_dir(self) -> None:
        filters = exclusion_filters(self.text)
        self.assertGreaterEqual(len(filters), 4)
        for flt in filters:
            for d in FRAGMENT_DIRS:
                self.assertIn(d.replace(".", r"\."), flt, d)

    def test_pre_commit_excludes_the_same_dirs(self) -> None:
        cfg = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        for d in FRAGMENT_DIRS:
            self.assertIn(d, cfg)

    def test_filter_extraction_refuses_a_missing_dir(self) -> None:
        planted = "keep -vE '^(docs/adr/README\\.md|docs/adr/_index_fragments/)'"
        (flt,) = exclusion_filters(planted)
        self.assertNotIn(r"docs/rebase-notes\.d/", flt)


if __name__ == "__main__":
    unittest.main()
