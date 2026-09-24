# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Regression coverage for historical issue-reference provenance."""

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
        "check_issue_reference_provenance",
        ROOT / "scripts/ci/check-issue-reference-provenance.py",
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot import issue-reference provenance checker")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


CHECKER = load_checker()


class IssueReferenceProvenanceTests(unittest.TestCase):
    def contract(self, *refs: str) -> object:
        return CHECKER.ProvenanceContract("docs/state.md", "historical CAMBI row", refs)

    def test_qualified_archived_refs_pass(self) -> None:
        text = "| historical CAMBI row | Issue lusoris/vmaf#857 | lusoris/vmaf#870 fixed it |\n"
        self.assertEqual(
            CHECKER.check_contract(text, self.contract("lusoris/vmaf#857", "lusoris/vmaf#870")), []
        )

    def test_bare_archived_ref_fails_red(self) -> None:
        text = "| historical CAMBI row | Issue #857 | PR #870 fixed it |\n"
        findings = CHECKER.check_contract(
            text, self.contract("lusoris/vmaf#857", "lusoris/vmaf#870")
        )
        self.assertIn("missing lusoris/vmaf#857", findings)
        self.assertIn("missing lusoris/vmaf#870", findings)
        self.assertIn("unqualified archived reference #857", findings)
        self.assertIn("unqualified archived reference #870", findings)

    def test_active_fork_refs_outside_the_historical_context_are_allowed(self) -> None:
        text = "\n".join(
            (
                "| historical CAMBI row | Issue lusoris/vmaf#857 |",
                "| active VMAFx row | PR #857 fixed a later unrelated defect |",
            )
        )
        self.assertEqual(CHECKER.check_contract(text, self.contract("lusoris/vmaf#857")), [])

    def test_wrapping_and_whitespace_do_not_disable_the_contract(self) -> None:
        text = "historical\nCAMBI   row cites\nIssue lusoris/vmaf#857\n"
        self.assertEqual(CHECKER.check_contract(text, self.contract("lusoris/vmaf#857")), [])

    def test_missing_or_duplicated_anchor_fails_closed(self) -> None:
        contract = self.contract("lusoris/vmaf#857")
        self.assertIn(
            "matched 0 logical blocks", CHECKER.check_contract("unrelated\n", contract)[0]
        )
        duplicated = (
            "historical CAMBI row lusoris/vmaf#857\n\nhistorical CAMBI row lusoris/vmaf#857\n"
        )
        self.assertIn("matched 2 logical blocks", CHECKER.check_contract(duplicated, contract)[0])

    def test_live_repository_satisfies_all_provenance_contracts(self) -> None:
        self.assertEqual(CHECKER.check_root(ROOT), [])

    def test_empty_and_missing_files_in_check_root_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            tmproot = Path(tmpdir)
            contract = CHECKER.ProvenanceContract(
                "docs/state.md", "historical CAMBI row", ("lusoris/vmaf#857",)
            )
            # Missing file reports file not found
            missing_findings = CHECKER.check_root(tmproot, (contract,))
            self.assertEqual(missing_findings, ["docs/state.md: file not found"])

            # Empty file fails closed because anchor matches 0 logical blocks
            state_path = tmproot / "docs/state.md"
            state_path.parent.mkdir(parents=True, exist_ok=True)
            state_path.write_text("", encoding="utf-8")
            empty_findings = CHECKER.check_root(tmproot, (contract,))
            self.assertEqual(len(empty_findings), 1)
            self.assertIn("matched 0 logical blocks", empty_findings[0])

    def test_main_cli_returns_zero_on_clean_repo(self) -> None:
        self.assertEqual(CHECKER.main(["--root", str(ROOT)]), 0)


if __name__ == "__main__":
    unittest.main()
