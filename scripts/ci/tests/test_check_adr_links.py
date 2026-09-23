# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary coverage for the ADR link checker (HISS-15)."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]


def load_checker() -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        "check_adr_links", ROOT / "scripts/ci/check-adr-links.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot import scripts/ci/check-adr-links.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


CHECKER = load_checker()


class AdrLinkCheckerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.docs = Path(self.tmp.name) / "docs"
        (self.docs / "adr").mkdir(parents=True)
        self.write_adr("0042-the-original-slug.md")

    def write_adr(self, name: str) -> None:
        (self.docs / "adr" / name).write_text(f"# {name}\n", encoding="utf-8")

    def page(self, body: str, name: str = "page.md") -> Path:
        path = self.docs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")
        return path

    def run_checker(self, *, fix: bool = False) -> int:
        # CHECKER comes from spec_from_file_location, so mypy types main() as
        # Any. int() both narrows it and asserts the contract the exit code is.
        return int(
            CHECKER.main(["check-adr-links", "--docs", str(self.docs)] + (["--fix"] if fix else []))
        )

    # positive
    def test_resolving_link_passes(self) -> None:
        self.page("see [ADR-0042](adr/0042-the-original-slug.md) for why\n")
        self.assertEqual(self.run_checker(), 0)

    def test_relative_prefix_is_understood(self) -> None:
        self.page("see [ADR-0042](../adr/0042-the-original-slug.md)\n", "guide/x.md")
        self.assertEqual(self.run_checker(), 0)

    def test_no_links_at_all_passes(self) -> None:
        self.page("prose with no citations\n")
        self.assertEqual(self.run_checker(), 0)

    # negative
    def test_stale_slug_fails(self) -> None:
        self.page("see [ADR-0042](adr/0042-a-slug-that-was-renamed.md)\n")
        self.assertEqual(self.run_checker(), 1)

    def test_number_with_no_adr_file_fails(self) -> None:
        self.page("see [ADR-0999](adr/0999-never-written.md)\n")
        self.assertEqual(self.run_checker(), 1)

    def test_missing_docs_directory_is_a_usage_error(self) -> None:
        self.assertEqual(CHECKER.main(["check-adr-links", "--docs", str(self.docs / "nope")]), 2)

    # slug-first resolution -- the case a by-number repair gets wrong
    def test_a_wrong_number_with_a_right_slug_resolves_by_slug(self) -> None:
        """The ADR collision sweeps renamed files, keeping the slug.

        A citation written before the sweep carries the old number and the still
        correct slug. Repairing it from the number points it at whatever unrelated
        ADR now holds that number; repairing from the slug recovers the decision
        the author meant.
        """
        self.write_adr("0389-vmaf-tiny-v3.md")  # the renamed file
        page = self.page("[ADR-0042](adr/0042-vmaf-tiny-v3.md) shipped it\n")
        self.assertEqual(self.run_checker(fix=True), 0)
        body = page.read_text(encoding="utf-8")
        self.assertIn("adr/0389-vmaf-tiny-v3.md", body)
        self.assertNotIn("0042-the-original-slug.md", body)

    def test_a_slug_resolve_rewrites_the_link_text_too(self) -> None:
        self.write_adr("0389-vmaf-tiny-v3.md")
        page = self.page("[ADR-0042](adr/0042-vmaf-tiny-v3.md)\n")
        self.assertEqual(self.run_checker(fix=True), 0)
        self.assertIn("[ADR-0389]", page.read_text(encoding="utf-8"))

    def test_a_descriptive_label_keeps_its_prose_and_moves_its_number(self) -> None:
        self.write_adr("0389-vmaf-tiny-v3.md")
        page = self.page("[ADR-0042 — the ship decision](adr/0042-vmaf-tiny-v3.md)\n")
        self.assertEqual(self.run_checker(fix=True), 0)
        self.assertIn("[ADR-0389 — the ship decision]", page.read_text(encoding="utf-8"))

    def test_slug_beats_number_when_both_could_resolve(self) -> None:
        """0042 exists and so does the slug under 0389. The slug must win."""
        self.write_adr("0389-vmaf-tiny-v3.md")
        page = self.page("[ADR-0042](adr/0042-vmaf-tiny-v3.md)\n")
        self.assertEqual(self.run_checker(fix=True), 0)
        self.assertIn("adr/0389-vmaf-tiny-v3.md", page.read_text(encoding="utf-8"))

    # the repair
    def test_fix_repoints_a_stale_slug_and_then_passes(self) -> None:
        page = self.page("see [ADR-0042](adr/0042-a-slug-that-was-renamed.md)\n")
        self.assertEqual(self.run_checker(fix=True), 0)
        self.assertIn("adr/0042-the-original-slug.md", page.read_text(encoding="utf-8"))
        self.assertEqual(self.run_checker(), 0)

    def test_fix_preserves_the_relative_prefix(self) -> None:
        page = self.page("[ADR-0042](../../adr/0042-old.md)\n", "a/b/c.md")
        self.assertEqual(self.run_checker(fix=True), 0)
        self.assertIn("(../../adr/0042-the-original-slug.md)", page.read_text(encoding="utf-8"))

    def test_fix_refuses_a_number_with_no_file(self) -> None:
        page = self.page("[ADR-0999](adr/0999-never-written.md)\n")
        self.assertEqual(self.run_checker(fix=True), 1)
        self.assertIn("0999-never-written.md", page.read_text(encoding="utf-8"))

    # boundary
    def test_ambiguous_number_is_not_auto_repointed(self) -> None:
        """Two files sharing a number: the tool must not guess between them."""
        self.write_adr("0042-a-duplicate.md")
        page = self.page("[ADR-0042](adr/0042-some-third-slug.md)\n")
        self.assertEqual(self.run_checker(fix=True), 1)
        self.assertIn("0042-some-third-slug.md", page.read_text(encoding="utf-8"))

    def test_three_digit_number_is_not_treated_as_an_adr_link(self) -> None:
        self.page("[x](adr/042-too-short.md)\n")
        self.assertEqual(self.run_checker(), 0)

    def test_the_repository_itself_is_clean(self) -> None:
        self.assertEqual(CHECKER.main(["check-adr-links", "--docs", str(ROOT / "docs")]), 0)


if __name__ == "__main__":
    unittest.main()
