#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise ADR freeze checks against isolated Git histories."""

from __future__ import annotations

import importlib
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, cast

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
run = importlib.import_module("scripts.lib.safe_subprocess").run
yaml = importlib.import_module("yaml")
GIT = shutil.which("git") or "git"

if TYPE_CHECKING:
    from scripts.lib.safe_subprocess import TextCommandResult

SCRIPT = Path(__file__).resolve().parents[1] / "check-adr-body-freeze.py"
ADR = "docs/adr/1234-example.md"
BASE = "# ADR-1234: Example\n\n- **Status**: Accepted\n\n## Context\nOld context.\n\n## Decision\nOld decision.\n\n## Consequences\nOld consequence.\n\n## References\n[Source](old.md)\n"


class FreezeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        self.env.update(
            GIT_CONFIG_GLOBAL=os.devnull,
            GIT_CONFIG_NOSYSTEM="1",
            GIT_AUTHOR_NAME="Test",
            GIT_AUTHOR_EMAIL="test@example.org",
            GIT_COMMITTER_NAME="Test",
            GIT_COMMITTER_EMAIL="test@example.org",
        )
        self.git("init", "-q")
        self.write(ADR, BASE)
        self.write("docs/adr/0000-template.md", BASE)
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()

    def git(self, *args: str) -> str:
        return str(
            run(
                [GIT, "-c", "commit.gpgsign=false", *args],
                allowed_executables=(GIT,),
                cwd=self.repo,
                env=self.env,
                text=True,
                capture_output=True,
                check=True,
                timeout_seconds=30,
            ).stdout
        )

    def write(self, name: str, content: str) -> None:
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def commit(self, message: str = "fixture") -> None:
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)

    def check_change(self, content: str, expected: int, section: str = "") -> None:
        self.write(ADR, content)
        self.commit()
        result = run(
            [sys.executable, str(SCRIPT), "--base", self.base, "--head", "HEAD"],
            allowed_executables=(sys.executable,),
            cwd=self.repo,
            env=self.env,
            text=True,
            capture_output=True,
            check=False,
            timeout_seconds=30,
        )
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        if section:
            self.assertRegex(result.stdout, r"docs/adr/1234-example\.md:\d+: " + section)

    def test_status_line(self) -> None:
        self.check_change(BASE.replace("Accepted", "Superseded"), 0)

    def test_dated_updates(self) -> None:
        for heading in ("##", "###"):
            with self.subTest(heading=heading):
                self.check_change(BASE + f"\n{heading} Status update 2026-11-15: x\nUpdate.\n", 0)

    def test_supersession_note(self) -> None:
        self.check_change(
            BASE.replace(
                "Accepted\n", "Accepted\n\n> [!NOTE]\n> **Superseded by [ADR-9999](x.md)**\n"
            ),
            0,
        )

    def test_note_after_status_metadata(self) -> None:
        self.write(ADR, BASE.replace("Accepted\n", "Accepted\n- **Date**: 2026-01-01\n"))
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        self.check_change(
            BASE.replace(
                "Accepted\n",
                "Superseded\n- **Date**: 2026-01-01\n\n> [!NOTE]\n> **Superseded by [ADR-9999](x.md)**\n",
            ),
            0,
        )

    def test_references(self) -> None:
        self.check_change(BASE.replace("[Source](old.md)", "New references."), 0)

    def test_link_target(self) -> None:
        self.check_change(BASE.replace("Old context.", "See [source](old.md)."), 1)

    def test_link_repair(self) -> None:
        self.check_change(
            BASE.replace("# ADR-1234: Example", "# ADR-1234: Example [link](old.md)"), 1
        )

    def test_link_repair_allowed(self) -> None:
        self.write(ADR, BASE.replace("Old context.", "See [source](old.md)."))
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        self.check_change(BASE.replace("Old context.", "See [source](new.md)."), 0)

    def test_proposed(self) -> None:
        self.write(ADR, BASE.replace("Accepted", "Proposed"))
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        self.check_change(BASE.replace("Accepted", "Proposed").replace("Old context.", "New."), 0)

    def test_changed_consequences(self) -> None:
        self.check_change(
            BASE.replace("Old consequence.", "New consequence."), 1, "## Consequences"
        )

    def test_removed_decision(self) -> None:
        self.check_change(BASE.replace("Old decision.\n", ""), 1, "## Decision")

    def test_added_context_line(self) -> None:
        self.check_change(
            BASE.replace("Old context.\n", "Old context.\nNew line.\n"), 1, "## Context"
        )

    def test_insert_blank_and_consequence(self) -> None:
        self.write(
            ADR, BASE.replace("Old consequence.\n", "Old consequence.\n\nNew consequence.\n")
        )
        self.commit()
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("frozen", result.stdout)

    def test_undated_heading(self) -> None:
        self.check_change(BASE + "\n## Status update\nNew line.\n", 1)

    def test_new_file_and_template(self) -> None:
        self.write("docs/adr/9999-new.md", BASE)
        self.write("docs/adr/0000-template.md", "changed\n")
        self.commit()
        self.assertEqual(self.run_check().returncode, 0)

    def run_check(self, base: str | None = None) -> TextCommandResult:
        return cast(
            "TextCommandResult",
            run(
                [sys.executable, str(SCRIPT), "--base", base or self.base, "--head", "HEAD"],
                allowed_executables=(sys.executable,),
                cwd=self.repo,
                env=self.env,
                text=True,
                capture_output=True,
                check=False,
                timeout_seconds=30,
            ),
        )

    def test_directive_changed(self) -> None:
        self.write(ADR, "<!-- markdownlint-disable MD060 -->\n" + BASE)
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        self.write(ADR, "<!-- markdownlint-disable MD060 MD013 -->\n" + BASE)
        self.commit()
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("frozen", result.stdout)

    def test_directive_before_title(self) -> None:
        self.write(ADR, "<!-- markdownlint-disable-next-line MD013 -->\n" + BASE)
        self.commit()
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("frozen", result.stdout)

    def reflow(self, changed: bool) -> TextCommandResult:
        self.write(ADR, BASE.replace("Old context.", "One two three\nfour five six."))
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        end = "seven" if changed else "six"
        self.write(ADR, BASE.replace("Old context.", f"One two\nthree four\nfive {end}."))
        self.commit()
        return self.run_check()

    def test_reflow_same_words(self) -> None:
        result = self.reflow(False)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn("frozen", result.stdout)

    def test_reflow_changed_word(self) -> None:
        result = self.reflow(True)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("## Context", result.stdout)

    def behind_master(self, edit: bool) -> TextCommandResult:
        self.git("checkout", "-q", "-b", "feature")
        self.write("other.txt", "branch change\n")
        if edit:
            self.write(ADR, BASE.replace("Old consequence.", "Changed consequence."))
        self.commit()
        self.git("checkout", "-q", "-b", "base", self.base)
        self.write(
            ADR,
            BASE.replace(
                "## References", "### Status update 2026-11-20\nMaster update.\n\n## References"
            ),
        )
        self.commit()
        base_tip = self.git("rev-parse", "HEAD").strip()
        self.git("checkout", "-q", "feature")
        return self.run_check(base_tip)

    def test_behind_master(self) -> None:
        result = self.behind_master(False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("frozen", result.stdout)

    def test_behind_master_consequences(self) -> None:
        result = self.behind_master(True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("## Consequences", result.stdout)
        self.assertNotIn("Status update", result.stdout.split(": body of")[0])

    def test_blank_after_directive(self) -> None:
        directive = "<!-- markdownlint-disable MD013 -->\n"
        self.write(ADR, directive + BASE)
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        self.write(ADR, directive + "\n" + BASE)
        self.commit()
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("frozen", result.stdout)

    def table_change(self, row: str) -> TextCommandResult:
        table = "| Name | Value |\n|---|---|\n| one two | value |"
        self.write(ADR, BASE.replace("Old context.", table))
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        self.write(ADR, BASE.replace("Old context.", row))
        self.commit()
        return self.run_check()

    def test_table_spacing(self) -> None:
        result = self.table_change("| Name | Value |\n| --- | --- |\n|one two|value|")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("frozen", result.stdout)

    def test_table_changed_word(self) -> None:
        result = self.table_change("| Name | Value |\n| --- | --- |\n|one too|value|")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("## Context", result.stdout)

    def test_override(self) -> None:
        self.write(ADR, BASE.replace("Old consequence.", "New consequence."))
        self.commit("fixture\n\nADR-Body-Edit: redact a development host name from the record")
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("note: docs/adr/1234-example.md:", result.stdout)
        self.assertIn("1 hunk(s) accepted under ADR-Body-Edit in", result.stdout)
        self.assertIn("redact a development host name from the record", result.stdout)

    def test_override_short_reason(self) -> None:
        self.write(ADR, BASE.replace("Old consequence.", "New consequence."))
        self.commit("fixture\n\nADR-Body-Edit: fix")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("at least 20 characters", result.stderr)

    def test_override_outside_range(self) -> None:
        self.commit("fixture\n\nADR-Body-Edit: redact a development host name from the record")
        self.base = self.git("rev-parse", "HEAD").strip()
        self.write(ADR, BASE.replace("Old consequence.", "New consequence."))
        self.commit()
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("## Consequences", result.stdout)
        self.assertNotIn("note:", result.stdout)

    def test_rename_unchanged(self) -> None:
        self.git("mv", ADR, "docs/adr/1234-renamed.md")
        self.commit()
        self.assertEqual(self.run_check().returncode, 0)

    def test_rename_changed(self) -> None:
        self.git("mv", ADR, "docs/adr/1234-renamed.md")
        self.write("docs/adr/1234-renamed.md", BASE.replace("Old context.", "New context."))
        self.commit()
        self.assertEqual(self.run_check().returncode, 1)

    def test_hook_always_runs(self) -> None:
        config = yaml.safe_load((SCRIPT.parents[2] / ".pre-commit-config.yaml").read_text())
        hook = next(
            h
            for repo in config["repos"]
            for h in repo.get("hooks", [])
            if h["id"] == "check-adr-body-freeze"
        )
        self.assertIs(hook.get("always_run"), True)
        self.assertIs(hook.get("pass_filenames"), False)

    def test_fenced_references_heading(self) -> None:
        original = BASE.replace("Old context.", "```markdown\n## References\n```\nOld context.")
        self.write(ADR, original)
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        self.check_change(original.replace("Old context.", "Changed context."), 1)

    def test_update_inside_context_fence(self) -> None:
        original = BASE.replace("Old context.", "```text\nexample\n```\nOld context.")
        self.write(ADR, original)
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        update = "## Status update 2026-11-20\nUpdate.\n"
        self.check_change(
            original.replace("example\n```", "example\n" + update + "```"), 1, "## Context"
        )
        self.check_change(original + "\n" + update, 0)

    def test_references_replacement_fenced_heading(self) -> None:
        self.check_change(
            BASE.replace("[Source](old.md)", "New reference.\n\n```markdown\n## Example\n```"), 0
        )

    def test_nested_dated_update_keeps_level(self) -> None:
        prefix = BASE + "\n## Status update 2026-11-20\n### Status update 2026-11-21\n"
        self.check_change(prefix + "### Details\nDetails.\n", 0)
        self.check_change(prefix + "x\n## Decision\nNew.\n", 1)

    def test_update_subheading_and_peer(self) -> None:
        self.check_change(BASE + "\n## Status update 2026-11-20\n### Details\nDetails.\n", 0)
        self.check_change(BASE + "\n## Status update 2026-11-20\nUpdate.\n## Decision\nNew.\n", 1)

    def test_reference_definition_destination(self) -> None:
        original = BASE.replace("Old context.", '[s]: old.md "Title"')
        self.write(ADR, original)
        self.commit()
        self.base = self.git("rev-parse", "HEAD").strip()
        self.check_change(original.replace('old.md "Title"', 'new.md "Title"'), 0)
        self.check_change(original.replace("[s]: old.md", "[t]: new.md"), 1)

    def test_status_with_note_and_context_change(self) -> None:
        changed = BASE.replace("Accepted\n", "Superseded\n> [!NOTE]\n> Superseded by ADR-9999\n")
        self.check_change(changed, 0)
        self.check_change(changed.replace("Old context.", "Changed context."), 1)

    def test_references_replacement_cannot_add_section(self) -> None:
        self.check_change(
            BASE.replace("[Source](old.md)", "New reference.\n\n## Decision\nNew decision."), 1
        )
        self.check_change(BASE.replace("[Source](old.md)", "New reference."), 0)

    def test_renamed_status_and_link_repairs(self) -> None:
        target = "docs/adr/1234-renamed.md"
        cases = (
            (BASE.replace("Accepted", "Proposed"), "Old context.", "New context.", 0),
            (BASE, "Old context.", "New context.", 1),
            (
                BASE.replace("Old context.", "See [source](old.md)."),
                "source](old.md)",
                "source](new.md)",
                0,
            ),
        )
        for original, before, after, expected in cases:
            with self.subTest(expected=expected, before=before):
                self.write(ADR, original)
                self.commit()
                self.base = self.git("rev-parse", "HEAD").strip()
                self.git("mv", ADR, target)
                self.write(target, original.replace(before, after))
                self.commit()
                result = self.run_check()
                self.git("mv", target, ADR)
                self.commit()
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)

    def test_no_changes(self) -> None:
        self.assertEqual(self.run_check().returncode, 0)

    def test_bad_revision(self) -> None:
        self.assertEqual(self.run_check("missing-revision").returncode, 2)


if __name__ == "__main__":
    unittest.main()
