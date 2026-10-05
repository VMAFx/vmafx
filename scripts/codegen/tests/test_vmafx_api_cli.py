#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Command-line modes that compare with an earlier definition (ADR-1852):
`--abi-check --against-merge-base` (the Meson gate) and `--changelog`.

The merge-base gate runs in a scratch git repository: it passes an
append-only change, refuses a planted renumbered constant, and exits 77 with
the reason when the ref is unknown or the base has no definition, so a gate
that did not run is never reported as passing.
"""

from __future__ import annotations

import copy
import io
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from support import DEFINITION, bumped, document, entry, run, tool
from vmafx_api import changelog, cli
from vmafx_api.loader import parse


def main_output(argv: list[str]) -> tuple[int, str]:
    out = io.StringIO()
    with redirect_stdout(out):
        status = cli.main(argv)
    return status, out.getvalue()


class ChangelogTest(unittest.TestCase):
    def test_draft_lists_additions_and_new_deprecations(self) -> None:
        base = document()
        doc = bumped(copy.deepcopy(base), "0.2.0")
        added = copy.deepcopy(entry(doc["functions"], "vmafx_version_string"))
        doc["functions"].append({**added, "name": "vmafx_build_id", "since": "0.2"})
        entry(doc["functions"], "vmafx_version_string")["deprecated"] = {
            "since": "0.2",
            "replacement": "vmafx_build_id",
            "removal": "1.0",
        }
        text = changelog.draft(parse(base), parse(doc))
        self.assertIn("--- changelog.d/added/api-<slug>.md", text)
        self.assertIn("function: `vmafx_build_id`", text)
        self.assertIn("--- changelog.d/changed/api-<slug>.md", text)
        self.assertIn("`vmafx_version_string` is deprecated", text)

    def test_unchanged_definition_has_nothing_to_record(self) -> None:
        api = parse(document())
        self.assertIn("no API additions or deprecations", changelog.draft(api, api))


@unittest.skipIf(tool("git") is None, "git is not on PATH")
class MergeBaseGateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "test")
        (self.root / "README").write_text("before the definition\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "root")
        self.git("branch", "before-definition")
        target = self.root / "core/api/vmafx.toml"
        target.parent.mkdir(parents=True)
        shutil.copyfile(DEFINITION, target)
        self.git("add", ".")
        self.git("commit", "-q", "-m", "base")
        self.git("checkout", "-q", "-b", "topic")
        self.definition = target

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def git(self, *args: str) -> None:
        done = run(["git", "-C", str(self.root), *args])
        self.assertEqual(done.returncode, 0, done.stderr)

    def gate(self, ref: str = "main") -> tuple[int, str]:
        return main_output(["--abi-check", "--against-merge-base", ref, "--root", str(self.root)])

    def test_unchanged_branch_passes(self) -> None:
        status, output = self.gate()
        self.assertEqual(status, 0, output)
        self.assertIn("append-only successor", output)

    def test_planted_break_fails(self) -> None:
        text = self.definition.read_text()
        self.assertIn("value = -8\n", text)
        self.definition.write_text(text.replace("value = -8\n", "value = -80\n"))
        status, output = self.gate()
        self.assertEqual(status, 1)
        self.assertIn("VMAFX_E_RANGE renumbered -8 -> -80", output)

    def test_unknown_ref_unrelated_history_and_missing_definition_skip(self) -> None:
        self.git("checkout", "-q", "--orphan", "unrelated")
        self.git("commit", "-q", "-m", "unrelated")
        self.git("checkout", "-q", "-f", "topic")
        for ref, reason in (
            ("no-such-branch", "no-such-branch"),
            ("unrelated", "merge-base"),
            ("before-definition", "no core/api/vmafx.toml at"),
        ):
            with self.subTest(ref=ref):
                status, output = self.gate(ref)
                self.assertEqual(status, cli.SKIP, output)
                self.assertIn("SKIP", output)
                self.assertIn(reason, output)


if __name__ == "__main__":
    unittest.main()
