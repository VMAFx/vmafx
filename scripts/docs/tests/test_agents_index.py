#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests for the ``AGENTS.d`` index generator and the migration check."""

from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.dev import project_modernization_audit as audit  # noqa: E402
from scripts.docs import agents_index  # noqa: E402
from scripts.docs import agents_migration_check as migration  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

DIRECTORY = "tools/gate"
HEAD = """\
<!-- markdownlint-disable MD013 -->
# `tools/gate/` notes

Parent: [../AGENTS.md](../../AGENTS.md).

Every script here is called by a workflow.
"""
ALPHA = """\
---
paths:
  - tools/gate/alpha.py
  - docs/guide/*.md
invariant: Alpha keeps `--flag`; cells with | stay escaped.
---
<!-- markdownlint-disable MD013 -->
# Alpha gate (ADR-0001)

`alpha.py` reads `--flag`. See [the guide](../../../docs/guide/alpha.md).
"""
BETA = """\
---
paths:
  - tools/gate/beta.sh
invariant: Beta exits 2 on bad usage.
---
# Beta gate

- `beta.sh` exits 2 on bad usage.
- Never `eval` its input.
"""
OLD = """\
<!-- markdownlint-disable MD013 -->
# `tools/gate/` notes

Parent: [../AGENTS.md](../AGENTS.md).

Every script here is called by a workflow.

## Alpha gate (ADR-0001)

`alpha.py` reads `--flag`. See [the guide](../../docs/guide/alpha.md).

## Beta gate

- `beta.sh` exits 2 on bad usage.
- Never `eval` its input.
"""


def quiet(function: Callable[[list[str]], int], arguments: list[str]) -> tuple[int, str, str]:
    """Call ``function`` and return (result, stdout, stderr)."""

    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        result = function(arguments)
    return result, out.getvalue(), err.getvalue()


class Fixture(unittest.TestCase):
    """A scratch repository root with one ``AGENTS.d`` directory."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="vmafx-agents-index-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.pages = self.root / DIRECTORY / agents_index.PAGES_DIR
        self.pages.mkdir(parents=True)
        for name in ("tools/gate/alpha.py", "tools/gate/beta.sh", "docs/guide/alpha.md"):
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            (self.root / name).write_text("fixture\n", encoding="utf-8")
        self.write("_index.md", HEAD)
        self.write("alpha.md", ALPHA)
        self.write("beta.md", BETA)

    def write(self, name: str, text: str) -> Path:
        path = self.pages / name
        path.write_text(text, encoding="utf-8")
        return path

    def index(self) -> Path:
        return self.root / DIRECTORY / agents_index.INDEX_NAME

    def run_index(self, mode: str) -> tuple[int, str]:
        status, _, err = quiet(agents_index.main, [mode, "--root", str(self.root), DIRECTORY])
        return status, err

    def assert_invalid(self, fragment: str) -> None:
        status, err = self.run_index("--check")
        self.assertEqual(status, agents_index.EXIT_INVALID, err)
        self.assertIn(fragment, err)


class RenderTests(Fixture):
    def test_renders_title_instruction_rules_and_sorted_rows(self) -> None:
        text = agents_index.render(DIRECTORY, self.root)
        self.assertTrue(text.startswith(agents_index.BANNER + "<!-- markdownlint-disable"))
        title = text.index("# `tools/gate/` notes")
        instruction = text.index("Generated index. Before editing")
        rules = text.index("Every script here is called by a workflow.")
        table = text.index("## Topic pages")
        self.assertLess(title, instruction)
        self.assertLess(instruction, rules)
        self.assertLess(rules, table)
        self.assertLess(text.index("[alpha](AGENTS.d/alpha.md)"), text.index("[beta]("))
        self.assertIn("([how](../../docs/development/agents-index.md))", text)

    def test_row_spells_paths_relative_to_the_directory_and_escapes_pipes(self) -> None:
        text = agents_index.render(DIRECTORY, self.root)
        self.assertIn(
            "| `alpha.py`, `/docs/guide/*.md` | [alpha](AGENTS.d/alpha.md) | "
            "Alpha keeps `--flag`; cells with \\| stay escaped. |\n",
            text,
        )

    def test_head_links_are_rebased_to_the_index_directory(self) -> None:
        text = agents_index.render(DIRECTORY, self.root)
        self.assertIn("Parent: [../AGENTS.md](../AGENTS.md).", text)

    def test_write_then_check_is_clean_and_a_missing_index_fails(self) -> None:
        status, err = self.run_index("--check")
        self.assertEqual(status, agents_index.EXIT_STALE)
        self.assertIn(
            "tools/gate/AGENTS.md is missing; it is rendered from tools/gate/AGENTS.d/", err
        )
        self.assertEqual(self.run_index("--write")[0], 0)
        self.assertEqual(self.run_index("--check")[0], 0)

    def test_a_paragraph_appended_to_the_index_fails_and_names_the_page_directory(self) -> None:
        self.assertEqual(self.run_index("--write")[0], 0)
        appended = "- **New invariant**: `gamma.py` must keep `--strict`.\n"
        self.index().write_text(self.index().read_text() + "\n" + appended, encoding="utf-8")
        status, err = self.run_index("--check")
        self.assertEqual(status, agents_index.EXIT_STALE)
        self.assertIn("holds 1 line(s) its sources do not produce", err)
        self.assertIn(appended.strip(), err)
        self.assertIn("Never edit or append to tools/gate/AGENTS.md", err)
        self.assertIn("a page under tools/gate/AGENTS.d/", err)
        self.assertIn("tools/gate/AGENTS.d/_index.md only when it binds every file", err)

    def test_a_hand_edited_row_fails_the_same_way(self) -> None:
        self.assertEqual(self.run_index("--write")[0], 0)
        text = self.index().read_text().replace("Beta exits 2 on bad usage.", "Beta exits 3.")
        self.index().write_text(text, encoding="utf-8")
        status, err = self.run_index("--check")
        self.assertEqual(status, agents_index.EXIT_STALE)
        self.assertIn("holds 1 line(s) its sources do not produce", err)
        self.assertIn("| Beta exits 3. |", err)

    def test_an_index_that_lost_a_generated_line_is_stale_not_hand_edited(self) -> None:
        self.assertEqual(self.run_index("--write")[0], 0)
        kept = [line for line in self.index().read_text().splitlines() if "[beta](" not in line]
        self.index().write_text("\n".join(kept) + "\n", encoding="utf-8")
        status, err = self.run_index("--check")
        self.assertEqual(status, agents_index.EXIT_STALE)
        self.assertIn("tools/gate/AGENTS.md is stale against tools/gate/AGENTS.d/", err)
        self.assertNotIn("do not produce", err)

    def test_hand_edit_report_is_bounded(self) -> None:
        self.assertEqual(self.run_index("--write")[0], 0)
        extra = "".join(f"appended line {n}\n" for n in range(agents_index.MAX_REPORTED_LINES + 1))
        self.index().write_text(self.index().read_text() + extra, encoding="utf-8")
        _, err = self.run_index("--check")
        self.assertIn("holds 6 line(s)", err)
        self.assertIn("appended line 4", err)
        self.assertNotIn("appended line 5", err)
        self.assertIn("... 1 more", err)

    def test_editing_a_page_body_leaves_the_index_unchanged(self) -> None:
        before = agents_index.render(DIRECTORY, self.root)
        self.write("beta.md", BETA + "\nA new invariant paragraph.\n")
        self.assertEqual(agents_index.render(DIRECTORY, self.root), before)

    def test_root_level_directory_keeps_plain_paths(self) -> None:
        self.assertEqual(agents_index.display_path("docs/a.md", ""), "docs/a.md")
        self.assertEqual(agents_index.display_path("tools/gate/x.py", "tools/gate"), "x.py")
        self.assertEqual(
            agents_index.display_path("tools/gates/x.py", "tools/gate"), "/tools/gates/x.py"
        )


class LinkTests(unittest.TestCase):
    def test_relative_targets_move_and_everything_else_stays(self) -> None:
        text = (
            "[a](../x.md#frag) [b](https://example.org/y) [c](/abs.md) [d](#here) [e](dir/)\n"
            "`[code](../x.md)` and ``[more](../x.md)``\n"
            "\n"
            "```text\n"
            "[fenced](../x.md)\n"
            "```\n"
            "[after](../x.md)\n"
        )
        moved = agents_index.rebase_links(text, "a/b", "a/b/AGENTS.d")
        self.assertIn("[a](../../x.md#frag)", moved)
        self.assertIn("[b](https://example.org/y) [c](/abs.md) [d](#here) [e](../dir/)", moved)
        self.assertIn("`[code](../x.md)` and ``[more](../x.md)``", moved)
        self.assertIn("```text\n[fenced](../x.md)\n```\n", moved)
        self.assertIn("[after](../../x.md)", moved)

    def test_rebasing_there_and_back_is_the_identity(self) -> None:
        text = "See [x](../../docs/a.md) and [y](sibling.md).\n"
        there = agents_index.rebase_links(text, "a/b", "a/b/AGENTS.d")
        self.assertNotEqual(there, text)
        self.assertEqual(agents_index.rebase_links(there, "a/b/AGENTS.d", "a/b"), text)


class SourceContractTests(Fixture):
    def test_rejects_a_page_without_front_matter(self) -> None:
        self.write("beta.md", "# Beta gate\n")
        self.assert_invalid("must open a `---` front-matter block")

    def test_rejects_unclosed_front_matter(self) -> None:
        self.write("beta.md", "---\npaths:\n  - tools/gate/beta.sh\ninvariant: x\n# Beta\n")
        self.assert_invalid("never closed")

    def test_rejects_unknown_duplicate_and_missing_keys(self) -> None:
        cases = {
            "owner: someone\n": "unexpected front-matter line",
            "invariant: again\n": "unexpected front-matter line",
            "": "needs `paths:` (a list) and `invariant:`",
        }
        for extra, message in cases.items():
            with self.subTest(extra=extra):
                body = "---\npaths:\n  - tools/gate/beta.sh\n"
                body += "invariant: x\n" + extra if extra else ""
                self.write("beta.md", body + "---\n# Beta\n")
                self.assert_invalid(message)

    def test_rejects_inline_paths_value_and_empty_path_list(self) -> None:
        self.write("beta.md", "---\npaths: tools/gate/beta.sh\ninvariant: x\n---\n# Beta\n")
        self.assert_invalid("needs `paths:` (a list)")
        self.write("beta.md", "---\npaths:\ninvariant: x\n---\n# Beta\n")
        self.assert_invalid("`paths:` needs 1 to")

    def test_rejects_paths_outside_the_contract(self) -> None:
        cases = {
            "../outside.py": "not a repository-relative glob",
            "/abs/path.py": "not a repository-relative glob",
            "tools/gate/has space.py": "not a repository-relative glob",
            "tools/gate/missing-*.py": "matches nothing in the tree",
        }
        for pattern, message in cases.items():
            with self.subTest(pattern=pattern):
                self.write("beta.md", BETA.replace("tools/gate/beta.sh", pattern))
                self.assert_invalid(message)

    def test_rejects_a_path_listed_twice(self) -> None:
        self.write("beta.md", BETA.replace("invariant:", "  - tools/gate/beta.sh\ninvariant:"))
        self.assert_invalid("lists an entry twice")

    def test_rejects_bad_slug_missing_title_and_empty_invariant(self) -> None:
        self.write("Beta_Gate.md", BETA)
        self.assert_invalid("lower-case words joined by `-`")
        (self.pages / "Beta_Gate.md").unlink()
        self.write("beta.md", BETA.replace("# Beta gate", "## Beta gate"))
        self.assert_invalid("must start with a `# ` title")
        self.write("beta.md", BETA.replace("Beta exits 2 on bad usage.\n---", "\n---"))
        self.assert_invalid("`invariant:` is empty")

    def test_rejects_stray_entries_in_the_pages_directory(self) -> None:
        stray = self.pages / "notes.txt"
        stray.write_text("x\n", encoding="utf-8")
        self.assert_invalid("notes.txt is not a Markdown page")
        stray.unlink()
        (self.pages / "nested").mkdir()
        self.assert_invalid("nested is not a Markdown page")

    def test_rejects_missing_head_headless_head_and_no_pages(self) -> None:
        self.write("_index.md", "Rules first, no title.\n")
        self.assert_invalid("must start with a `# ` title")
        (self.pages / "_index.md").unlink()
        self.assert_invalid("_index.md: missing")
        self.write("_index.md", HEAD)
        (self.pages / "alpha.md").unlink()
        (self.pages / "beta.md").unlink()
        self.assert_invalid("no topic page")


class AreaTests(Fixture):
    """A directory that groups its pages into areas gets one sub-index per area."""

    def with_areas(self) -> None:
        self.write("_area-first.md", "# First\n\nAlpha things.\n")
        self.write("_area-second.md", "# Second\n\nBeta things.\n")
        self.write("alpha.md", ALPHA.replace("---\n<!--", "area: first\n---\n<!--", 1))
        self.write(
            "beta.md",
            BETA.replace(
                "invariant: Beta exits 2 on bad usage.",
                "invariant: Beta exits 2 on bad usage.\narea: second",
            ),
        )

    def test_index_lists_areas_and_each_area_has_its_own_table(self) -> None:
        self.with_areas()
        index = agents_index.render(DIRECTORY, self.root)
        self.assertIn("## Areas", index)
        self.assertIn("[first](AGENTS-first.md)", index)
        self.assertNotIn("## Topic pages", index)
        self.assertNotIn("[alpha](", index)
        areas = agents_index.render_areas(DIRECTORY, self.root)
        self.assertEqual(sorted(areas), ["AGENTS-first.md", "AGENTS-second.md"])
        self.assertIn("[alpha](AGENTS.d/alpha.md)", areas["AGENTS-first.md"])
        self.assertNotIn("[beta](", areas["AGENTS-first.md"])

    def test_write_then_check_passes_and_a_stale_sub_index_fails(self) -> None:
        self.with_areas()
        self.assertEqual(self.run_index("--write")[0], 0)
        self.assertEqual(self.run_index("--check")[0], 0)
        sub = self.root / DIRECTORY / "AGENTS-first.md"
        sub.write_text(sub.read_text(encoding="utf-8") + "hand edit\n", encoding="utf-8")
        status, err = self.run_index("--check")
        self.assertEqual(status, agents_index.EXIT_STALE, err)
        self.assertIn("AGENTS-first.md", err)

    def test_each_sub_index_has_the_index_budget(self) -> None:
        self.with_areas()
        size = len(agents_index.render_areas(DIRECTORY, self.root)["AGENTS-first.md"].encode())
        with mock.patch.object(agents_index, "INDEX_MAX_BYTES", size - 1):
            with self.assertRaisesRegex(agents_index.AgentsIndexError, "AGENTS-first.md"):
                agents_index.render_areas(DIRECTORY, self.root)

    def test_a_page_in_an_unknown_area_is_refused(self) -> None:
        self.with_areas()
        self.write(
            "beta.md",
            BETA.replace("invariant: Beta exits 2 on bad usage.", "invariant: x\narea: nowhere"),
        )
        self.assert_invalid("`area:` 'nowhere'")

    def test_a_page_without_an_area_is_refused_once_areas_exist(self) -> None:
        self.with_areas()
        self.write("beta.md", BETA)
        self.assert_invalid("does not match")

    def test_an_area_without_pages_is_refused(self) -> None:
        self.with_areas()
        self.write("_area-empty.md", "# Empty\n\nNothing.\n")
        self.assert_invalid("area empty has no page")

    def test_an_area_key_without_area_files_is_refused(self) -> None:
        self.write(
            "beta.md",
            BETA.replace("invariant: Beta exits 2 on bad usage.", "invariant: x\narea: first"),
        )
        self.assert_invalid("does not match")


class BudgetTests(Fixture):
    def page_with_invariant(self, length: int) -> str:
        return BETA.replace("Beta exits 2 on bad usage.\n---", "x" * length + "\n---")

    def test_invariant_length_boundary(self) -> None:
        self.write("beta.md", self.page_with_invariant(agents_index.INVARIANT_MAX_CHARS))
        self.assertEqual(self.run_index("--write")[0], 0)
        self.write("beta.md", self.page_with_invariant(agents_index.INVARIANT_MAX_CHARS + 1))
        self.assert_invalid("limit 120")

    def test_page_size_boundary(self) -> None:
        padding = agents_index.PAGE_MAX_BYTES - len(BETA.encode("utf-8"))
        self.write("beta.md", BETA + "x" * (padding - 1) + "\n")
        self.assertEqual(self.run_index("--write")[0], 0)
        self.write("beta.md", BETA + "x" * padding + "\n")
        self.assert_invalid("page budget 12000")

    def test_index_size_boundary(self) -> None:
        size = len(agents_index.render(DIRECTORY, self.root).encode("utf-8"))
        with mock.patch.object(agents_index, "INDEX_MAX_BYTES", size):
            self.assertEqual(self.run_index("--write")[0], 0)
        with mock.patch.object(agents_index, "INDEX_MAX_BYTES", size - 1):
            self.assert_invalid(f"{size} bytes, index budget {size - 1}")

    def test_path_count_boundary(self) -> None:
        for count in range(agents_index.MAX_PATHS_PER_PAGE + 1):
            (self.root / DIRECTORY / f"f{count}.py").write_text("x\n", encoding="utf-8")

        def page(count: int) -> str:
            listing = "".join(f"  - tools/gate/f{n}.py\n" for n in range(count))
            return f"---\npaths:\n{listing}invariant: x\n---\n# Beta\n"

        self.write("beta.md", page(agents_index.MAX_PATHS_PER_PAGE))
        self.assertEqual(self.run_index("--write")[0], 0)
        self.write("beta.md", page(agents_index.MAX_PATHS_PER_PAGE + 1))
        self.assert_invalid("`paths:` needs 1 to 24 entries")


class DiscoveryTests(Fixture):
    def test_directories_from_names_only_owners_of_a_pages_directory(self) -> None:
        names = ["a/AGENTS.d/x.md", "a/AGENTS.d/_index.md", "AGENTS.d/y.md", "b/AGENTS.md", "c/d"]
        self.assertEqual(agents_index.directories_from(names), ["", "a"])

    def git(self, *arguments: str) -> None:
        environment = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        run_command(
            ("git", "-C", str(self.root), *arguments),
            allowed_executables=("git",),
            env=environment,
            capture_output=True,
            text=True,
            check=True,
        )

    def test_discover_finds_untracked_pages_and_skips_ignored_ones(self) -> None:
        home = {"HOME": str(self.root), "XDG_CONFIG_HOME": str(self.root / ".config")}
        ignored = self.root / "build/AGENTS.d"
        ignored.mkdir(parents=True)
        (ignored / "_index.md").write_text("# ignored\n", encoding="utf-8")
        (self.root / ".gitignore").write_text("build/\n", encoding="utf-8")
        with mock.patch.dict(os.environ, home):
            self.git("init", "--quiet")
            self.assertEqual(agents_index.discover(self.root), [DIRECTORY])
            status, _, err = quiet(agents_index.main, ["--write", "--root", str(self.root)])
            self.assertEqual(status, 0, err)
            self.assertTrue(self.index().is_file())


class UnitSplitTests(unittest.TestCase):
    def kinds(self, text: str) -> list[tuple[str, int, str]]:
        return [(u.kind, u.line, u.text) for u in migration.split_units(text, "t.md")]

    def test_paragraphs_headings_comments_and_fences(self) -> None:
        text = "<!-- c -->\n# T\n\npara one\nwraps\n\n```text\na\n\nb\n```\n\nlast\n"
        self.assertEqual(
            self.kinds(text),
            [
                (migration.COMMENT, 1, "<!-- c -->"),
                (migration.HEADING, 2, "# T"),
                (migration.TEXT, 4, "para one\nwraps"),
                (migration.TEXT, 7, "```text\na\n\nb\n```"),
                (migration.TEXT, 13, "last"),
            ],
        )

    def test_top_level_list_items_are_separate_and_keep_indented_blocks(self) -> None:
        text = "- one\n  more\n- two\n\n  still two\n\n- three\nlazy tail\n\nnot a list\n"
        self.assertEqual(
            self.kinds(text),
            [
                (migration.ITEM, 1, "- one\n  more"),
                (migration.ITEM, 3, "- two\n\n  still two"),
                (migration.ITEM, 7, "- three\nlazy tail"),
                (migration.TEXT, 10, "not a list"),
            ],
        )

    def test_table_header_rows_are_structure_and_data_rows_are_content(self) -> None:
        text = "| A | B |\n| --- | :-: |\n| 1 | 2 |\n| 3 | 4 |\n\n| 5 | 6 |\n"
        units = migration.split_units(text, "t.md")
        self.assertEqual(
            [unit.kind for unit in units],
            [migration.TABLE_HEAD] * 2 + [migration.TABLE_ROW] * 3,
        )
        self.assertEqual([unit.line for unit in migration.headerless_rows(units)], [6])


class MigrationTests(Fixture):
    def setUp(self) -> None:
        super().setUp()
        self.assertEqual(self.run_index("--write")[0], 0)

    def report(self, old: str = OLD) -> migration.Report:
        return migration.check(old, DIRECTORY, self.root)

    def findings(self, old: str = OLD) -> str:
        return "\n".join(self.report(old).lines)

    def test_a_verbatim_split_loses_nothing(self) -> None:
        report = self.report()
        self.assertEqual(report.failures, 0, "\n".join(report.lines))
        self.assertIn("  content units: 5 old, 5 new", report.lines)

    def test_cli_reads_the_old_file_and_reports_ok(self) -> None:
        old = self.root / "old.md"
        old.write_text(OLD, encoding="utf-8")
        arguments = ["--old-file", str(old), "--root", str(self.root), DIRECTORY]
        status, out, _ = quiet(migration.main, arguments)
        self.assertEqual(status, 0, out)
        self.assertIn("OK: nothing lost", out)
        self.write("beta.md", BETA.replace("- Never `eval` its input.\n", ""))
        status, out, _ = quiet(migration.main, arguments)
        self.assertEqual(status, 1)
        self.assertIn("FAIL: ", out)

    def test_a_dropped_unit_is_reported(self) -> None:
        self.write("beta.md", BETA.replace("- Never `eval` its input.\n", ""))
        text = self.findings()
        self.assertIn("units dropped: 1", text)
        self.assertIn("(old):15: - Never `eval` its input.", text)

    def test_a_unit_in_two_pages_is_reported_as_duplicated(self) -> None:
        self.write("alpha.md", ALPHA + "\n- Never `eval` its input.\n")
        text = self.findings()
        self.assertIn("units duplicated: 2", text)
        self.assertIn("units dropped: 0", text)

    def test_reworded_text_is_a_drop_plus_an_addition(self) -> None:
        self.write("beta.md", BETA.replace("Never `eval` its input", "Do not `eval` its input"))
        text = self.findings()
        self.assertIn("units dropped: 1", text)
        self.assertIn("units added: 1", text)
        self.assertIn("tools/gate/AGENTS.d/beta.md:9: - Do not `eval` its input.", text)

    def test_a_link_that_was_not_moved_one_level_deeper_fails(self) -> None:
        self.write("alpha.md", ALPHA.replace("../../../docs/guide", "../../docs/guide"))
        text = self.findings()
        self.assertIn("units dropped: 1", text)
        self.assertIn("tokens missing: 1", text)
        self.assertIn("](/docs/guide/alpha.md)", text)

    def test_a_lost_heading_is_reported_with_its_token(self) -> None:
        self.write("alpha.md", ALPHA.replace("# Alpha gate (ADR-0001)", "# Alpha gate"))
        text = self.findings()
        self.assertIn("headings missing: 1", text)
        self.assertIn("tokens missing: 1\n    ADR-0001", text)

    def test_heading_levels_and_repeated_structure_are_free(self) -> None:
        old = OLD.replace("## Beta gate", "### Beta gate")
        self.write("alpha.md", ALPHA + "\n## Beta gate\n")
        self.assertEqual(self.report(old).failures, 0)

    def test_table_rows_need_a_header_in_their_page(self) -> None:
        old = OLD + "\n| Script | Lane |\n| --- | --- |\n| `a` | one |\n| `b` | two |\n"
        self.write("alpha.md", ALPHA + "\n| Script | Lane |\n| --- | --- |\n| `a` | one |\n")
        self.write("beta.md", BETA + "\n| `b` | two |\n")
        self.assertIn("table rows without a header: 1", self.findings(old))
        self.write("beta.md", BETA + "\n| Script | Lane |\n| --- | --- |\n| `b` | two |\n")
        self.assertEqual(self.report(old).failures, 0)

    def test_a_stale_index_and_an_old_anchor_link_fail(self) -> None:
        self.index().write_text("stale\n", encoding="utf-8")
        self.assertIn("stale index: 1", self.findings())
        self.assertEqual(self.run_index("--write")[0], 0)
        old = OLD.replace("Never `eval` its input.", "Never `eval` its input, see [above](#beta).")
        self.assertIn("same-file anchor links in the old file (unsupported): 1", self.findings(old))

    def test_trailing_whitespace_and_blank_runs_do_not_count(self) -> None:
        old = OLD.replace(
            "Every script here is called by a workflow.\n",
            "Every script here is called by a workflow.  \n\n\n",
        )
        self.assertEqual(self.report(old).failures, 0)

    def test_invalid_sources_exit_65(self) -> None:
        self.write("beta.md", "# no front matter\n")
        old = self.root / "old.md"
        old.write_text(OLD, encoding="utf-8")
        arguments = ["--old-file", str(old), "--root", str(self.root), DIRECTORY]
        status, _, err = quiet(migration.main, arguments)
        self.assertEqual(status, agents_index.EXIT_INVALID)
        self.assertIn("front-matter", err)

    def test_old_text_can_come_from_a_git_revision(self) -> None:
        home = {"HOME": str(self.root), "XDG_CONFIG_HOME": str(self.root / ".config")}
        environment = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        environment.update(home)
        identity = ("-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid")
        safety = ("-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null")
        self.index().write_text(OLD, encoding="utf-8")
        for arguments in (
            ("init", "--quiet"),
            ("add", "--", f"{DIRECTORY}/AGENTS.md"),
            (*identity, *safety, "commit", "--quiet", "-m", "old"),
        ):
            run_command(
                ("git", "-C", str(self.root), *arguments),
                allowed_executables=("git",),
                env=environment,
                capture_output=True,
                text=True,
                check=True,
            )
        self.assertEqual(self.run_index("--write")[0], 0)
        with mock.patch.dict(os.environ, home):
            status, out, _ = quiet(
                migration.main, ["--old-ref", "HEAD", "--root", str(self.root), DIRECTORY]
            )
        self.assertEqual(status, 0, out)
        self.assertIn("old AGENTS.md: ", out)


class ModernizationAuditScopeTests(unittest.TestCase):
    def test_topic_pages_are_skipped_like_the_index_they_came_from(self) -> None:
        skipped = (
            "scripts/ci/AGENTS.md",
            "scripts/ci/AGENTS.d/tidy-ratchet.md",
            "AGENTS.d/_index.md",
        )
        for name in skipped:
            with self.subTest(name=name):
                self.assertTrue(audit._skip_path(Path(name), include_archives=False))
        self.assertFalse(audit._skip_path(Path("docs/AGENTS.d.md"), include_archives=False))
        self.assertFalse(audit._skip_path(Path("scripts/ci/notes.md"), include_archives=False))


if __name__ == "__main__":
    unittest.main()
