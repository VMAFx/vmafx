#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""No workflow grants a Scorecard-penalised write permission at the top level.

OpenSSF Scorecard's Token-Permissions check deducts for a top-level write of the
scopes in PENALISED (`contents`, `packages` and `actions` take the score to 0,
and so does `write-all`), and that one check moves the master Scorecard gate (scripts/ci/scorecard_gate.py, required aggregate 8.5):
`ci-escalate.yml` declared `actions: write` at the top and the gate failed on
every master push from #2390 on (aggregate 7.90 to 8.02). A job that needs a
write asks for it in its own `permissions:` block. Top-level `issues`,
`pull-requests`, `pages` and `id-token` writes are not deducted: master
40e87b159 had them and scored Token-Permissions 10.

Positive, negative and boundary cases run against synthetic workflows; the last
test runs the check over every tracked workflow.
"""

from __future__ import annotations

import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

WORKFLOWS = Path(__file__).resolve().parents[3] / ".github" / "workflows"

# The top-level scopes whose write Token-Permissions deducts for.
PENALISED = frozenset(
    {"actions", "checks", "contents", "deployments", "packages", "security-events", "statuses"}
)


def top_level_writes(workflow: dict[str, Any]) -> list[str]:
    """The penalised write grants of a workflow's top-level `permissions`."""
    perms = workflow.get("permissions")
    if perms is None:
        return []
    if isinstance(perms, str):
        return [perms] if perms == "write-all" else []
    if not isinstance(perms, dict):
        return [repr(perms)]
    return sorted(
        f"{scope}: {level}"
        for scope, level in perms.items()
        if level == "write" and scope in PENALISED
    )


class TopLevelWrites(unittest.TestCase):
    def test_read_only_top_level_passes(self) -> None:
        wf = yaml.safe_load("permissions:\n  contents: read\n  pull-requests: read\n")
        self.assertEqual(top_level_writes(wf), [])

    def test_job_level_write_passes(self) -> None:
        wf = yaml.safe_load(
            "permissions:\n  contents: read\n"
            "jobs:\n  a:\n    permissions:\n      actions: write\n"
        )
        self.assertEqual(top_level_writes(wf), [])

    def test_top_level_write_fails(self) -> None:
        wf = yaml.safe_load("permissions:\n  actions: write\n  contents: read\n")
        self.assertEqual(top_level_writes(wf), ["actions: write"])

    def test_write_all_fails(self) -> None:
        self.assertEqual(top_level_writes({"permissions": "write-all"}), ["write-all"])

    def test_unpenalised_top_level_write_passes(self) -> None:
        wf = yaml.safe_load("permissions:\n  issues: write\n  id-token: write\n")
        self.assertEqual(top_level_writes(wf), [])

    def test_boundary_read_all_and_empty_pass(self) -> None:
        self.assertEqual(top_level_writes({"permissions": "read-all"}), [])
        self.assertEqual(top_level_writes({"permissions": {}}), [])
        self.assertEqual(top_level_writes({}), [])

    def test_tracked_workflows_grant_no_top_level_write(self) -> None:
        files = sorted(WORKFLOWS.glob("*.yml")) + sorted(WORKFLOWS.glob("*.yaml"))
        self.assertTrue(files, f"no workflow under {WORKFLOWS}")
        offenders = {}
        for path in files:
            workflow = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            writes = top_level_writes(workflow)
            if writes:
                offenders[path.name] = writes
        self.assertEqual(offenders, {}, "move these grants to the job that needs them")


if __name__ == "__main__":
    unittest.main()
