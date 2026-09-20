#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the fail-closed HISS-20/21 replay wiring from ADR-1274."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
STRICT_CONTEXTS = {
    "Standards & Invariant Verification Gate",
    "HISS Replay Evidence (Linux)",
    "HISS Replay Evidence (macOS)",
    "HISS Replay Evidence (Windows)",
}


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def javascript_array(source: str, name: str) -> set[str]:
    match = re.search(rf"const {re.escape(name)} = \[(.*?)\];", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing JavaScript array: {name}")
    return set(re.findall(r"'([^']+)'", match.group(1)))


class HissReplayContractTests(unittest.TestCase):
    def test_catalog_and_local_entrypoints_exist(self) -> None:
        catalog = read(".config/hiss/coverage.yaml")
        self.assertIn("# Replayable enforcement evidence", catalog)
        self.assertRegex(catalog, r"(?m)^version: 1$")
        self.assertGreaterEqual(catalog.count("  - id: HISS-"), 6)

        makefile = read("Makefile")
        self.assertRegex(makefile, r"(?m)^verify-all:\n\t.*hiss coverage --verify$")
        self.assertRegex(makefile, r"(?m)^hiss-coverage:\n\t.*hiss coverage --verify$")

        hooks = read("lefthook.yml")
        self.assertEqual(hooks.count("    hiss-evidence:\n"), 2)
        self.assertGreaterEqual(hooks.count("hiss coverage --verify"), 2)

    def test_platform_workflow_always_reports_all_replay_contexts(self) -> None:
        workflow = read(".github/workflows/standards-gate.yml")
        self.assertNotRegex(workflow, r"(?m)^\s+paths(?:-ignore)?:")
        self.assertIn("name: Standards & Invariant Verification Gate", workflow)
        self.assertIn("name: HISS Replay Evidence (${{ matrix.name }})", workflow)
        self.assertEqual(workflow.count("standardsctl hiss coverage --verify"), 2)
        for name, runner in (
            ("Linux", "ubuntu-26.04"),
            ("macOS", "macos-15"),
            ("Windows", "windows-2025"),
        ):
            self.assertIn(f"- name: {name}\n            os: {runner}", workflow)

    def test_aggregator_rejects_absent_skipped_or_neutral_replay(self) -> None:
        aggregator = read(".github/workflows/required-aggregator.yml")
        required = javascript_array(aggregator, "required")
        strict = javascript_array(aggregator, "strictMustReport")
        self.assertEqual(strict, STRICT_CONTEXTS)
        self.assertTrue(strict <= required)
        self.assertIn("if (strictMustReport.includes(name))", aggregator)
        self.assertIn("if (!run)", aggregator)
        self.assertIn("run.conclusion !== 'success'", aggregator)
        self.assertIn("never reported (strict required context)", aggregator)

    def test_public_claim_matches_the_enforced_revision(self) -> None:
        self.assertIn("HISS--21", read("README.md"))
        agents = read("AGENTS.md")
        self.assertIn("HISS-20", agents)
        self.assertIn("HISS-21", agents)


if __name__ == "__main__":
    unittest.main()
