#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the Gitleaks job to the history of the commit it checked out.

gitleaks falls back to `git log --all` when --log-opts is empty. With the
job's fetch-depth: 0 checkout that scans every branch in the repository, so
a finding on one branch failed every other pull request.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOW = ROOT / ".github/workflows/security-scans.yml"


def scan_command(workflow: str) -> str:
    """Return the `gitleaks detect` command of the Gitleaks job."""
    job = workflow.split("\n  secret-scan:", 1)[1].split("\n  dependency-review:", 1)[0]
    match = re.search(r"gitleaks detect \\\n(?:\s+.*\\\n)*\s+.*", job)
    if match is None:
        raise AssertionError("no `gitleaks detect` command in the secret-scan job")
    return match.group(0)


class GitleaksScanScopeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = WORKFLOW.read_text(encoding="utf-8")
        self.command = scan_command(self.workflow)

    def test_scans_checked_out_history_only(self) -> None:
        self.assertIn('--log-opts="--full-history HEAD"', self.command)

    def test_never_scans_all_refs(self) -> None:
        self.assertNotRegex(self.command, r"--all\b")
        self.assertNotRegex(self.command, r"--log-opts=\"?\s*\"?\s*\\")

    def test_keeps_the_full_history_checkout(self) -> None:
        job = self.workflow.split("\n  secret-scan:", 1)[1]
        self.assertIn("fetch-depth: 0", job.split("gitleaks detect", 1)[0])

    def test_fails_on_findings(self) -> None:
        self.assertIn("--exit-code 1", self.command)

    def test_parser_rejects_a_job_without_the_command(self) -> None:
        with self.assertRaises(AssertionError):
            scan_command("\n  secret-scan:\n    steps: []\n  dependency-review:\n")


if __name__ == "__main__":
    unittest.main()
