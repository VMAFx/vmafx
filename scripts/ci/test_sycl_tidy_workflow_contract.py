#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Enforce non-advisory required gate contract for SYCL clang-tidy (T-SYCL-CLANG-TIDY-DISABLED).

Validates:
1. .github/workflows/lint-and-format.yml declares job `clang-tidy-sycl` with name
   `Tidy SYCL`, marked `# required-aggregator`, without `continue-on-error` or
   `if: false`, covering all SYCL headers (.h, .hpp) and sources (.cpp) alongside
   tests, and failing closed on any tidy diagnostic.
2. .github/workflows/required-aggregator.yml registers exact check name `Tidy SYCL`
   (and explicitly not any advisory variant).
3. The real Node.js aggregator execution fails closed when `Tidy SYCL` reports
   failure, and passes when it succeeds or legitimately skips on unimpacted diffs.
4. Mutation tests prove the assertions are red-capable against relaxations.
"""

from __future__ import annotations

import json
import re
import shutil
import sys
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

sys.path.insert(0, str(REPO_ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

SUBPROCESS_TIMEOUT_S = 120
SYCL_CHECK_NAME = "Tidy SYCL"
SYCL_JOB_ID = "clang-tidy-sycl"

EXPECTED_FILE_PATTERNS = (
    "core/src/sycl/*.cpp",
    "core/src/sycl/*.hpp",
    "core/src/sycl/*.h",
    "core/src/feature/sycl/*.cpp",
    "core/src/feature/sycl/*.hpp",
    "core/src/feature/sycl/*.h",
    "core/test/test_sycl*.c",
    "core/test/test_sycl*.cpp",
    "core/test/test_integer_cambi_sycl.c",
)


def extract_job_block(workflow_text: str, job_id: str) -> str:
    """Extract a top-level job definition block from workflow YAML."""
    match = re.search(
        rf"(?ms)^  {re.escape(job_id)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        workflow_text,
    )
    if match is None:
        raise AssertionError(f"Job '{job_id}' not found in workflow")
    return match.group(0)


def validate_sycl_workflow_structure(workflow_text: str) -> None:
    """Validate that the SYCL clang-tidy job in lint-and-format.yml is a non-advisory gate."""
    job_block = extract_job_block(workflow_text, SYCL_JOB_ID)

    if not re.search(r"^\s*#\s*required-aggregator\s*$", job_block, re.MULTILINE):
        raise AssertionError(
            f"Job '{SYCL_JOB_ID}' must have '# required-aggregator' marker comment"
        )

    name_match = re.search(r"^\s*name:\s*(.+)$", job_block, re.MULTILINE)
    if not name_match:
        raise AssertionError(f"Job '{SYCL_JOB_ID}' missing name field")
    declared_name = name_match.group(1).strip()
    if declared_name != SYCL_CHECK_NAME:
        raise AssertionError(
            f"Job '{SYCL_JOB_ID}' must have exact 'name: {SYCL_CHECK_NAME}', found '{declared_name}'"
        )

    # Ensure no continue-on-error anywhere in the job or its steps
    for line in job_block.splitlines():
        stripped = line.strip()
        if stripped.startswith("continue-on-error:"):
            raise AssertionError(f"Job '{SYCL_JOB_ID}' must not contain continue-on-error: {line}")
        if stripped == "if: false":
            raise AssertionError(f"Job '{SYCL_JOB_ID}' must not be disabled with 'if: false'")

    # Ensure all required SYCL source and header file patterns are tracked in detect step
    for pattern in EXPECTED_FILE_PATTERNS:
        quoted = f"'{pattern}'"
        if quoted not in job_block:
            raise AssertionError(
                f"Job '{SYCL_JOB_ID}' detect step missing expected pattern: {quoted}"
            )

    # Ensure fail-closed exit check is present in execution step
    if "scripts/ci/clang-tidy-sycl.sh" not in job_block:
        raise AssertionError(f"Job '{SYCL_JOB_ID}' must invoke 'scripts/ci/clang-tidy-sycl.sh'")
    if "[ -f /tmp/tidy-sycl.fail ] && exit 1 || exit 0" not in job_block:
        raise AssertionError(f"Job '{SYCL_JOB_ID}' must check /tmp/tidy-sycl.fail and exit 1")


def validate_sycl_aggregator_declaration(aggregator_text: str) -> None:
    """Validate that required-aggregator.yml registers Tidy SYCL as a required check."""
    script_match = re.search(r"const required = \[(.*?)\];", aggregator_text, re.DOTALL)
    if script_match is None:
        raise AssertionError("required-aggregator.yml must declare 'const required = [...]'")

    names = re.findall(r"'([^']+)'", script_match.group(1))
    if SYCL_CHECK_NAME not in names:
        raise AssertionError(
            f"required-aggregator.yml must declare '{SYCL_CHECK_NAME}' in required array"
        )

    if "Tidy SYCL (advisory)" in names:
        raise AssertionError("required-aggregator.yml must not contain 'Tidy SYCL (advisory)'")


class SyclTidyWorkflowContractTest(unittest.TestCase):
    """Contract assertions and mutation tests for SYCL clang-tidy gate promotion."""

    def test_lint_and_format_workflow_structure(self) -> None:
        workflow_text = (WORKFLOWS_DIR / "lint-and-format.yml").read_text(encoding="utf-8")
        validate_sycl_workflow_structure(workflow_text)

    def test_required_aggregator_declaration(self) -> None:
        aggregator_text = (WORKFLOWS_DIR / "required-aggregator.yml").read_text(encoding="utf-8")
        validate_sycl_aggregator_declaration(aggregator_text)

    def _aggregate(self, sycl_conclusion: str | None) -> list[str]:
        text = (WORKFLOWS_DIR / "required-aggregator.yml").read_text(encoding="utf-8")
        script = textwrap.dedent(text.split("          script: |\n", 1)[1])
        required_block = re.search(r"const required = \[(.*?)\];", script, re.DOTALL)
        if required_block is None:
            self.fail("required aggregator must declare its check list")
        names = re.findall(r"'([^']+)'", required_block.group(1))
        self.assertIn(SYCL_CHECK_NAME, names)
        checks = [
            {"name": name, "conclusion": sycl_conclusion if name == SYCL_CHECK_NAME else "success"}
            for name in names
            if name != SYCL_CHECK_NAME or sycl_conclusion is not None
        ]
        node = shutil.which("node")
        if node is None:
            self.fail("Node.js is needed to exercise the Actions JavaScript")
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
        result = run_command(
            [node, "-e", driver],
            allowed_executables=(node,),
            input_data=json.dumps({"script": script, "checks": checks}),
            text=True,
            capture_output=True,
            check=True,
            timeout_seconds=SUBPROCESS_TIMEOUT_S,
        )
        payload: object = json.loads(result.stdout)
        if not isinstance(payload, list):
            self.fail("aggregator driver must return a list of failure messages")
        failures: list[str] = []
        for message in payload:
            if not isinstance(message, str):
                self.fail("aggregator failure messages must be strings")
            failures.append(message)
        return failures

    def test_failed_sycl_tidy_blocks_aggregator(self) -> None:
        failures = self._aggregate("failure")
        self.assertEqual(len(failures), 1)
        self.assertIn(f"{SYCL_CHECK_NAME}: failure", failures[0])

    def test_successful_sycl_tidy_passes_aggregator(self) -> None:
        self.assertEqual(self._aggregate("success"), [])

    def test_unreported_sycl_tidy_preserves_path_skip_semantics(self) -> None:
        # SYCL tidy is path-filtered; an unimpacted PR does not run it and aggregator passes
        self.assertEqual(self._aggregate(None), [])

    # Red mutation tests proving contract sensitivity
    def test_mutation_continue_on_error_fails_contract(self) -> None:
        workflow_text = (WORKFLOWS_DIR / "lint-and-format.yml").read_text(encoding="utf-8")
        mutated = workflow_text.replace(
            f"name: {SYCL_CHECK_NAME}\n",
            f"name: {SYCL_CHECK_NAME}\n    continue-on-error: true\n",
        )
        with self.assertRaises(AssertionError) as ctx:
            validate_sycl_workflow_structure(mutated)
        self.assertIn("continue-on-error", str(ctx.exception))

    def test_mutation_advisory_name_fails_contract(self) -> None:
        workflow_text = (WORKFLOWS_DIR / "lint-and-format.yml").read_text(encoding="utf-8")
        mutated = workflow_text.replace(
            f"name: {SYCL_CHECK_NAME}\n",
            f"name: {SYCL_CHECK_NAME} (advisory)\n",
        )
        with self.assertRaises(AssertionError) as ctx:
            validate_sycl_workflow_structure(mutated)
        self.assertIn("exact 'name: Tidy SYCL'", str(ctx.exception))

    def test_mutation_missing_marker_comment_fails_contract(self) -> None:
        workflow_text = (WORKFLOWS_DIR / "lint-and-format.yml").read_text(encoding="utf-8")
        mutated = workflow_text.replace("    # required-aggregator\n", "")
        with self.assertRaises(AssertionError) as ctx:
            validate_sycl_workflow_structure(mutated)
        self.assertIn("# required-aggregator", str(ctx.exception))

    def test_mutation_missing_header_pattern_fails_contract(self) -> None:
        workflow_text = (WORKFLOWS_DIR / "lint-and-format.yml").read_text(encoding="utf-8")
        mutated = workflow_text.replace("'core/src/sycl/*.h' ", "")
        with self.assertRaises(AssertionError) as ctx:
            validate_sycl_workflow_structure(mutated)
        self.assertIn("missing expected pattern", str(ctx.exception))

    def test_mutation_omitted_from_aggregator_fails_contract(self) -> None:
        aggregator_text = (WORKFLOWS_DIR / "required-aggregator.yml").read_text(encoding="utf-8")
        mutated = aggregator_text.replace(f"'{SYCL_CHECK_NAME}',", "")
        with self.assertRaises(AssertionError) as ctx:
            validate_sycl_aggregator_declaration(mutated)
        self.assertIn(f"must declare '{SYCL_CHECK_NAME}' in required array", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
