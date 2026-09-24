#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Contract tests for Rust CI impact routing and required check emission.

The workflow must start for every pull request so its required contexts cannot
disappear.  The shared impact planner decides whether the expensive Rust work
runs; public libvmaf headers remain an input because bindgen consumes them.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

REPO_ROOT = Path(__file__).resolve().parents[3]
RUST_CI_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "rust-ci.yml"
CONFIG = REPO_ROOT / ".github" / "ci-impact.json"


class RustCIWorkflowContractTest(unittest.TestCase):
    """Ensure Rust work is routed in-job and its required checks always report."""

    def test_rust_ci_has_no_workflow_path_filters(self) -> None:
        self.assertTrue(RUST_CI_WORKFLOW.exists(), f"missing {RUST_CI_WORKFLOW}")
        raw = RUST_CI_WORKFLOW.read_text(encoding="utf-8")
        parsed = yaml.safe_load(raw)

        on = parsed.get("on") or parsed.get(True) or {}
        for event_name in ("push", "pull_request"):
            event = on.get(event_name, {})
            with self.subTest(event=event_name):
                self.assertNotIn("paths", event)
                self.assertNotIn("paths-ignore", event)

    def test_rust_ci_uses_fail_closed_planner_work_gate_contract(self) -> None:
        parsed = yaml.safe_load(RUST_CI_WORKFLOW.read_text(encoding="utf-8"))
        jobs = parsed["jobs"]
        self.assertEqual(jobs["impact"]["outputs"]["selected"], "${{ steps.impact.outputs.rust }}")

        for work_job in ("rust-vmafx-sys-work", "cargo-deny-work"):
            with self.subTest(work_job=work_job):
                self.assertEqual(jobs[work_job]["needs"], "impact")
                self.assertIn("needs.impact.outputs.selected == 'true'", jobs[work_job]["if"])

        gates = {
            "vmafx-sys-gate": ("vmafx-sys CI", "rust-vmafx-sys-work"),
            "cargo-deny-gate": ("cargo-deny", "cargo-deny-work"),
        }
        for gate_job, (check_name, work_job) in gates.items():
            with self.subTest(gate_job=gate_job):
                gate = jobs[gate_job]
                self.assertEqual(gate["name"], check_name)
                self.assertEqual(gate["needs"], ["impact", work_job])
                self.assertEqual(gate["if"], "always()")
                script = gate["steps"][0]["run"]
                self.assertIn('if [ "$PLAN_RESULT" != success ]', script)
                self.assertIn("true:success|false:skipped", script)

    def test_ci_impact_json_rust_selector_includes_libvmaf_headers(self) -> None:
        self.assertTrue(CONFIG.exists(), f"missing {CONFIG}")
        config = json.loads(CONFIG.read_text(encoding="utf-8"))
        rust_patterns = config.get("selectors", {}).get("rust", {}).get("patterns", [])
        self.assertIn(
            "core/include/libvmaf/**",
            rust_patterns,
            "core/include/libvmaf/** missing from selectors.rust.patterns in ci-impact.json",
        )


if __name__ == "__main__":
    unittest.main()
