#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check source-driven ADR generation without modifying the caller's repo."""

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import CommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
ADR_NAV = [
    "- Overview: adr/README.md",
    "- Template: adr/0000-template.md",
    "- By tag: adr/by-tag/index.md",
]


def git_init(root: Path) -> None:
    """Make the fixture a repository: the fragment order is read from its history."""
    git = shutil.which("git")
    assert git is not None, "git is required by the generator fixture"
    run_command(
        (git, "init", "-q", str(root)),
        allowed_executables=(git,),
        # Without the hook's GIT_DIR: the fixture, not the caller's repository.
        env={key: value for key, value in os.environ.items() if not key.startswith("GIT_")},
        capture_output=True,
        check=True,
        timeout_seconds=60,
    )


def adr_nav_entries(mkdocs_text: str) -> list[str]:
    """The entries under the top-level `ADRs` navigation item, comments dropped."""
    lines = mkdocs_text.splitlines()
    start = lines.index("  - ADRs:") + 1
    entries = []
    for line in lines[start:]:
        if line.startswith("  - ") or not line.startswith("    "):
            break
        if not line.strip().startswith("#"):
            entries.append(line.strip())
    return entries


class GeneratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-adr-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "scripts/docs").mkdir(parents=True)
        for script in (
            "generate-adr-by-tag.sh",
            "concat-adr-index.sh",
            "check-adr-index.py",
            "fragment-order.py",
        ):
            shutil.copyfile(ROOT / "scripts/docs" / script, self.root / "scripts/docs" / script)
        shutil.copytree(ROOT / "scripts/lib", self.root / "scripts/lib")
        (self.root / "scripts/__init__.py").write_text("")
        git_init(self.root)
        self.adr = self.root / "docs/adr"
        self.adr.mkdir(parents=True)
        (self.adr / "0000-template.md").write_text("# ADR-0000: Template\nTags: ignore\n")
        (self.adr / "0001-example.md").write_text(
            "# ADR-0001: _MSC_VER and <NAME> | `^## `\n"
            "- **Tags**: `CI`, ci, c++, <placeholder>, two words\n"
        )
        (self.adr / "0100-second.md").write_text("# ADR-0100: Second\nTags: docs\n")

    def run_generator(self, script: str, mode: str, *, check: bool = True) -> CommandResult:
        executable = shutil.which("bash")
        assert executable is not None, "bash is required by the generator fixture"
        # ADR-1242: fixed repository scripts in a disposable fixture, no shell.
        result = run_command(
            (executable, str(self.root / "scripts/docs" / script), mode),
            allowed_executables=(executable,),
            text=True,
            capture_output=True,
            check=False,
            timeout_seconds=60,
        )
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def snapshot(self) -> dict[str, bytes]:
        return {
            str(path.relative_to(self.root)): path.read_bytes()
            for path in self.root.rglob("*")
            if path.is_file()
        }

    def refresh(self) -> None:
        self.run_generator("generate-adr-by-tag.sh", "--write")

    def test_roundtrip_title_rendering_dedup_and_readonly_checks(self) -> None:
        self.refresh()
        baseline = self.snapshot()
        self.refresh()
        self.assertEqual(self.snapshot(), baseline)
        self.run_generator("generate-adr-by-tag.sh", "--check")
        self.assertEqual(self.snapshot(), baseline)
        tags = self.adr / "by-tag"
        self.assertEqual(
            {p.name for p in tags.glob("*.md")}, {"ci.md", "c++.md", "docs.md", "index.md"}
        )
        ci = (tags / "ci.md").read_text()
        self.assertIn("1 ADR(s)", ci)
        self.assertIn(r"\_MSC\_VER and &lt;NAME&gt; \| `^## `", ci)
        self.assertIn("markdownlint-disable MD013 MD038 MD060", ci)
        self.assertIn("c++.md", (tags / "index.md").read_text())

    def test_missing_stale_changed_outputs_fail_without_writing(self) -> None:
        self.refresh()
        tags = self.adr / "by-tag"
        (tags / "ci.md").unlink()
        (tags / "docs.md").write_text("changed\n")
        (tags / "obsolete.md").write_text("obsolete\n")
        before = self.snapshot()
        result = self.run_generator("generate-adr-by-tag.sh", "--check", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing in tree", result.stderr)
        self.assertIn("stale in tree", result.stderr)
        self.assertEqual(self.snapshot(), before)
        self.refresh()
        self.assertFalse((tags / "obsolete.md").exists())

    def test_unsafe_tag_does_not_replace_outputs(self) -> None:
        self.refresh()
        for tag in ("../outside", "ci/escape", "index"):
            with self.subTest(tag=tag):
                (self.adr / "0002-unsafe.md").write_text(f"# ADR-0002: Unsafe\nTags: {tag}\n")
                before = self.snapshot()
                self.assertNotEqual(
                    self.run_generator("generate-adr-by-tag.sh", "--write", check=False).returncode,
                    0,
                )
                self.assertEqual(self.snapshot(), before)

    def test_adr_navigation_is_collapsed(self) -> None:
        """ADR-1510: three static ADR entries, no generated block, no generator."""
        text = (ROOT / "mkdocs.yml").read_text()
        self.assertEqual(adr_nav_entries(text), ADR_NAV)
        self.assertNotIn("ADR-NAV-GENERATED", text)
        self.assertFalse((ROOT / "scripts/docs/generate-adr-nav.sh").exists())
        planted = text.replace(
            "      - By tag: adr/by-tag/index.md\n",
            "      - By tag: adr/by-tag/index.md\n      - ADR-0001: adr/0001-example.md\n",
        )
        self.assertNotEqual(planted, text)
        self.assertNotEqual(adr_nav_entries(planted), ADR_NAV)

    def test_make_order_required_docs_and_deploy_contract(self) -> None:
        makefile = (ROOT / "Makefile").read_text()
        # ADR-2197: the rendered outputs are written by `docs-render` and compared by
        # `docs-render-check`; `docs-fragments-write` calls the first and
        # `docs-fragments-check` does not compare them (a pull request carries none).
        for mode, name in (("check", "docs-render-check"), ("write", "docs-render")):
            target = makefile.split(f"\n{name}:", 1)[1].split("\n\n", 1)[0]
            scripts = [
                "concat-changelog-fragments",
                "concat-adr-index",
                "generate-adr-by-tag",
                "generate-record-titles",
                "concat-rebase-notes",
            ]
            positions = [target.index(f"{script}.") for script in scripts]
            self.assertEqual(positions, sorted(positions))
            for script in scripts:
                self.assertIn(f"--{mode}", target.split(script, 1)[1].splitlines()[0])
        self.assertIn("docs-fragments-write: docs-render\n", makefile)
        check = makefile.split("\ndocs-fragments-check:\n", 1)[1].split("\n\n", 1)[0]
        for rendered in ("concat-adr-index.sh --check", "generate-adr-by-tag.sh --check"):
            self.assertNotIn(rendered, check)
        self.assertNotIn("concat-changelog-fragments.sh --check", check)
        lint = (ROOT / ".github/workflows/lint-and-format.yml").read_text()
        docs = lint.split("  docs-lint:\n", 1)[1].split("  check-conflict-markers:\n", 1)[0]
        self.assertIn("# required-aggregator", docs)
        self.assertIn("run: make docs-fragments-check", docs)
        self.assertIn("run: make docs-render-check", docs)
        self.assertIn("if: github.event_name == 'push'", docs)
        # A test runs once in CI (ADR-1568): Tooling Tests runs this file.
        from scripts.ci.suite_registry import suite_members  # noqa: PLC0415

        self.assertIn("scripts/docs/tests/test_generators.py", suite_members(ROOT, "tooling"))
        pages = (ROOT / ".github/workflows/docs.yml").read_text()
        self.assertIn("docs: ${{ steps.impact.outputs.docs }}", pages)
        self.assertIn(
            "if: github.event_name == 'push' && needs.build.outputs.docs == 'true'", pages
        )

    def test_index_coverage_references_and_order_are_validated(self) -> None:
        fragments = self.adr / "_index_fragments"
        fragments.mkdir()
        (fragments / "_header.md").write_text("# ADR index\n\n")
        slugs = ("0001-example", "0100-second")
        for slug in slugs:
            (fragments / f"{slug}.md").write_text(f"| [ADR-{slug[:4]}]({slug}.md) | Example |\n")
        order = fragments / "_order.txt"
        order.write_text("\n".join(slugs) + "\n")
        self.run_generator("concat-adr-index.sh", "--write")
        self.run_generator("concat-adr-index.sh", "--check")
        order.write_text(order.read_text() + "0001-example\n")
        before = self.snapshot()
        result = self.run_generator("concat-adr-index.sh", "--write", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate ADR index order", result.stderr)
        self.assertEqual(self.snapshot(), before)
        order.write_text("\n".join(slugs) + "\n")
        (fragments / "0100-second.md").unlink()
        result = self.run_generator("concat-adr-index.sh", "--check", check=False)
        self.assertIn("missing ADR index fragment", result.stderr)
        (fragments / "0100-second.md").write_text(
            "| [ADR-0100](0100-second.md) | [ADR-9999](9999-absent.md) |\n"
        )
        result = self.run_generator("concat-adr-index.sh", "--check", check=False)
        self.assertIn("missing ADR reference 9999-absent.md", result.stderr)


if __name__ == "__main__":
    unittest.main()
