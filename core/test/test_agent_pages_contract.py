#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The two pages `AGENTS.md` imports must stay true and navigable.

`docs/development/rebase-sensitive-invariants.md` and
`docs/development/agent-hard-rules.md` are compiled into every agent's context.
A stale statement there misleads every agent, so this pins what the 2026-10-03
audit found: retired status text, links to nothing, bullets that stop at "See",
a flat 8,000-word list with no structure, and two rules that named the wrong
socket and generator. Device-free: reads the pages and the tree.
"""

from __future__ import annotations

import os
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INVARIANTS = ROOT / "docs/development/rebase-sensitive-invariants.md"
RULES = ROOT / "docs/development/agent-hard-rules.md"
# Named in the text as removed, so it is allowed not to exist.
REMOVED_PATHS = {"scripts/docs/generate-adr-nav.sh"}
# The invariant entries on the page when it was restructured (2026-10-04). It may
# grow; it must not shrink without the ADR that retires an invariant.
MIN_INVARIANTS = 91


def bullets(text: str) -> list[str]:
    """The invariant entries: each `- **` item with its continuation lines."""
    items: list[list[str]] = []
    for line in text.split("\n"):
        if line.startswith("- **"):
            items.append([line])
        elif line.startswith("## "):
            items.append([])  # a heading closes the open item
        elif items and items[-1]:
            items[-1].append(line)
    return ["\n".join(item).rstrip() for item in items if item]


def repo_paths(text: str) -> set[str]:
    pattern = r"`((?:core|scripts|docs|tools|cmd|mcp-server|dev|ai|python|testdata)/[A-Za-z0-9_./-]+\.[A-Za-z0-9]+)`"
    return set(re.findall(pattern, text))


class InvariantsPage(unittest.TestCase):
    text = INVARIANTS.read_text(encoding="utf-8")

    def test_every_invariant_is_still_there(self) -> None:
        self.assertGreaterEqual(len(bullets(self.text)), MIN_INVARIANTS)

    def test_the_page_is_grouped_by_area_under_h2_sections(self) -> None:
        headings = re.findall(r"^## (.+)$", self.text, re.M)
        self.assertGreaterEqual(len(headings), 10)
        for heading in headings:
            anchor = re.sub(r"[^a-z0-9_ -]", "", heading.lower()).replace(" ", "-")
            self.assertIn(f"](#{anchor})", self.text, f"no contents entry for {heading!r}")
        # the intro and contents come first: no invariant sits above the first H2
        self.assertNotIn("- **", self.text[: self.text.index("\n## ")])

    def test_no_retired_status_text(self) -> None:
        for stale in (
            "placeholder",
            "audit-first `-ENOSYS` stubs",
            "T5-2b\n  (cJSON + mongoose + transport bodies) is open",
            "PR #213 (open)",
            "3 unregistered legacy stubs",
        ):
            self.assertNotIn(stale, self.text)

    def test_no_bullet_stops_at_see(self) -> None:
        for item in bullets(self.text):
            self.assertIsNone(re.search(r"\b[Ss]ee\s*$", item), item[:80])

    def test_every_relative_link_and_repo_path_resolves(self) -> None:
        for target in re.findall(r"\]\(([^)#\s]+)(?:#[^)]*)?\)", self.text):
            if "://" not in target and not target.startswith("#"):
                self.assertTrue((INVARIANTS.parent / target).resolve().exists(), target)
        for path in repo_paths(self.text) - REMOVED_PATHS:
            self.assertTrue((ROOT / path).exists(), path)

    def test_the_speed_bounds_are_described_as_gone(self) -> None:
        # ADR-1477: speed_chroma / speed_temporal left LIBM_TWINS; the entry says so.
        calibration = (ROOT / "scripts/ci/cross_backend_calibration.py").read_text("utf-8")
        self.assertNotRegex(calibration, r'LIBM_TWINS\s*[:=][^\n]*"speed_chroma"')
        self.assertIn("are gone", self.text)


class HardRulesPage(unittest.TestCase):
    text = RULES.read_text(encoding="utf-8")

    def test_rule_12_names_the_real_mcp_attachment(self) -> None:
        self.assertNotIn("MCP socket at", self.text)
        self.assertIn("docker exec -i vmaf-dev-mcp vmafx-mcp", self.text)
        compose = (ROOT / "dev/docker-compose.yml").read_text("utf-8")
        self.assertIn("default entrypoint uses stdio and does not create the socket", compose)

    def test_rule_8_names_the_generators_that_exist(self) -> None:
        self.assertIn("make docs-render", self.text)
        makefile = (ROOT / "Makefile").read_text("utf-8")
        self.assertIn("scripts/docs/concat-adr-index.sh --write", makefile)
        for path in repo_paths(self.text):
            if "NNNN" not in path:
                self.assertTrue((ROOT / path).exists(), path)
        self.assertTrue(os.access(ROOT / "scripts/adr/next-free.sh", os.X_OK))


if __name__ == "__main__":
    unittest.main()
