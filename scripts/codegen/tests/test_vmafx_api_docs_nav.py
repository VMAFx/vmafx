#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every generated VMAFx reference page is in the site navigation (issue 2436).

The generator writes one page per header (ADR-1852); a page that is not listed
under `nav:` in mkdocs.yml is reachable only through the reference index. The
check is shown refusing a planted defect: a nav without one header's page.
"""

from __future__ import annotations

import re
import unittest

from support import ROOT, document
from vmafx_api import emit_docs
from vmafx_api.loader import parse

NAV_PAGE = re.compile(r"^\s+- (?:[^:]+: )?(api/vmafx/[a-z_]+\.md)\s*$")


def nav_pages(mkdocs_text: str) -> set[str]:
    """The api/vmafx pages listed in the navigation of `mkdocs_text`."""
    return {m.group(1) for line in mkdocs_text.splitlines() if (m := NAV_PAGE.match(line))}


def generated_pages() -> set[str]:
    api = parse(document())
    return {path.removeprefix("docs/") for path, _ in emit_docs.pages(api)}


def missing(mkdocs_text: str) -> set[str]:
    return generated_pages() - nav_pages(mkdocs_text)


class ReferenceNavTest(unittest.TestCase):
    def test_every_generated_page_is_in_nav(self) -> None:
        text = (ROOT / "mkdocs.yml").read_text(encoding="utf-8")
        self.assertEqual(missing(text), set())

    def test_planted_missing_page_fails(self) -> None:
        text = (ROOT / "mkdocs.yml").read_text(encoding="utf-8")
        planted = "\n".join(
            line for line in text.splitlines() if not line.endswith("api/vmafx/frame.md")
        )
        self.assertNotEqual(planted, text)
        self.assertEqual(missing(planted), {"api/vmafx/frame.md"})


if __name__ == "__main__":
    unittest.main()
