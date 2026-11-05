#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Codacy's markdownlint skips exactly the paths the pre-commit markdownlint hook skips.

Codacy lints Markdown with its own markdownlint, configured in `.codacy.yml`. A path
the `markdownlint-cli2` hook excludes but Codacy does not (a changelog fragment, a
rendered index, a vendored tree) turns every pull request touching it into a Codacy
failure. The check compares the two exclusion sets on every tracked Markdown file
plus probe paths, so a new hook exclusion without a Codacy glob fails here.

The compiled agent-context files (`praetorctl compile-context` renders them from
`AGENTS.md`) repeat its instructions by construction. Codacy's Agentlinter reports
each repeated line as a duplicate instruction on every pull request that adds an
`AGENTS.md` line, so every compiled output must match a top-level `exclude_paths`
glob. The check finds the outputs by their compile marker, not by a list.
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
    """A Java PathMatcher glob as Codacy reads it.

    ** crosses directories and * does not. **/ needs at least one directory, so
    **/testdata/** does not match the root testdata/x.md (java.nio.file.FileSystem).
    """
    out = []
    for token in re.split(r"(\*\*/|\*\*|\*|\?)", glob):
        out.append(
            {"**/": ".*/", "**": ".*", "*": "[^/]*", "?": "[^/]"}.get(token, re.escape(token))
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


COMPILED_MARKER = b"Compiled automatically by praetorctl compile-context"


def codacy_global_excludes(text: str) -> list[str]:
    """The top-level exclude_paths of a Codacy configuration file."""
    if not text.startswith("---\n"):
        raise ValueError(".codacy.yml must start with a '---' line")
    config = yaml.safe_load(text) or {}
    return [str(glob) for glob in config.get("exclude_paths") or []]


def compiled_context_files() -> list[str]:
    """Tracked files whose first 512 bytes carry the compile-context marker."""
    listed = run_command(
        [GIT, "-C", str(ROOT), "ls-files", "-z"],
        allowed_executables=(GIT,),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=60,
    ).stdout.split("\0")
    out = []
    for path in listed:
        target = ROOT / path
        if not path or target.is_symlink() or not target.is_file():
            continue
        with target.open("rb") as handle:
            if COMPILED_MARKER in handle.read(512):
                out.append(path)
    return out


def unexcluded_compiled(codacy_text: str, paths: list[str]) -> list[str]:
    """Compiled context files no top-level exclude_paths glob matches."""
    globs = [glob_regex(glob) for glob in codacy_global_excludes(codacy_text)]
    return [path for path in sorted(set(paths)) if not any(g.match(path) for g in globs)]


def without_glob(codacy_text: str, glob: str) -> str:
    """The Codacy configuration with one markdownlint glob removed from the parsed YAML."""
    config = yaml.safe_load(codacy_text)
    excludes = config["engines"]["markdownlint"]["exclude_paths"]
    if glob not in excludes:
        raise ValueError(f"{glob} is not a markdownlint exclusion")
    excludes.remove(glob)
    return "---\n" + str(yaml.safe_dump(config, sort_keys=False))


def tracked_markdown() -> list[str]:
    """Every tracked Markdown file (.md and .markdown, as the hook takes), plus the probes."""
    listed = run_command(
        [GIT, "-C", str(ROOT), "ls-files", "-z", "--", "*.md", "*.markdown"],
        allowed_executables=(GIT,),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=60,
    ).stdout.split("\0")
    return [path for path in listed if path] + list(PROBES)


class CodacyConfig(unittest.TestCase):
    def test_repository_lists_agree(self) -> None:
        """Positive: Codacy and the hook skip the same tracked Markdown files."""
        self.assertEqual(drift(CODACY.read_text(), PRECOMMIT.read_text(), tracked_markdown()), [])

    def test_config_without_changelog_tree_fails(self) -> None:
        """Negative: a planted config that drops changelog.d is reported."""
        planted = without_glob(CODACY.read_text(), "changelog.d/**")
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

    def test_config_without_root_testdata_glob_fails(self) -> None:
        """Negative: **/testdata/** alone leaves the root testdata tree linted by Codacy."""
        planted = without_glob(CODACY.read_text(), "testdata/**")
        self.assertIn(
            "hook-only: testdata/x.md", drift(planted, PRECOMMIT.read_text(), list(PROBES))
        )

    def test_planting_an_unknown_glob_fails(self) -> None:
        """Boundary: removing a glob the configuration does not hold is an error, not a no-op."""
        with self.assertRaises(ValueError):
            without_glob(CODACY.read_text(), "no-such-tree/**")

    def test_config_without_leading_dashes_fails(self) -> None:
        """Negative: Codacy ignores a file that does not start with '---'."""
        with self.assertRaises(ValueError):
            codacy_markdownlint_excludes("engines: {}\n")

    def test_compiled_context_files_are_excluded(self) -> None:
        """Positive: every compile-context output matches a top-level exclude glob."""
        compiled = compiled_context_files()
        self.assertIn("CLAUDE.md", compiled)
        self.assertEqual(unexcluded_compiled(CODACY.read_text(), compiled), [])

    def test_config_without_compiled_exclude_fails(self) -> None:
        """Negative: a planted config that drops CLAUDE.md from exclude_paths is reported."""
        config = yaml.safe_load(CODACY.read_text())
        config["exclude_paths"].remove("CLAUDE.md")
        planted = "---\n" + str(yaml.safe_dump(config, sort_keys=False))
        self.assertEqual(
            unexcluded_compiled(planted, ["CLAUDE.md", ".windsurfrules"]), ["CLAUDE.md"]
        )

    def test_config_without_global_excludes_reports_all(self) -> None:
        """Boundary: a config with no top-level exclude_paths reports every compiled file."""
        self.assertEqual(
            unexcluded_compiled("---\nengines: {}\n", [".gemini/GEMINI.md", "CLAUDE.md"]),
            [".gemini/GEMINI.md", "CLAUDE.md"],
        )

    def test_glob_boundaries(self) -> None:
        """Boundary: * stays in one directory, ** crosses them, **/ needs one directory."""
        self.assertTrue(glob_regex("**/testdata/**").match("a/b/testdata/x.md"))
        self.assertFalse(glob_regex("**/testdata/**").match("testdata/x.md"))
        self.assertTrue(glob_regex("testdata/**").match("testdata/x.md"))
        self.assertFalse(glob_regex("docs/*.md").match("docs/adr/x.md"))
        self.assertFalse(glob_regex("docs/adr/README.md").match("docs/adr/README.mdx"))


if __name__ == "__main__":
    unittest.main()
