#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The Cursor workspace rule moved into the canonical agent documents (#3346).

The former `.cursor/rules/vmafx-workspace.mdc` was an instruction file outside
`AGENTS.md`, which `praetorctl compile-context --verify` refuses. These checks
are structural: the hand-written file is gone, the canonical pages carry the
migrated sentences, `AGENTS.md` points at them, and the generated Cursor
projection carries the same pointer (regenerate it, do not hand-edit).
"""

from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RULES = ROOT / ".cursor" / "rules"


class CursorRulesMigrationTest(unittest.TestCase):
    def test_hand_written_workspace_rule_is_gone(self) -> None:
        self.assertFalse((RULES / "vmafx-workspace.mdc").exists())

    def test_workspace_defaults_are_in_ide_setup(self) -> None:
        text = (ROOT / "docs" / "development" / "ide-setup.md").read_text(encoding="utf-8")
        self.assertIn("canonical workspace\ndefaults", text)
        self.assertIn("clangd as the only C/C++ language server", text)

    def test_agent_integration_contract_is_in_the_reference(self) -> None:
        text = (ROOT / "docs" / "development" / "agent-reference.md").read_text(encoding="utf-8")
        self.assertIn("## Agent integration contract", text)
        self.assertIn("explicit approval", text)
        self.assertIn("smallest applicable checked-in task", text)

    def test_agents_md_points_at_both_pages(self) -> None:
        text = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        self.assertIn("docs/development/ide-setup.md", text)
        self.assertIn("docs/development/agent-reference.md#agent-integration-contract", text)

    def test_generated_cursor_projection_carries_the_pointer(self) -> None:
        text = (RULES / "hiss-invariants.mdc").read_text(encoding="utf-8")
        self.assertIn("ide-setup.md", text)
        self.assertIn("agent-reference.md#agent-integration-contract", text)

    def test_workspace_rule_names_only_the_migrated_page(self) -> None:
        self.assertEqual(["hiss-invariants.mdc"], sorted(path.name for path in RULES.glob("*.mdc")))


if __name__ == "__main__":
    unittest.main()
