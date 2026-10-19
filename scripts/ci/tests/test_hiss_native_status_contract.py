#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep the exit-status handling of the two workflows and the Rust example (HISS-07)."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DISCARD = re.compile(r"\|\|\s*true\b")
WORKFLOWS = (
    ".github/workflows/docker-image.yml",
    ".github/workflows/pr-type-label.yml",
)
EXAMPLE = "bindings/rust/vmafx-sys/examples/score.rs"


def discarded_status_lines(text: str) -> list[int]:
    """Return the 1-based numbers of the lines that discard a command's exit status."""
    return [n for n, line in enumerate(text.splitlines(), 1) if DISCARD.search(line)]


class StatusHandling(unittest.TestCase):
    def test_planted_discard_is_found(self) -> None:
        self.assertEqual(discarded_status_lines("a\ncat x || true\nb\n"), [2])

    def test_handled_status_is_not_flagged(self) -> None:
        self.assertEqual(discarded_status_lines("if ! cat x; then echo no; fi\n"), [])

    def test_workflows_do_not_discard_a_status(self) -> None:
        for rel in WORKFLOWS:
            with self.subTest(workflow=rel):
                text = (ROOT / rel).read_text(encoding="utf-8")
                self.assertEqual(discarded_status_lines(text), [])

    def test_label_workflow_warns_when_a_label_call_fails(self) -> None:
        text = (ROOT / WORKFLOWS[1]).read_text(encoding="utf-8")
        self.assertIn("could not read the labels", text)
        self.assertIn("could not remove label", text)

    def test_example_returns_an_error_instead_of_exiting(self) -> None:
        text = (ROOT / EXAMPLE).read_text(encoding="utf-8")
        self.assertNotIn("process::exit(1)", text)
        self.assertIn("no frames read", text)


if __name__ == "__main__":
    unittest.main()
