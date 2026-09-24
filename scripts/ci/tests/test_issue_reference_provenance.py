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

MISSED_SCOPE_CONTRACTS = (
    CHECKER.ProvenanceContract(
        "changelog.d/fixed/cambi-cuda-segfault.md",
        "### cambi_cuda: fix SIGSEGV on every input",
        ("lusoris/vmaf#857", "lusoris/vmaf#866"),
    ),
    CHECKER.ProvenanceContract(
        "changelog.d/fixed/cambi-cuda-host-preprocessing.md",
        "`integer_cambi_cuda.c::submit_fex_cuda` called",
        ("lusoris/vmaf#857", "lusoris/vmaf#870"),
    ),
    CHECKER.ProvenanceContract(
        "changelog.d/changed/cuda-extractor-cambi-and-ssim-promotion.md",
        "Per-clip wall time on CUDA workers improves",
        ("lusoris/vmaf#857", "lusoris/vmaf#866", "lusoris/vmaf#870"),
    ),
    CHECKER.ProvenanceContract(
        "changelog.d/changed/changelog-d-stale-fragments-cleanup.md",
        "rewritten to drop the contradiction with the cambi_cuda SIGSEGV",
        ("lusoris/vmaf#866", "lusoris/vmaf#870"),
    ),
    CHECKER.ProvenanceContract(
        "changelog.d/changed/state-md-refresh-2026-05-03.md",
        "`docs/state.md` refresh 2026-05-03",
        ("lusoris/vmaf#239", "lusoris/vmaf#241", "lusoris/vmaf#310"),
    ),
    CHECKER.ProvenanceContract(
        "changelog.d/changed/state-md-github-issues-crossref.md",
        "Cross-reference `docs/state.md` against `VMAFx/vmafx`",
        (
            "lusoris/vmaf#239",
            "lusoris/vmaf#241",
            "lusoris/vmaf#310",
            "lusoris/vmaf#857",
            "lusoris/vmaf#870",
        ),
    ),
    CHECKER.ProvenanceContract(
        "changelog.d/changed/state-md-audit-y4m-oob-239-cleanup.md",
        "orphaned `|---|---|---|---|---|` separator",
        ("lusoris/vmaf#239", "lusoris/vmaf#241"),
    ),
    CHECKER.ProvenanceContract(
        "changelog.d/fixed/adr-link-slug-drift.md",
        "An ADR link carries the decision's identity twice",
        ("lusoris/vmaf#310", "lusoris/vmaf#752"),
    ),
    CHECKER.ProvenanceContract(
        "core/src/feature/cuda/AGENTS.md",
        "`cuLaunchKernel` `kernelParams[]` must point",
        ("lusoris/vmaf#857", "lusoris/vmaf#866"),
    ),
    CHECKER.ProvenanceContract(
        "core/src/feature/cuda/AGENTS.md",
        "Host-side preprocessing in CUDA feature extractor",
        ("lusoris/vmaf#857", "lusoris/vmaf#870"),
    ),
    CHECKER.ProvenanceContract(
        "core/src/feature/cuda/integer_cambi_cuda.c",
        "dispatch_mask — GPU spatial-mask kernel",
        ("lusoris/vmaf#857",),
    ),
    CHECKER.ProvenanceContract(
        "core/src/feature/cuda/integer_cambi_cuda.c",
        "dispatch_decimate — GPU",
        ("lusoris/vmaf#857",),
    ),
    CHECKER.ProvenanceContract(
        "core/src/feature/cuda/integer_cambi_cuda.c",
        "dispatch_filter_mode — GPU 3-tap mode filter",
        ("lusoris/vmaf#857",),
    ),
    CHECKER.ProvenanceContract(
        "core/src/feature/cuda/integer_cambi_cuda.c",
        "Step 0: download dist_pic GPU→host",
        ("lusoris/vmaf#857",),
    ),
    CHECKER.ProvenanceContract(
        "docs/metrics/cambi.md",
        "**Implementation note",
        ("lusoris/vmaf#870",),
    ),
    CHECKER.ProvenanceContract(
        "docs/sync-upstream/2026-05-02-sync-report.md",
        "**Fork tip**",
        ("lusoris/vmaf#241",),
    ),
    CHECKER.ProvenanceContract(
        "docs/sync-upstream/2026-05-03-sync-report.md",
        "`af227b02`",
        ("lusoris/vmaf#310",),
    ),
    CHECKER.ProvenanceContract(
        "scripts/ci/check-adr-links.py",
        "Slug wins when both could apply",
        ("lusoris/vmaf#310", "lusoris/vmaf#752"),
    ),
    CHECKER.ProvenanceContract(
        "docs/development/adr-workflow.md",
        "The second row is not hypothetical",
        ("lusoris/vmaf#310", "lusoris/vmaf#752"),
    ),
    CHECKER.ProvenanceContract(
        "docs/state.md",
        "Root cause of the larger half, verified from history rather than inferred",
        ("lusoris/vmaf#310", "lusoris/vmaf#752"),
    ),
    CHECKER.ProvenanceContract(
        "docs/development/post-merge-profile-2026-05-03.md",
        "**Branch / Commit:**",
        (
            "lusoris/vmaf#310",
            "lusoris/vmaf#312",
            "lusoris/vmaf#314",
            "lusoris/vmaf#319",
            "lusoris/vmaf#320",
            "lusoris/vmaf#321",
        ),
    ),
    CHECKER.ProvenanceContract(
        "docs/research/0053-post-merge-cpu-profile-2026-05-03.md",
        "A full CPU perf profile was collected",
        (
            "lusoris/vmaf#310",
            "lusoris/vmaf#312",
            "lusoris/vmaf#314",
            "lusoris/vmaf#319",
            "lusoris/vmaf#320",
            "lusoris/vmaf#321",
        ),
    ),
    CHECKER.ProvenanceContract(
        "docs/research/README.md",
        "Post-merge CPU profile 2026-05-03",
        ("lusoris/vmaf#310", "lusoris/vmaf#321"),
    ),
    CHECKER.ProvenanceContract(
        "docs/ai/mos-corpora.md",
        "Use `ai/scripts/merge_corpora.py`",
        ("lusoris/vmaf#407",),
    ),
    CHECKER.ProvenanceContract(
        "docs/research/0136-hdr-ugc-dataset-license-audit-2026-05-15.md",
        "Despite the attractive technical specifications",
        ("lusoris/vmaf#407",),
    ),
    CHECKER.ProvenanceContract(
        "CHANGELOG.md",
        "rewritten to drop the contradiction with the cambi_cuda SIGSEGV",
        ("lusoris/vmaf#866", "lusoris/vmaf#870"),
    ),
    CHECKER.ProvenanceContract(
        "CHANGELOG.md",
        "Per-clip wall time on CUDA workers improves",
        ("lusoris/vmaf#857", "lusoris/vmaf#866", "lusoris/vmaf#870"),
    ),
    CHECKER.ProvenanceContract(
        "CHANGELOG.md",
        "orphaned `|---|---|---|---|---|` separator",
        ("lusoris/vmaf#239", "lusoris/vmaf#241"),
    ),
    CHECKER.ProvenanceContract(
        "CHANGELOG.md",
        "Cross-reference `docs/state.md` against `VMAFx/vmafx`",
        (
            "lusoris/vmaf#239",
            "lusoris/vmaf#241",
            "lusoris/vmaf#310",
            "lusoris/vmaf#857",
            "lusoris/vmaf#870",
        ),
    ),
    CHECKER.ProvenanceContract(
        "CHANGELOG.md",
        "`docs/state.md` refresh 2026-05-03",
        ("lusoris/vmaf#239", "lusoris/vmaf#241", "lusoris/vmaf#310"),
    ),
    CHECKER.ProvenanceContract(
        "CHANGELOG.md",
        "An ADR link carries the decision's identity twice",
        ("lusoris/vmaf#310", "lusoris/vmaf#752"),
    ),
    CHECKER.ProvenanceContract(
        "CHANGELOG.md",
        "`integer_cambi_cuda.c::submit_fex_cuda` called",
        ("lusoris/vmaf#857", "lusoris/vmaf#870"),
    ),
    CHECKER.ProvenanceContract(
        "CHANGELOG.md",
        "### cambi_cuda: fix SIGSEGV on every input",
        ("lusoris/vmaf#857", "lusoris/vmaf#866"),
    ),
)


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

    def test_wrong_active_repository_forms_fail_red(self) -> None:
        wrong_forms = (
            "VMAFx/vmafx#857",
            "https://github.com/VMAFx/vmafx/issues/857",
            "https://github.com/VMAFx/vmafx/pull/857",
        )
        for wrong_form in wrong_forms:
            with self.subTest(wrong_form=wrong_form):
                text = f"| historical CAMBI row | {wrong_form} |\n"
                findings = CHECKER.check_contract(text, self.contract("lusoris/vmaf#857"))
                self.assertIn(
                    "wrong repository VMAFx/vmafx for archived reference #857",
                    findings,
                )

    def test_active_fork_refs_outside_the_historical_context_are_allowed(self) -> None:
        text = "\n".join(
            (
                "| historical CAMBI row | Issue lusoris/vmaf#857 |",
                "| active VMAFx row | PR #857 fixed a later unrelated defect |",
            )
        )
        self.assertEqual(CHECKER.check_contract(text, self.contract("lusoris/vmaf#857")), [])

    def test_proven_active_vmafx_reference_is_not_rewritten_as_archived(self) -> None:
        text = (
            ROOT / "docs/research/0870-helm-values-schema-and-container-rebuild-audit.md"
        ).read_text(encoding="utf-8")
        matches = [
            block
            for block in CHECKER.logical_blocks(text)
            if "The Containerfile (last touched" in block
        ]
        self.assertEqual(len(matches), 1)
        self.assertIn("PR #239", matches[0])
        self.assertNotIn("lusoris/vmaf#239", matches[0])

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

    def test_live_repository_covers_every_proven_historical_context(self) -> None:
        for contract in MISSED_SCOPE_CONTRACTS:
            with self.subTest(path=contract.path, anchor=contract.anchor):
                self.assertEqual(CHECKER.check_root(ROOT, (contract,)), [])

    def test_production_gate_contains_every_proven_historical_context(self) -> None:
        self.assertTrue(set(MISSED_SCOPE_CONTRACTS).issubset(set(CHECKER.CONTRACTS)))

    def test_production_contract_identities_are_unique(self) -> None:
        identities = [(contract.path, contract.anchor) for contract in CHECKER.CONTRACTS]
        self.assertEqual(len(identities), len(set(identities)))

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
