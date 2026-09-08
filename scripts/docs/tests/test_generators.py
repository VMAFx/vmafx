#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check source-driven ADR generation without modifying the caller's repo."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NAV = """site_name: fixture
nav:
  - ADRs:
      # >>> ADR-NAV-GENERATED
      stale: adr/missing.md
      # <<< ADR-NAV-GENERATED
extra:
  preserved: true
"""


class GeneratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-adr-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "scripts/docs").mkdir(parents=True)
        for script in (
            "generate-adr-by-tag.sh",
            "generate-adr-nav.sh",
            "concat-adr-index.sh",
            "check-adr-index.py",
        ):
            shutil.copyfile(ROOT / "scripts/docs" / script, self.root / "scripts/docs" / script)
        self.adr = self.root / "docs/adr"
        self.adr.mkdir(parents=True)
        self.nav = self.root / "mkdocs.yml"
        self.nav.write_text(NAV)
        (self.adr / "0000-template.md").write_text("# ADR-0000: Template\nTags: ignore\n")
        (self.adr / "0001-example.md").write_text(
            "# ADR-0001: _MSC_VER and <NAME> | `^## `\n"
            "- **Tags**: `CI`, ci, c++, <placeholder>, two words\n"
        )
        (self.adr / "0100-second.md").write_text("# ADR-0100: Second\nTags: docs\n")

    def run_generator(
        self, script: str, mode: str, *, check: bool = True
    ) -> subprocess.CompletedProcess[str]:
        executable = shutil.which("bash")
        assert executable is not None, "bash is required by the generator fixture"
        # ADR-1242: fixed repository scripts in a disposable fixture, no shell.
        result = subprocess.run(  # noqa: S603
            (executable, str(self.root / "scripts/docs" / script), mode),
            text=True,
            capture_output=True,
            check=False,
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
        self.run_generator("generate-adr-nav.sh", "--write")

    def test_roundtrip_title_rendering_dedup_and_readonly_checks(self) -> None:
        self.refresh()
        baseline = self.snapshot()
        self.refresh()
        self.assertEqual(self.snapshot(), baseline)
        self.run_generator("generate-adr-by-tag.sh", "--check")
        self.run_generator("generate-adr-nav.sh", "--check")
        self.assertEqual(self.snapshot(), baseline)
        tags = self.adr / "by-tag"
        self.assertEqual(
            {p.name for p in tags.glob("*.md")}, {"ci.md", "c++.md", "docs.md", "index.md"}
        )
        ci = (tags / "ci.md").read_text()
        self.assertIn("1 ADR(s)", ci)
        self.assertIn(r"\_MSC\_VER and &lt;NAME&gt; \| `^## `", ci)
        self.assertIn("markdownlint-disable MD013 MD038 MD060", ci)
        self.assertIn("adr/by-tag/c++.md", self.nav.read_text())
        self.assertTrue(self.nav.read_text().endswith("extra:\n  preserved: true\n"))

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
        self.nav.write_text(self.nav.read_text().replace("adr/0100-second.md", "adr/absent.md"))
        before = self.snapshot()
        self.assertNotEqual(
            self.run_generator("generate-adr-nav.sh", "--check", check=False).returncode, 0
        )
        self.assertEqual(self.snapshot(), before)

    def test_unsafe_tag_and_invalid_sentinel_do_not_replace_outputs(self) -> None:
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
        (self.adr / "0002-unsafe.md").unlink()
        for content in (
            NAV.replace("# <<< ADR-NAV-GENERATED", "# removed"),
            NAV + "# >>> ADR-NAV-GENERATED\n",
            NAV.replace("# >>> ADR-NAV-GENERATED", "# temp")
            .replace("# <<< ADR-NAV-GENERATED", "# >>> ADR-NAV-GENERATED")
            .replace("# temp", "# <<< ADR-NAV-GENERATED"),
        ):
            self.nav.write_text(content)
            for mode in ("--check", "--write"):
                self.assertNotEqual(
                    self.run_generator("generate-adr-nav.sh", mode, check=False).returncode, 0
                )
                self.assertEqual(self.nav.read_text(), content)

    def test_make_order_required_docs_and_deploy_contract(self) -> None:
        makefile = (ROOT / "Makefile").read_text()
        for mode in ("check", "write"):
            target = makefile.split(f"docs-fragments-{mode}:\n", 1)[1].split("\n\n", 1)[0]
            scripts = [
                "concat-changelog-fragments",
                "concat-adr-index",
                "generate-adr-by-tag",
                "generate-adr-nav",
            ]
            positions = [target.index(f"{script}.sh --{mode}") for script in scripts]
            self.assertEqual(positions, sorted(positions))
        lint = (ROOT / ".github/workflows/lint-and-format.yml").read_text()
        docs = lint.split("  docs-lint:\n", 1)[1].split("  check-conflict-markers:\n", 1)[0]
        self.assertIn("# required-aggregator", docs)
        self.assertIn("run: make docs-fragments-check", docs)
        self.assertIn("run: python3 scripts/docs/tests/test_generators.py", docs)
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
