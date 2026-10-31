#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Codacy's markdownlint skips exactly the paths the pre-commit markdownlint hook skips.

Codacy lints Markdown with its own markdownlint, configured in `.codacy.yml`. A path
the `markdownlint-cli2` hook excludes but Codacy does not (a changelog fragment, a
rendered index, a vendored tree) turns every pull request touching it into a Codacy
failure. The check compares the two exclusion sets on every tracked Markdown file
plus probe paths, so a new hook exclusion without a Codacy glob fails here.
"""

from __future__ import annotations

import re
import shutil
import sys
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]  # no PyYAML stubs in the hook environment

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

CODACY = ROOT / ".codacy.yml"
PRECOMMIT = ROOT / ".pre-commit-config.yaml"
GIT = shutil.which("git") or "/usr/bin/git"
PROBES = (
    "changelog.d/fixed/x.md",
    "docs/adr/_index_fragments/0001-x.md",
    "docs/rebase-notes.d/x.md",
    "docs/adr/README.md",
    "CHANGELOG.md",
    "tools/figures/README.md",
    "subprojects/x/README.md",
    "a/b/testdata/x.md",
    "testdata/x.md",
    ".claude/skills/caveman/SKILL.md",
    "docs/index.md",
    "README.md",
)


def codacy_markdownlint_excludes(text: str) -> list[str]:
    """The markdownlint exclude_paths of a Codacy configuration file."""
    if not text.startswith("---\n"):
        raise ValueError(".codacy.yml must start with a '---' line")
    config = yaml.safe_load(text) or {}
    engine = (config.get("engines") or {}).get("markdownlint") or {}
    return [str(glob) for glob in engine.get("exclude_paths") or []]


def glob_regex(glob: str) -> re.Pattern[str]:
    """A Java-style glob as Codacy reads it: ** crosses directories, * does not."""
    out = []
    for token in re.split(r"(\*\*/|\*\*|\*|\?)", glob):
        out.append(
            {"**/": "(?:.*/)?", "**": ".*", "*": "[^/]*", "?": "[^/]"}.get(token, re.escape(token))
        )
    return re.compile("".join(out) + r"\Z")


def precommit_markdownlint_exclude(text: str) -> re.Pattern[str]:
    """The exclude pattern of the markdownlint-cli2 hook."""
    for repo in yaml.safe_load(text).get("repos") or []:
        for hook in repo.get("hooks") or []:
            if hook.get("id") == "markdownlint-cli2":
                return re.compile(str(hook.get("exclude", "")))
    raise ValueError("no markdownlint-cli2 hook in .pre-commit-config.yaml")


def drift(codacy_text: str, precommit_text: str, paths: list[str]) -> list[str]:
    """Paths one exclusion list skips and the other lints, as 'hook-only:' / 'codacy-only:'."""
    hook = precommit_markdownlint_exclude(precommit_text)
    globs = [glob_regex(glob) for glob in codacy_markdownlint_excludes(codacy_text)]
    out = []
    for path in sorted(set(paths)):
        by_hook = hook.search(path) is not None
        by_codacy = any(glob.match(path) for glob in globs)
        if by_hook != by_codacy:
            out.append(f"{'hook-only' if by_hook else 'codacy-only'}: {path}")
    return out


def tracked_markdown() -> list[str]:
    """Every tracked Markdown file, plus the probe paths."""
    listed = run_command(
        [GIT, "-C", str(ROOT), "ls-files", "*.md"],
        allowed_executables=(GIT,),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=60,
    ).stdout.split()
    return listed + list(PROBES)


class CodacyConfig(unittest.TestCase):
    def test_repository_lists_agree(self) -> None:
        """Positive: Codacy and the hook skip the same tracked Markdown files."""
        self.assertEqual(drift(CODACY.read_text(), PRECOMMIT.read_text(), tracked_markdown()), [])

    def test_config_without_changelog_tree_fails(self) -> None:
        """Negative: a planted config that drops changelog.d is reported."""
        planted = CODACY.read_text().replace('      - "changelog.d/**"\n', "")
        self.assertIn(
            "hook-only: changelog.d/fixed/x.md",
            drift(planted, PRECOMMIT.read_text(), list(PROBES)),
        )

    def test_hook_exclusion_without_codacy_glob_fails(self) -> None:
        """Negative: a new hook exclusion that .codacy.yml lacks is reported."""
        hook = PRECOMMIT.read_text().replace("^changelog\\.d/", "^changelog\\.d/|^docs/index\\.md$")
        self.assertEqual(
            drift(CODACY.read_text(), hook, ["docs/index.md"]), ["hook-only: docs/index.md"]
        )

    def test_config_without_leading_dashes_fails(self) -> None:
        """Negative: Codacy ignores a file that does not start with '---'."""
        with self.assertRaises(ValueError):
            codacy_markdownlint_excludes("engines: {}\n")

    def test_glob_boundaries(self) -> None:
        """Boundary: * stays in one directory, ** crosses them, **/ also matches the root."""
        self.assertTrue(glob_regex("**/testdata/**").match("a/b/testdata/x.md"))
        self.assertTrue(glob_regex("**/testdata/**").match("testdata/x.md"))
        self.assertFalse(glob_regex("docs/*.md").match("docs/adr/x.md"))
        self.assertFalse(glob_regex("docs/adr/README.md").match("docs/adr/README.mdx"))


if __name__ == "__main__":
    unittest.main()
