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

# ADR-1297 extended the same fail-closed contract to the gates it promoted that
# have no trigger path filter, no conditional skip, and report on both
# `pull_request` and `push` to master. For these, "the check never appeared" is
# a broken workflow or a vanished runner, not an ADR-0313 path skip. Kept as a
# separate set so the ADR-1274 pin above stays an exact statement about the
# replay contexts.
ADR_1297_STRICT_CONTEXTS = {
    "Coverage Gate",
    "MCP Smoke",
    "Markdown Lint",
    "No Conflict Markers",
    "Tiny-Model Registry Validate",
    "Windows ARM64 MSVC",
}

# BUG-098 replaced trigger-level path filters on these required consumer
# workflows with unconditional planner -> work -> gate jobs. They now report on
# every pull request and master push, so absence is a registration failure, not
# an ADR-0313 not-applicable result.
BUG_098_STRICT_CONTEXTS = {
    "Linux Intel LLVM",
    "macOS Clang+Metal",
    "Windows MSVC+CUDA (full)",
    "FFmpeg Ubuntu gcc",
    "FFmpeg macOS clang",
    "FFmpeg SYCL",
    "Docker Image Build",
    "Dev Container Build",
    "vmafx-sys CI",
    "cargo-deny",
    "helm lint + template",
    "Doxygen Public API",
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
        self.assertEqual(
            strict,
            STRICT_CONTEXTS | ADR_1297_STRICT_CONTEXTS | BUG_098_STRICT_CONTEXTS,
        )
        self.assertTrue(strict >= STRICT_CONTEXTS)
        self.assertTrue(strict <= required)
        self.assertIn("if (strictMustReport.includes(name))", aggregator)
        self.assertIn("if (!run)", aggregator)
        self.assertIn("run.conclusion !== 'success'", aggregator)
        self.assertIn("never reported (strict required context)", aggregator)

    def test_public_claim_matches_the_enforced_revision(self) -> None:
        readme = read("README.md")
        agents = read("AGENTS.md")
        declared_revisions = {int(value) for value in re.findall(r"HISS-(\d+)", agents)}
        self.assertTrue(declared_revisions)
        enforced_revision = max(declared_revisions)

        self.assertIn(f"HISS--{enforced_revision}", readme)
        self.assertIn(f"(HISS-{enforced_revision})", readme)
        advertised_revisions = {int(value) for value in re.findall(r"HISS-(?:-)?(\d+)", readme)}
        self.assertEqual(advertised_revisions, {enforced_revision})


if __name__ == "__main__":
    unittest.main()
