#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Markdown Lint job skips the fragment trees the pre-commit hook skips.

`.pre-commit-config.yaml` keeps `docs/adr/_index_fragments/`,
`docs/rebase-notes.d/` and `changelog.d/` out of markdownlint: a fragment is a
piece of a generated page (a rebase note starts at `##`, a changelog entry at
`-`), so MD041 and MD013 do not apply to it. The `Markdown Lint` job of
`lint-and-format.yml` listed the first and the last but not the rebase notes,
so a push that landed two `docs/rebase-notes.d/*.md` files failed it with MD041
(run 37775116233).

The contract: every path the hook excludes as a fragment tree is excluded in
every file list of the job. Positive, negative and boundary cases on synthetic
text; the last test reads both files.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HOOK = ROOT / ".pre-commit-config.yaml"
WORKFLOW = ROOT / ".github" / "workflows" / "lint-and-format.yml"
FRAGMENT_TREES = (r"docs/adr/_index_fragments/", r"docs/rebase-notes\.d/")
# One `keep -vE '^(...)'` filter that drops generated pages from the job's file list.
GENERATED_FILTER = re.compile(r"keep -vE '\^\((docs/adr/README[^']*)\)'")


def missing(workflow: str) -> list[str]:
    """Fragment trees absent from a generated-pages filter, as `tree@filter-number`."""
    out = []
    filters = GENERATED_FILTER.findall(workflow)
    for number, text in enumerate(filters, 1):
        for tree in FRAGMENT_TREES:
            if tree not in text:
                out.append(f"{tree}@{number}")
    return out


class MarkdownLintScopeTest(unittest.TestCase):
    def test_hook_excludes_every_fragment_tree(self) -> None:
        hook = HOOK.read_text(encoding="utf-8")
        for tree in FRAGMENT_TREES:
            self.assertIn(tree, hook, tree)

    def test_job_excludes_every_fragment_tree_in_every_file_list(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(len(GENERATED_FILTER.findall(workflow)), 4, "filters not found")
        self.assertEqual(missing(workflow), [])

    def test_planted_missing_tree_is_refused(self) -> None:
        planted = "keep -vE '^(docs/adr/README\\.md|docs/adr/_index_fragments/|changelog\\.d/)'\n"
        self.assertEqual(missing(planted), [r"docs/rebase-notes\.d/@1"])

    def test_complete_filter_is_accepted(self) -> None:
        ok = (
            "keep -vE '^(docs/adr/README\\.md|docs/adr/_index_fragments/"
            "|docs/rebase-notes\\.d/)'\n"
        )
        self.assertEqual(missing(ok), [])


if __name__ == "__main__":
    unittest.main()
