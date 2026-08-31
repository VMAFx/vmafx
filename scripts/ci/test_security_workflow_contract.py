#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Guard independent Security Scans concurrency domains by event type."""

from __future__ import annotations

import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SECURITY_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "security-scans.yml"
EXPECTED_GROUP = "group: security-${{ github.workflow }}-${{ github.event_name }}-${{ github.ref }}"
COLLIDING_GROUP = "group: security-${{ github.workflow }}-${{ github.ref }}"


class SecurityWorkflowContractTest(unittest.TestCase):
    """Prevent schedules from canceling master-push security coverage."""

    def test_concurrency_is_scoped_by_event_and_ref(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        concurrency = workflow.split("\nconcurrency:\n", maxsplit=1)[1].split(
            "\njobs:\n", maxsplit=1
        )[0]

        self.assertIn(EXPECTED_GROUP, concurrency)
        self.assertNotIn(COLLIDING_GROUP, concurrency)
        self.assertIn("cancel-in-progress: true", concurrency)


if __name__ == "__main__":
    unittest.main()
