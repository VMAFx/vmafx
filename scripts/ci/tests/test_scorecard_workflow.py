#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the actual aggregator plus publisher/PR provenance boundaries."""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from typing import Any, cast

import yaml  # type: ignore[import-untyped]

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import run as run_command

# A hang detector, not a timing assertion: these subprocesses finish in tens of
# milliseconds locally, but a loaded CI runner has blown a 10-second cap and the
# TimeoutExpired then reads as a real test failure (bug ledger L-76). 120s still
# catches a genuine hang long before the job's own timeout.
SUBPROCESS_TIMEOUT_S = 120

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github/workflows"
GATE_SCRIPT = ROOT / "scripts/ci/scorecard_gate.py"


def scorecard_pr_gate_job(workflow: str) -> str:
    """The text of the `scorecard-pr-gate` job, up to the next job or the end."""
    job = workflow.split("\n  scorecard-pr-gate:\n", 1)[1]
    return re.split(r"(?m)^  [a-z0-9_-]+:\n", job, maxsplit=1)[0]


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
        self.assertIn("runs-on: ubuntu-26.04", analysis)
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
        # A failed analysis still reaches the gate, which then fails; a cancelled run
        # does not (ADR-1686: GitHub keeps running a job whose `if` stays true).
        self.assertIn("\n    if: ${{ !cancelled() }}\n", gate)
        self.assertNotIn("\n    if: always()\n", gate)
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
        # The PR gate is a job of the Security workflow since the suite cut: the
        # triggers are the workflow's, the rest is the job's own text.
        host = (WORKFLOWS / "security-scans.yml").read_text()
        self.assertIn("types: [opened, synchronize, reopened, ready_for_review]", host)
        self.assertIn("branches: [master]", host)
        self.assertNotRegex(host, r"(?m)^\s+paths(?:-ignore)?:")
        self.assertNotIn("pull_request_target", host)
        workflow = scorecard_pr_gate_job(host)
        self.assertNotIn(": write", workflow)
        self.assertNotIn("secrets.", workflow)
        # ADR-2169: the only dependency is the tier decision, never the master aggregate.
        self.assertEqual(re.findall(r"(?m)^\s+needs:.*$", workflow), ["    needs: tier"])
        self.assertNotIn("download-artifact", workflow)
        self.assertIn('run: test "$IS_DRAFT" = false', workflow)
        self.assertIn(
            "if: ${{ always() && github.event.pull_request.draft == false }}",
            workflow,
        )
        self.assertIn("if-no-files-found: error", workflow)
        self.assertNotIn("        if: always()\n", workflow)
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
        # A test runs once in CI (ADR-1568): Tooling Tests runs these files.
        from scripts.ci.suite_registry import suite_members  # noqa: PLC0415

        tooling = suite_members(ROOT, "tooling")
        for name in ("test_scorecard_gate.py", "test_scorecard_workflow.py"):
            self.assertIn(f"scripts/ci/tests/{name}", tooling)

    def test_repository_protection_controls_are_mandatory_and_separate(self) -> None:
        publisher, gate = (WORKFLOWS / "scorecard.yml").read_text().split("\n  gate:", 1)
        pr = scorecard_pr_gate_job((WORKFLOWS / "security-scans.yml").read_text())
        # The policy's tests run once in CI, in Tooling Tests (ADR-1568), on
        # every pull request and push, not inside either Scorecard job.
        from scripts.ci.suite_registry import suite_members  # noqa: PLC0415

        self.assertIn(
            "scripts/dev/tests/test_repository_security.py", suite_members(ROOT, "tooling")
        )
        self.assertNotIn("test_repository_security.py", pr)
        self.assertNotIn("test_repository_security.py", gate)
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
 actions: {getWorkflowRun: async () => ({data: {created_at: new Date(now).toISOString()}}),
  listWorkflowRunsForRepo: async () => ({data: {workflow_runs: []}})},
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
        result = run_command(
            [node, "-e", driver],
            allowed_executables=(node,),
            input_data=json.dumps({"script": script, "event": event, "checks": checks}),
            text=True,
            capture_output=True,
            check=True,
            timeout_seconds=SUBPROCESS_TIMEOUT_S,
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

    def test_dependency_review_allows_text_unidecode_package_wide(self) -> None:
        security_scans = (WORKFLOWS / "security-scans.yml").read_text(encoding="utf-8")
        review_block = security_scans.split("dependency-review:", 1)[1]
        self.assertIn("deny-licenses: GPL-3.0, AGPL-3.0", review_block)
        # Ensure allow-dependencies-licenses uses exact package-wide purl (no version)
        self.assertIn("allow-dependencies-licenses: pkg:pypi/text-unidecode", review_block)
        # Must not include version in purl since GitHub Dependency Review ignores version in purl matching
        self.assertNotIn("pkg:pypi/text-unidecode@", review_block)


GATE_STEP = "Enforce exact-head full repository policy"
RUN_ID = "4242"
# The superseded step's wait for its own cancellation stays well inside the gate
# job's 5-minute timeout, so a cancel that never lands still ends the step red.
WAIT_CEILING_SECONDS = 180
# Stubs for the policy step's commands: each records its call; python3 plays the
# gate's exit status, sleep returns at once so the bounded wait is counted, not waited.
STUBS = {
    "gh": 'printf "%s\\n" "$*" >> "$STUB_DIR/gh-calls"\nprintf "{}\\n"\n',
    "python3": 'printf "%s\\n" "$*" >> "$STUB_DIR/python-calls"\nexit "$STUB_GATE_STATUS"\n',
    "sleep": 'printf "%s\\n" "$1" >> "$STUB_DIR/sleeps"\n',
}


def scorecard_gate_job() -> dict[str, Any]:
    workflow = yaml.safe_load((WORKFLOWS / "scorecard.yml").read_text(encoding="utf-8"))
    return cast(dict[str, Any], workflow["jobs"]["gate"])


def gate_step(name: str) -> dict[str, Any]:
    matches = [step for step in scorecard_gate_job()["steps"] if step.get("name") == name]
    if len(matches) != 1:
        raise AssertionError(f"scorecard.yml gate job has {len(matches)} steps named {name!r}")
    return cast(dict[str, Any], matches[0])


class SupersededMasterRunTests(unittest.TestCase):
    """ADR-1686: a run whose master moved on to a descendant ends cancelled."""

    def test_gate_job_is_cancellable_and_may_cancel_its_own_run(self) -> None:
        job = scorecard_gate_job()
        self.assertEqual(job["if"], "${{ !cancelled() }}")
        self.assertEqual(job["permissions"], {"contents": "read", "actions": "write"})
        gate_text = (WORKFLOWS / "scorecard.yml").read_text().split("\n  gate:", 1)[1]
        permissions = gate_text.split("    permissions:\n", 1)[1].split("    steps:", 1)[0]
        reason = permissions.split("      actions: write", 1)[0]
        self.assertIn("cancel", reason)
        self.assertIn("ADR-1686", reason)
        step = gate_step(GATE_STEP)
        self.assertEqual(step["env"]["RUN_ID"], "${{ github.run_id }}")
        self.assertNotIn("if", step)
        self.assertNotIn("continue-on-error", step)

    def test_receipt_and_live_check_conditions_survive_a_cancelled_run(self) -> None:
        self.assertEqual(
            gate_step("Verify live repository protection")["if"], "${{ !cancelled() }}"
        )
        self.assertEqual(gate_step("Preserve gate receipt")["if"], "always()")

    @unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "the step is a bash script")
    def test_only_the_superseded_exit_cancels_the_run(self) -> None:
        for status in (0, 1, 2, 4):
            with self.subTest(status=status):
                code, calls, output = self.run_policy_step(status)
                self.assertEqual(code, status, output)
                self.assertEqual(len(calls["gh"]), 1)
                self.assertIn("git/ref/heads/master", calls["gh"][0])
                self.assertEqual(calls["sleeps"], [])

    @unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "the step is a bash script")
    def test_superseded_exit_cancels_then_fails_if_the_cancel_never_lands(self) -> None:
        spec = importlib.util.spec_from_file_location("scorecard_gate", GATE_SCRIPT)
        assert spec and spec.loader
        gate = importlib.util.module_from_spec(spec)
        sys.modules.setdefault(spec.name, gate)
        spec.loader.exec_module(gate)
        code, calls, output = self.run_policy_step(gate.SUPERSEDED_EXIT)
        self.assertEqual(len(calls["python"]), 1)
        self.assertIn("--master-compare ", calls["python"][0])
        self.assertEqual(len(calls["gh"]), 2)
        self.assertEqual(
            calls["gh"][1].split(),
            [
                "api",
                "--hostname",
                "github.com",
                "--method",
                "POST",
                f"repos/VMAFx/vmafx/actions/runs/{RUN_ID}/cancel",
            ],
        )
        # The wait is bounded; a run the cancel never reached is red, never green.
        waited = sum(int(seconds) for seconds in calls["sleeps"])
        self.assertTrue(0 < waited <= WAIT_CEILING_SECONDS, calls["sleeps"])
        self.assertEqual(code, 1)
        self.assertIn(f"::error::Run {RUN_ID} was not cancelled", output)

    def run_policy_step(self, gate_status: int) -> tuple[int, dict[str, list[str]], str]:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "runner/scorecard-master").mkdir(parents=True)
            (root / "bin").mkdir()
            for name, body in STUBS.items():
                stub = root / "bin" / name
                stub.write_text("#!/usr/bin/env bash\n" + body, encoding="utf-8")
                stub.chmod(0o755)
            env = {
                "PATH": f"{root / 'bin'}{os.pathsep}{os.environ['PATH']}",
                "HOME": str(root),
                "STUB_DIR": str(root),
                "STUB_GATE_STATUS": str(gate_status),
                "RUNNER_TEMP": str(root / "runner"),
                "GITHUB_STEP_SUMMARY": str(root / "summary.md"),
                "EXPECTED_SHA": "b" * 40,
                "EXPECTED_REPOSITORY": "VMAFx/vmafx",
                "GH_TOKEN": "stub-token",
                "RUN_ID": RUN_ID,
            }
            bash = shutil.which("bash")
            assert bash is not None
            result = run_command(
                [bash, "-e", "-c", gate_step(GATE_STEP)["run"]],
                allowed_executables=(bash,),
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                check=False,
                timeout_seconds=SUBPROCESS_TIMEOUT_S,
            )
            calls = {
                name: (
                    (root / f"{name}-calls").read_text().splitlines()
                    if (root / f"{name}-calls").exists()
                    else []
                )
                for name in ("gh", "python")
            }
            sleeps = root / "sleeps"
            calls["sleeps"] = sleeps.read_text().splitlines() if sleeps.exists() else []
            return result.returncode, calls, f"{result.stdout}{result.stderr}"


if __name__ == "__main__":
    unittest.main()
