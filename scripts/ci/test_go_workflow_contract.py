#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Exercise the real required-check script with Go outcomes and routing guards."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
GO_CHECK = "go vet + go test"


class GoWorkflowContract(unittest.TestCase):
    def test_ready_pr_and_master_runs_are_routed_inside_the_job(self):
        workflow = (WORKFLOWS / "go-ci.yml").read_text(encoding="utf-8")
        trigger = workflow.split("\nconcurrency:", 1)[0]
        self.assertNotRegex(trigger, r"(?m)^\s+paths(?:-ignore)?:")
        self.assertIn("types: [opened, synchronize, reopened, ready_for_review]", trigger)
        self.assertIn("branches: [master]", trigger)
        self.assertIn("github.event.pull_request.draft == false", workflow)
        self.assertIn("fetch-depth: 0", workflow)
        self.assertIn("name: " + GO_CHECK, workflow)
        self.assertIn("# required-aggregator", workflow)
        self.assertIn('--base "$BASE_SHA" --head "$HEAD_SHA"', workflow)
        steps = workflow.split("\n      - ")[1:]
        heavy = [
            step
            for step in steps
            if not step.startswith(
                ("uses: actions/checkout@", "name: Plan CI", "name: Go checks not impacted")
            )
        ]
        self.assertGreater(len(heavy), 8)
        for step in heavy:
            self.assertIn("if: steps.impact.outputs.go_checks == 'true'", step)
            self.assertNotIn("continue-on-error", step)
        self.assertIn("if: steps.impact.outputs.go_checks != 'true'", workflow)

    def _aggregate(self, go_conclusion: str | None) -> list[str]:
        text = (WORKFLOWS / "required-aggregator.yml").read_text(encoding="utf-8")
        script = textwrap.dedent(text.split("          script: |\n", 1)[1])
        required_block = re.search(r"const required = \[(.*?)\];", script, re.DOTALL)
        self.assertIsNotNone(required_block)
        names = re.findall(r"'([^']+)'", required_block.group(1))
        self.assertIn(GO_CHECK, names)
        checks = [
            {"name": name, "conclusion": go_conclusion if name == GO_CHECK else "success"}
            for name in names
            if name != GO_CHECK or go_conclusion is not None
        ]
        node = shutil.which("node")
        self.assertIsNotNone(node, "Node.js is needed to exercise the Actions JavaScript")
        driver = r"""
const fs = require('fs');
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
const now = Date.now();
let clock = now;
class VirtualDate extends Date { static now() { clock += 180000; return clock; } }
const checks = input.checks.map(c => ({...c, status: 'completed', started_at: new Date(now).toISOString()}));
const failures = [];
const github = {rest: {
  actions: {getWorkflowRun: async () => ({data: {created_at: new Date(now).toISOString()}})},
  checks: {listForRef: async () => ({data: {check_runs: checks}})},
}};
const context = {eventName: 'pull_request', repo: {owner: 'test', repo: 'test'},
  payload: {pull_request: {head: {ref: 'fix/example', sha: 'abc'}}}};
const core = {info: () => {}, setFailed: message => failures.push(message)};
const AsyncFunction = Object.getPrototypeOf(async function(){}).constructor;
new AsyncFunction('github', 'context', 'core', 'process', 'Date', 'setTimeout', input.script)(
  github, context, core, {env: {GITHUB_RUN_ID: '1'}}, VirtualDate, callback => callback()
).then(() => process.stdout.write(JSON.stringify(failures))).catch(error => {console.error(error); process.exitCode = 1;});
"""
        result = subprocess.run(  # noqa: S603 -- fixed Node driver, repository-owned workflow input
            [node, "-e", driver],
            input=json.dumps({"script": script, "checks": checks}),
            text=True,
            capture_output=True,
            check=True,
            timeout=10,
        )
        return json.loads(result.stdout)

    def test_failed_go_scan_blocks_otherwise_green_checks(self):
        failures = self._aggregate("failure")
        self.assertEqual(len(failures), 1)
        self.assertIn(GO_CHECK + ": failure", failures[0])

    def test_successful_go_checks_pass(self):
        self.assertEqual(self._aggregate("success"), [])

    def test_unreported_check_keeps_existing_absence_semantics(self):
        self.assertEqual(self._aggregate(None), [])


if __name__ == "__main__":
    unittest.main()
