#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""The shared Conventional-Commit definition is one definition (issue #2750).

The commit-msg hook cannot import Python (it is the pinned remote hook
``compilerla/conventional-pre-commit``), so this module pins every consumer
to ``scripts/lib/conventional_commits.py``: the hook's argument list and the
label workflow's wiring are asserted here, and the gate's verdicts are
exercised against the cases the issue names — an unknown type, a missing
subject, a breaking change without ``!`` or footer.
"""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib import conventional_commits as cc

_ROOT = Path(__file__).resolve().parents[3]
_CHECKER = _ROOT / "scripts" / "ci" / "check-pr-title.py"
_HOOK_CONFIG = _ROOT / ".pre-commit-config.yaml"
_LABEL_WORKFLOW = _ROOT / ".github" / "workflows" / "pr-type-label.yml"


def _hook_args_line() -> str:
    """The ``args:`` line of the conventional-pre-commit hook block."""
    lines = _HOOK_CONFIG.read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if "id: conventional-pre-commit" in line)
    for line in lines[start + 1 :]:
        stripped = line.strip()
        if stripped.startswith("args: ["):
            return stripped
        if stripped.startswith(("- id:", "- repo:")):
            break
    raise AssertionError("conventional-pre-commit block has no args: [ line")


class TestHookStaysOnTheSharedList(unittest.TestCase):
    """The hook is a remote pre-commit hook; the test is the pin."""

    def test_hook_args_are_the_shared_list(self) -> None:
        line = _hook_args_line()
        self.assertIn("--strict", line)
        quoted = line[line.index("[") + 1 : line.rindex("]")]
        args = [part.strip().strip('"') for part in quoted.split(",")]
        self.assertEqual(args[0], "--strict")
        self.assertEqual(tuple(args[1:]), cc.TYPES)

    def test_label_workflow_reads_the_shared_gate(self) -> None:
        workflow = _LABEL_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("check-pr-title.py --emit-label", workflow)
        self.assertIn("actions/checkout", workflow)
        # The workflow no longer carries its own type list: the case block
        # that knew `security` and `style` — types the hook refuses — is gone.
        self.assertNotIn('label="type:', workflow)
        self.assertNotIn("type:security", workflow)
        self.assertNotIn("type:style", workflow)

    def test_label_mapping_covers_every_shared_type(self) -> None:
        self.assertEqual(set(cc.LABELS), set(cc.TYPES))
        for label in cc.LABELS.values():
            self.assertTrue(label.startswith("type:"))


class TestGateVerdicts(unittest.TestCase):
    """The cases issue #2750 names, through the checker's own entry point."""

    def _run(self, *argv: str) -> subprocess.CompletedProcess[bytes]:
        return subprocess.run(  # noqa: S603 - fixed argv; the checker path and inputs are this test's own fixtures
            [sys.executable, str(_CHECKER), *argv],
            capture_output=True,
            timeout=60,
            check=False,
        )

    def test_valid_titles_pass(self) -> None:
        for title in (
            "feat: add the thing",
            "feat!: add the thing",
            "feat(cache): add the thing",
            "fix(sycl): register the graph with the extractor's flags",
            "docs(hardware): list results per backend",
            "chore(main): release v1.2.3",
        ):
            with self.subTest(title=title):
                self.assertEqual(self._run(title).returncode, 0, title)

    def test_every_shared_type_passes(self) -> None:
        for known in cc.TYPES:
            with self.subTest(type=known):
                self.assertTrue(cc.is_conventional("%s: subject here" % known))

    def test_unknown_type_is_refused(self) -> None:
        result = self._run("security: harden the badges branch")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing type", result.stdout.decode(errors="replace"))
        self.assertFalse(cc.is_conventional("security: harden the badges branch"))
        self.assertEqual(cc.errors("security: harden the badges branch"), ["type"])

    def test_missing_subject_is_refused(self) -> None:
        result = self._run("feat:")
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing subject", result.stdout.decode(errors="replace"))
        self.assertFalse(cc.is_conventional("feat:"))

    def test_breaking_change_without_bang_or_footer_is_refused(self) -> None:
        message = "feat: add the thing\n\nbreaking change: the flag moved"
        self.assertFalse(cc.is_conventional(message))
        result = self._run(message)
        self.assertEqual(result.returncode, 1)

    def test_breaking_change_with_bang_and_with_footer_pass(self) -> None:
        self.assertTrue(cc.is_conventional("feat!: add the thing"))
        self.assertTrue(
            cc.is_conventional("feat: add the thing\n\nBREAKING CHANGE: the flag moved")
        )

    def test_uppercase_body_without_separator_is_refused(self) -> None:
        # A body glued to the subject without a blank line is not a footer.
        self.assertFalse(cc.is_conventional("feat: add the thing\nBREAKING CHANGE: the flag moved"))


class TestParseType(unittest.TestCase):
    """The label workflow's prefix contract, now from the shared module."""

    def test_known_prefixes(self) -> None:
        cases = {
            "feat: add the thing": "feat",
            "feat!: add the thing": "feat",
            "feat(cache): add the thing": "feat",
            "FIX(sycl): register the graph": "fix",
            "chore(main): release v1.2.3": "chore",
        }
        for title, expected in cases.items():
            with self.subTest(title=title):
                self.assertEqual(cc.parse_type(title), expected)

    def test_unknown_prefixes(self) -> None:
        for title in (
            "security: harden the badges branch",
            "style: keep the badge block one band",
            "Merge branch 'master' into fix/issue-3350-libaom-cpu-used",
            "",
        ):
            with self.subTest(title=title):
                self.assertIsNone(cc.parse_type(title))


if __name__ == "__main__":
    unittest.main()
