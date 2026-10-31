#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Codacy's markdownlint skips the fragment trees the pre-commit markdownlint hook skips.

Changelog, ADR-index and rebase-note fragments are spliced under a heading by the
release renderers (ADR-0221), so they cannot start with a heading. Codacy lints with
its own markdownlint, configured in `.codacy.yml`; a fragment tree missing there turns
every pull request that adds a fragment into a Codacy failure.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

ROOT = Path(__file__).resolve().parents[3]
CODACY = ROOT / ".codacy.yml"
PRECOMMIT = ROOT / ".pre-commit-config.yaml"
FRAGMENT_TREES = ("changelog.d", "docs/adr/_index_fragments", "docs/rebase-notes.d")


def codacy_markdownlint_excludes(text: str) -> set[str]:
    """The markdownlint exclude_paths of a Codacy configuration file."""
    if not text.startswith("---\n"):
        raise ValueError(".codacy.yml must start with a '---' line")
    config = yaml.safe_load(text) or {}
    engine = (config.get("engines") or {}).get("markdownlint") or {}
    return set(engine.get("exclude_paths") or [])


def precommit_markdownlint_exclude(text: str) -> str:
    """The exclude pattern of the markdownlint-cli2 hook."""
    for repo in yaml.safe_load(text).get("repos") or []:
        for hook in repo.get("hooks") or []:
            if hook.get("id") == "markdownlint-cli2":
                return str(hook.get("exclude", ""))
    raise ValueError("no markdownlint-cli2 hook in .pre-commit-config.yaml")


def missing_trees(codacy_text: str, precommit_text: str) -> list[str]:
    """Fragment trees the pre-commit hook excludes but Codacy's markdownlint still lints."""
    pattern = re.compile(precommit_markdownlint_exclude(precommit_text))
    excluded = codacy_markdownlint_excludes(codacy_text)
    missing = []
    for tree in FRAGMENT_TREES:
        if not pattern.search(f"{tree}/x.md"):
            raise ValueError(f"pre-commit markdownlint no longer excludes {tree}/")
        if f"{tree}/**" not in excluded:
            missing.append(tree)
    return missing


class CodacyConfig(unittest.TestCase):
    def test_repository_config_skips_every_fragment_tree(self) -> None:
        """Positive: the committed .codacy.yml covers every fragment tree."""
        self.assertEqual(missing_trees(CODACY.read_text(), PRECOMMIT.read_text()), [])

    def test_config_without_changelog_tree_fails(self) -> None:
        """Negative: a planted config that drops changelog.d is reported."""
        planted = CODACY.read_text().replace('      - "changelog.d/**"\n', "")
        self.assertEqual(missing_trees(planted, PRECOMMIT.read_text()), ["changelog.d"])

    def test_config_without_leading_dashes_fails(self) -> None:
        """Negative: Codacy ignores a file that does not start with '---'."""
        with self.assertRaises(ValueError):
            codacy_markdownlint_excludes("engines: {}\n")

    def test_empty_engine_section_misses_all_trees(self) -> None:
        """Boundary: a config with no markdownlint section misses every tree."""
        self.assertEqual(missing_trees("---\n", PRECOMMIT.read_text()), list(FRAGMENT_TREES))


if __name__ == "__main__":
    unittest.main()
