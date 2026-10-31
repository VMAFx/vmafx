#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract test for ADR Collision Guard job in rule-enforcement.yml."""

from __future__ import annotations

import os
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WORKFLOW = ROOT / ".github" / "workflows" / "rule-enforcement.yml"


def extract_job(workflow_text: str, job_id: str = "adr-collision-check") -> str:
    """Return one top-level job block by its identifier."""
    pattern = rf"(?ms)^  {re.escape(job_id)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)"
    match = re.search(pattern, workflow_text)
    if match is None:
        raise AssertionError(f"workflow job not found: {job_id}")
    return match.group(0)


def strip_comments(text: str) -> str:
    """Return text with full-line comments removed."""
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def verify_adr_collision_guard(job_text: str) -> None:
    """Verify that ADR Collision Guard preserves ancestry and full history."""
    # 1. Checkout fetch-depth must be 0 (full clone)
    checkout_match = re.search(
        r"(?ms)uses:\s+actions/checkout@\S+.*?with:\s*.*?\bfetch-depth:\s*0\b",
        job_text,
    )
    if not checkout_match:
        raise AssertionError("ADR Collision Guard must use fetch-depth: 0")

    # 2. Descendant check requires merge-base --is-ancestor
    exec_body = strip_comments(job_text)
    if "merge-base --is-ancestor" not in exec_body:
        raise AssertionError(
            "ADR Collision Guard must use merge-base --is-ancestor for descendant checks"
        )

    # 3. No git fetch line may carry --depth when ancestry is tested
    depth_fetch_lines = [
        line.strip()
        for line in exec_body.splitlines()
        if "git fetch" in line and re.search(r"--depth\b", line)
    ]
    if depth_fetch_lines:
        raise AssertionError(
            "git fetch in ADR Collision Guard must not use --depth (grafts tip and cuts ancestry):\n"
            + "\n".join(f"  {ln}" for ln in depth_fetch_lines)
        )


class AdrCollisionGuardWorkflowContract(unittest.TestCase):
    def test_workflow_contract(self) -> None:
        workflow_path = Path(os.environ.get("VMAFX_RULE_ENFORCEMENT_WORKFLOW", DEFAULT_WORKFLOW))
        content = workflow_path.read_text(encoding="utf-8")
        job = extract_job(content)
        self.assertIn("name: ADR Collision Guard", job)
        verify_adr_collision_guard(job)

    def test_planted_depth_fetch_rejected(self) -> None:
        """Planted negative case: a shallow git fetch must fail the contract."""
        synthetic_job = (
            "  adr-collision-check:\n"
            "    name: ADR Collision Guard\n"
            "    steps:\n"
            "      - uses: actions/checkout@v7\n"
            "        with:\n"
            "          fetch-depth: 0\n"
            "      - name: Fetch PR tip\n"
            "        run: |\n"
            '          git fetch --depth=1 origin "${pr_sha}"\n'
            '          git merge-base --is-ancestor "${HEAD_SHA}" "${pr_sha}"\n'
        )
        with self.assertRaises(AssertionError) as ctx:
            verify_adr_collision_guard(synthetic_job)
        self.assertIn(
            "git fetch in ADR Collision Guard must not use --depth",
            str(ctx.exception),
        )

    def test_planted_nonzero_checkout_depth_rejected(self) -> None:
        """Planted negative case: non-zero checkout depth must fail."""
        synthetic_job = (
            "  adr-collision-check:\n"
            "    name: ADR Collision Guard\n"
            "    steps:\n"
            "      - uses: actions/checkout@v7\n"
            "        with:\n"
            "          fetch-depth: 1\n"
            "      - name: Check\n"
            "        run: |\n"
            '          git merge-base --is-ancestor "${HEAD_SHA}" "${pr_sha}"\n'
        )
        with self.assertRaises(AssertionError) as ctx:
            verify_adr_collision_guard(synthetic_job)
        self.assertIn("fetch-depth: 0", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
