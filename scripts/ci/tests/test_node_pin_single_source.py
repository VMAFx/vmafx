#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify the pre-commit, .node-version and setup-node pins name build-config.env's NODE_VERSION.

Unpinned, pre-commit on Windows builds its node hooks' environment at nodeenv's
"latest", the head of the Node index. On 2026-10-29 that head was the
prerelease v27.0.0-alpha.0.0.0, nodeenv 1.11.0 could not parse it, and the
Windows Lefthook Pre-Commit job on master failed. `check_node_pins()` refuses a
missing or differing pre-commit pin, a differing setup-node pin and a differing
.node-version, the file the engine-managed documentation workflow reads Node from.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from typing import Protocol, cast

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NodePinGate(Protocol):
    def check_node_pins(self, root: Path, node_want: str | None) -> list[str]: ...

    def load_config(self, root: Path) -> dict[str, str]: ...


GATE = cast(NodePinGate, MODULE)
WANT = "24.22.1"
PRECOMMIT = 'fail_fast: false\ndefault_language_version:\n  node: "{version}"\nrepos: []\n'
WORKFLOW = (
    "jobs:\n  lint:\n    steps:\n      - uses: actions/setup-node@949feb24  # v7.1.0\n"
    '        with:\n          node-version: "{version}"\n'
)


class NodePinSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        self.write(".pre-commit-config.yaml", PRECOMMIT.format(version=WANT))
        self.write(".github/workflows/lint.yml", WORKFLOW.format(version=WANT))
        self.write(".node-version", f"{WANT}\n")

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_matching_pins_pass(self) -> None:
        self.assertEqual(GATE.check_node_pins(self.repo, WANT), [])

    def test_repository_pins_match_config(self) -> None:
        want = GATE.load_config(ROOT).get("NODE_VERSION")
        self.assertEqual(GATE.check_node_pins(ROOT, want), [])

    def test_missing_config_value_fails(self) -> None:
        self.assertEqual(
            GATE.check_node_pins(self.repo, None), ["build-config.env defines no NODE_VERSION"]
        )

    def test_unpinned_precommit_fails(self) -> None:
        self.write(".pre-commit-config.yaml", "fail_fast: false\nrepos: []\n")
        problems = GATE.check_node_pins(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("no default_language_version node pin", problems[0])

    def test_differing_precommit_pin_fails(self) -> None:
        self.write(".pre-commit-config.yaml", PRECOMMIT.format(version="24.22.0"))
        problems = GATE.check_node_pins(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("pins node '24.22.0'", problems[0])

    def test_differing_workflow_pin_fails(self) -> None:
        self.write(".github/workflows/lint.yml", WORKFLOW.format(version="27.0.0"))
        problems = GATE.check_node_pins(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn("node-version '27.0.0'", problems[0])

    def test_node_version_file_without_newline_passes(self) -> None:
        self.write(".node-version", WANT)
        self.assertEqual(GATE.check_node_pins(self.repo, WANT), [])

    def test_missing_node_version_file_fails(self) -> None:
        (self.repo / ".node-version").unlink()
        problems = GATE.check_node_pins(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn(".node-version is missing", problems[0])

    def test_leading_blank_node_version_file_fails(self) -> None:
        self.write(".node-version", f"\n{WANT}\n")
        self.assertEqual(len(GATE.check_node_pins(self.repo, WANT)), 1)

    def test_differing_node_version_file_fails(self) -> None:
        self.write(".node-version", "26.0.0\n")
        problems = GATE.check_node_pins(self.repo, WANT)
        self.assertEqual(len(problems), 1)
        self.assertIn(".node-version says '26.0.0'", problems[0])

    def test_empty_node_version_file_fails(self) -> None:
        self.write(".node-version", "\n")
        self.assertEqual(len(GATE.check_node_pins(self.repo, WANT)), 1)

    def test_major_only_workflow_pin_on_the_same_line_passes(self) -> None:
        self.write(".github/workflows/lint.yml", WORKFLOW.format(version="24"))
        self.assertEqual(GATE.check_node_pins(self.repo, WANT), [])

    def test_major_only_workflow_pin_on_another_line_fails(self) -> None:
        self.write(".github/workflows/lint.yml", WORKFLOW.format(version="27"))
        self.assertEqual(len(GATE.check_node_pins(self.repo, WANT)), 1)


if __name__ == "__main__":
    unittest.main()
