#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the actual aggregator plus publisher/PR provenance boundaries."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github/workflows"


class ScorecardWorkflowTests(unittest.TestCase):
    def test_publisher_preserves_upstream_authenticity_restrictions(self) -> None:
        workflow = (WORKFLOWS / "scorecard.yml").read_text()
        prefix, jobs = workflow.split("\njobs:", 1)
        self.assertNotRegex(prefix, r"(?m)^(env|defaults):")
        self.assertIn("permissions: read-all", prefix)
        self.assertIn("branches: [master]", prefix)
        self.assertIn('cron: "19 4 * * 1"', prefix)
        analysis, gate = jobs.split("\n  gate:", 1)
        self.assertNotRegex(analysis, r"(?m)^    (env|defaults|container|services):")
        self.assertIn("runs-on: ubuntu-latest", analysis)
        self.assertIn("id-token: write", analysis)
        self.assertNotIn("id-token: write", gate)
        self.assertNotRegex(analysis, r"(?m)^\s+run:")
        actions = re.findall(r"uses: ([^@\s]+)@([^\s]+)", analysis)
        allowed = {
            "actions/checkout",
            "actions/upload-artifact",
            "github/codeql-action/upload-sarif",
            "ossf/scorecard-action",
        }
        self.assertEqual({name for name, _ in actions}, allowed)
        for _, pin in actions:
            self.assertRegex(pin, r"^[0-9a-f]{40}$")
        self.assertIn("persist-credentials: false", analysis)
        self.assertIn("results_format: sarif", analysis)
        self.assertIn("publish_results: true", analysis)
        self.assertIn("            results.json", analysis)
        self.assertIn("            results.sarif", analysis)
        self.assertIn("if-no-files-found: error", analysis)

    def test_master_uses_only_successful_same_run_artifact_and_exact_head(self) -> None:
        workflow = (WORKFLOWS / "scorecard.yml").read_text()
        gate = workflow.split("\n  gate:", 1)[1]
        self.assertIn("needs: analysis", gate)
        self.assertIn("if: always()", gate)
        self.assertIn('run: test "$ANALYSIS_RESULT" = success', gate)
        self.assertIn("ANALYSIS_RESULT: ${{ needs.analysis.result }}", gate)
        self.assertIn("ref: ${{ github.sha }}", gate)
        self.assertIn("EXPECTED_SHA: ${{ github.sha }}", gate)
        self.assertIn("EXPECTED_REPOSITORY: ${{ github.repository }}", gate)
        self.assertEqual(
            workflow.count(
                "name: scorecard-${{ github.run_id }}-${{ github.run_attempt }}-${{ github.sha }}"
            ),
            2,
        )
        self.assertIn("scorecard_gate.py master", gate)
        self.assertIn("digest-mismatch: error", gate)
        self.assertIn("repos/$EXPECTED_REPOSITORY/git/ref/heads/master", gate)
        self.assertIn('--master-ref "$RUNNER_TEMP/scorecard-master/final-master-ref.json"', gate)
        self.assertIn("GH_TOKEN: ${{ github.token }}", gate)
        self.assertNotIn("api.scorecard.dev", gate)
        self.assertNotIn("run-id:", gate)  # download-artifact must default to this run
        self.assertNotIn("continue-on-error", gate)

    def test_pr_runs_unprivileged_actual_head_without_waiting_for_master(self) -> None:
        workflow = (WORKFLOWS / "scorecard-policy.yml").read_text()
        self.assertIn("types: [opened, synchronize, reopened, ready_for_review]", workflow)
        self.assertIn("branches: [master]", workflow)
        self.assertNotRegex(workflow, r"(?m)^\s+paths(?:-ignore)?:")
        self.assertNotIn("pull_request_target", workflow)
        self.assertNotIn(": write", workflow)
        self.assertNotIn("secrets.", workflow)
        self.assertNotIn("needs:", workflow)
        self.assertNotIn("download-artifact", workflow)
        self.assertIn('run: test "$IS_DRAFT" = false', workflow)
        self.assertIn("ref: ${{ github.event.pull_request.head.sha }}", workflow)
        self.assertIn("persist-credentials: false", workflow)
        self.assertIn("publish_results: false", workflow)
        self.assertIn("results_format: json", workflow)
        self.assertIn("scorecard_gate.py snapshot", workflow)
        self.assertIn("scorecard_gate.py local", workflow)
        self.assertIn('--snapshot "$RUNNER_TEMP/scorecard-source.json"', workflow)
        self.assertNotIn("continue-on-error", workflow)
        snapshot = workflow.index("scorecard_gate.py snapshot")
        scan = workflow.index("uses: ossf/scorecard-action@")
        validate = workflow.index("scorecard_gate.py local")
        self.assertLess(snapshot, scan)
        self.assertLess(scan, validate)

    def test_contracts_are_wired_into_local_and_remote_gates(self) -> None:
        hooks = (ROOT / ".pre-commit-config.yaml").read_text()
        hook = hooks.split("- id: scorecard-policy-contract", 1)[1].split("\n      - id:", 1)[0]
        self.assertIn("test_scorecard_*.py", hook)
        self.assertIn("stages: [pre-commit, pre-push]", hook)
        self.assertIn("required-aggregator", hook)
        self.assertIn("scorecard_gate", hook)
        self.assertIn("test_scorecard_*.py", (WORKFLOWS / "scorecard-policy.yml").read_text())

    def test_repository_protection_controls_are_mandatory_and_separate(self) -> None:
        publisher, gate = (WORKFLOWS / "scorecard.yml").read_text().split("\n  gate:", 1)
        pr = (WORKFLOWS / "scorecard-policy.yml").read_text()
        command = "python3 -B scripts/dev/tests/test_repository_security.py -v"
        self.assertIn(command, pr)
        self.assertIn(command, gate)
        self.assertLess(pr.index(command), pr.index("scorecard_gate.py snapshot"))
        self.assertNotIn("check_repository_security.py", publisher)
        live = gate.split("- name: Verify live repository protection", 1)[1].split(
            "- name: Preserve gate receipt", 1
        )[0]
        self.assertIn("if: ${{ !cancelled() }}", live)
        self.assertIn("GH_TOKEN: ${{ github.token }}", live)
        self.assertIn("python3 -B scripts/dev/check_repository_security.py", live)
        self.assertIn('--report "$RUNNER_TEMP/scorecard-master/repository-security.json"', live)
        self.assertNotIn("continue-on-error", live)
        self.assertNotIn("||", live)
        hooks = (ROOT / ".pre-commit-config.yaml").read_text()
        hook = hooks.split("- id: repository-security-contract", 1)[1].split("\n      - id:", 1)[0]
        self.assertIn("entry: python3 -B scripts/dev/tests/test_repository_security.py", hook)
        self.assertIn("stages: [pre-commit, pre-push]", hook)
        for dependency in [
            "check_repository_security",
            "test_repository_security",
            "repository-security-policy",
            "scorecard",
        ]:
            self.assertIn(dependency, hook)

    def aggregate(self, event: str, conclusion: str | None, inactive: str = "failure") -> list[str]:
        workflow = (WORKFLOWS / "required-aggregator.yml").read_text()
        script = textwrap.dedent(workflow.split("          script: |\n", 1)[1])
        block = re.search(r"const required = \[(.*?)\];", script, re.DOTALL)
        self.assertIsNotNone(block)
        assert block is not None
        names = re.findall(r"'([^']+)'", block.group(1))
        expected = "Scorecard PR Gate" if event == "pull_request" else "Scorecard Master Gate"
        other = "Scorecard Master Gate" if event == "pull_request" else "Scorecard PR Gate"
        self.assertIn(expected, names)
        self.assertIn(other, names)
        checks = [
            {
                "name": name,
                "conclusion": (
                    conclusion if name == expected else inactive if name == other else "success"
                ),
            }
            for name in names
            if name != expected or conclusion is not None
        ]
        node = shutil.which("node")
        self.assertIsNotNone(node, "Node is required for actual Actions-script controls")
        assert node is not None
        driver = r"""
const input = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const now = Date.now(); let clock = now;
class VirtualDate extends Date { static now() { clock += 180000; return clock; } }
const failures = [];
const checks = input.checks.map(c => ({...c, status: 'completed', started_at: new Date(now).toISOString()}));
const github = {rest: {
 actions: {getWorkflowRun: async () => ({data: {created_at: new Date(now).toISOString()}})},
 checks: {listForRef: async () => ({data: {check_runs: checks}})}
}};
const context = {eventName: input.event, sha: 'abc', repo: {owner:'test',repo:'test'},
 payload: {pull_request: {head:{ref:'fix/example',sha:'abc'}}}};
const core = {info: () => {}, setFailed: message => failures.push(message)};
const AsyncFunction = Object.getPrototypeOf(async function(){}).constructor;
new AsyncFunction('github','context','core','process','Date','setTimeout',input.script)(
 github,context,core,{env:{GITHUB_RUN_ID:'1'}},VirtualDate,callback=>callback()
).then(()=>process.stdout.write(JSON.stringify(failures))).catch(e=>{console.error(e);process.exitCode=1;});
"""
        result = subprocess.run(  # noqa: S603 -- ADR-1247: fixed Node driver and repository workflow fixture
            [node, "-e", driver],
            input=json.dumps({"script": script, "event": event, "checks": checks}),
            text=True,
            capture_output=True,
            check=True,
            timeout=10,
        )
        messages: object = json.loads(result.stdout)
        if not isinstance(messages, list) or not all(isinstance(m, str) for m in messages):
            self.fail("aggregator driver returned malformed failures")
        return [str(m) for m in messages]

    def test_only_success_of_applicable_context_satisfies_aggregator(self) -> None:
        for event in ["pull_request", "push"]:
            for conclusion in [None, "skipped", "neutral", "failure", "cancelled", "timed_out"]:
                with self.subTest(event=event, conclusion=conclusion):
                    self.assertTrue(self.aggregate(event, conclusion))
            self.assertEqual(self.aggregate(event, "success"), [])
            # The other event's failed scope cannot create a cross-event deadlock.
            self.assertEqual(self.aggregate(event, "success", inactive="cancelled"), [])


if __name__ == "__main__":
    unittest.main()
