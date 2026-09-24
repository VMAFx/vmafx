#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Enforce non-advisory required gate contract for SYCL clang-tidy (T-SYCL-CLANG-TIDY-DISABLED).

Validates:
1. .github/workflows/lint-and-format.yml declares job `clang-tidy-sycl` with name
   `Tidy SYCL`, marked `# required-aggregator`, without `continue-on-error` or
   any normalized constant-false job guard, covering all SYCL headers (.h, .hpp)
   and sources (.cpp) alongside tests, and failing closed on any tidy diagnostic.
2. .github/workflows/required-aggregator.yml registers exact check name `Tidy SYCL`
   in both `required` and `strictMustReport` (and not any advisory variant).
3. The real Node.js aggregator execution fails closed when `Tidy SYCL` reports
   failure, skip, or absence, and passes only when it reports success.
4. Mutation tests prove the assertions are red-capable against relaxations.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

sys.path.insert(0, str(REPO_ROOT))

from scripts.ci.required_aggregator_harness import run_required_aggregator  # noqa: E402

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

EVENT_SELECTION_ANCHORS = (
    ("pull request", '"origin/${GH_BASE_REF}...HEAD"'),
    ("push fallback", "HEAD~1..HEAD"),
    ("push", '"$before"..HEAD'),
    ("dispatch", "files=$(git ls-files"),
)

CONSTANT_FALSE_EXPRESSIONS = {
    "false",
    "null",
    "0",
    "-0",
    "0.0",
    "-0.0",
    "''",
    '""',
    "!true",
    "!1",
}


def extract_job_block(workflow_text: str, job_id: str) -> str:
    """Extract a top-level job definition block from workflow YAML."""
    match = re.search(
        rf"(?ms)^  {re.escape(job_id)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        workflow_text,
    )
    if match is None:
        raise AssertionError(f"Job '{job_id}' not found in workflow")
    return match.group(0)


def _strip_balanced_outer_parentheses(expression: str) -> str:
    """Remove parentheses that wrap an entire Actions expression."""
    value = expression.strip()
    while value.startswith("(") and value.endswith(")"):
        depth = 0
        wraps_entire_value = True
        for index, character in enumerate(value):
            if character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0 and index != len(value) - 1:
                    wraps_entire_value = False
                    break
            if depth < 0:
                wraps_entire_value = False
                break
        if depth != 0 or not wraps_entire_value:
            break
        value = value[1:-1].strip()
    return value


def is_constant_false_condition(raw_condition: str) -> bool:
    """Recognize GitHub Actions literal-false job guards after normalizing wrappers."""
    expression = raw_condition.split(" #", 1)[0].strip()
    if expression.startswith("${{") and expression.endswith("}}"):
        expression = expression[3:-2].strip()
    expression = _strip_balanced_outer_parentheses(expression)
    compact = re.sub(r"\s+", "", expression).casefold()
    return compact in CONSTANT_FALSE_EXPRESSIONS


def extract_file_selection(job_block: str, branch_name: str, anchor: str) -> str:
    """Return the one git file-selection command identified by its event-branch anchor."""
    lines = job_block.splitlines()
    anchor_lines = [index for index, line in enumerate(lines) if anchor in line]
    if len(anchor_lines) != 1:
        raise AssertionError(
            f"Job '{SYCL_JOB_ID}' must have exactly one {branch_name} selection anchor: {anchor}"
        )

    anchor_line = anchor_lines[0]
    start = anchor_line
    while start >= 0 and "files=$(git" not in lines[start]:
        start -= 1
    if start < 0:
        raise AssertionError(f"Job '{SYCL_JOB_ID}' {branch_name} selection has no git command")

    end = anchor_line
    while end < len(lines) and "| tr '\\n' ' ')" not in lines[end]:
        end += 1
    if end == len(lines):
        raise AssertionError(f"Job '{SYCL_JOB_ID}' {branch_name} selection is unterminated")
    return "\n".join(lines[start : end + 1])


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

    # Ensure no continue-on-error anywhere in the job or its steps.
    for line in job_block.splitlines():
        stripped = line.strip()
        if stripped.startswith("continue-on-error:"):
            raise AssertionError(f"Job '{SYCL_JOB_ID}' must not contain continue-on-error: {line}")

    job_if_match = re.search(r"^    if:\s*(.+?)\s*$", job_block, re.MULTILINE)
    if job_if_match is not None and is_constant_false_condition(job_if_match.group(1)):
        raise AssertionError(
            f"Job '{SYCL_JOB_ID}' must not use a constant-false job guard: "
            f"{job_if_match.group(1)}"
        )

    # Every event path owns a separate git command. Validate each independently
    # so one complete branch cannot hide missing header coverage in another.
    for branch_name, anchor in EVENT_SELECTION_ANCHORS:
        selection = extract_file_selection(job_block, branch_name, anchor)
        for pattern in EXPECTED_FILE_PATTERNS:
            quoted = f"'{pattern}'"
            if quoted not in selection:
                raise AssertionError(
                    f"Job '{SYCL_JOB_ID}' {branch_name} selection missing expected pattern: "
                    f"{quoted}"
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

    strict_match = re.search(r"const strictMustReport = \[(.*?)\];", aggregator_text, re.DOTALL)
    if strict_match is None:
        raise AssertionError(
            "required-aggregator.yml must declare 'const strictMustReport = [...]'"
        )
    strict_names = re.findall(r"'([^']+)'", strict_match.group(1))
    if SYCL_CHECK_NAME not in strict_names:
        raise AssertionError(
            f"required-aggregator.yml must declare '{SYCL_CHECK_NAME}' in strictMustReport"
        )


class SyclTidyWorkflowContractTest(unittest.TestCase):
    """Contract assertions and mutation tests for SYCL clang-tidy gate hardening."""

    def test_lint_and_format_workflow_structure(self) -> None:
        workflow_text = (WORKFLOWS_DIR / "lint-and-format.yml").read_text(encoding="utf-8")
        validate_sycl_workflow_structure(workflow_text)

    def test_required_aggregator_declaration(self) -> None:
        aggregator_text = (WORKFLOWS_DIR / "required-aggregator.yml").read_text(encoding="utf-8")
        validate_sycl_aggregator_declaration(aggregator_text)

    def test_failed_sycl_tidy_blocks_aggregator(self) -> None:
        failures = run_required_aggregator(SYCL_CHECK_NAME, "failure")
        self.assertEqual(len(failures), 1)
        self.assertIn(f"{SYCL_CHECK_NAME}: failure", failures[0])

    def test_successful_sycl_tidy_passes_aggregator(self) -> None:
        self.assertEqual(run_required_aggregator(SYCL_CHECK_NAME, "success"), [])

    def test_unreported_sycl_tidy_blocks_aggregator(self) -> None:
        failures = run_required_aggregator(SYCL_CHECK_NAME, None)
        self.assertEqual(len(failures), 1)
        self.assertIn(
            f"{SYCL_CHECK_NAME}: never reported (strict required context)",
            failures[0],
        )

    def test_skipped_sycl_tidy_blocks_aggregator(self) -> None:
        failures = run_required_aggregator(SYCL_CHECK_NAME, "skipped")
        self.assertEqual(len(failures), 1)
        self.assertIn(
            f"{SYCL_CHECK_NAME}: skipped (strict required context)",
            failures[0],
        )

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

    def test_mutation_constant_false_job_guards_fail_contract(self) -> None:
        workflow_text = (WORKFLOWS_DIR / "lint-and-format.yml").read_text(encoding="utf-8")
        job_block = extract_job_block(workflow_text, SYCL_JOB_ID)
        active_guard = (
            "    if: github.event_name != 'pull_request' || "
            "github.event.pull_request.draft == false\n"
        )
        false_guards = (
            "false",
            "( false )",
            "${{ false }}",
            "${{ (( false )) }}",
            "0",
            "null",
            "${{ ! true }}",
        )

        for false_guard in false_guards:
            with self.subTest(condition=false_guard):
                mutated_job = job_block.replace(
                    active_guard,
                    f"    if: {false_guard}\n",
                    1,
                )
                self.assertNotEqual(job_block, mutated_job)
                mutated = workflow_text.replace(job_block, mutated_job, 1)
                with self.assertRaises(AssertionError) as ctx:
                    validate_sycl_workflow_structure(mutated)
                self.assertIn("constant-false", str(ctx.exception))

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

    def test_mutation_missing_header_pattern_in_each_event_branch_fails_contract(self) -> None:
        workflow_text = (WORKFLOWS_DIR / "lint-and-format.yml").read_text(encoding="utf-8")
        pattern = "'core/src/sycl/*.h'"
        branch_names = ("pull request", "push fallback", "push", "dispatch")
        positions = [match.start() for match in re.finditer(re.escape(pattern), workflow_text)]
        self.assertEqual(len(positions), len(branch_names))

        for branch_name, position in zip(branch_names, positions, strict=True):
            with self.subTest(branch=branch_name):
                mutated = workflow_text[:position] + workflow_text[position + len(pattern) :]
                with self.assertRaises(AssertionError) as ctx:
                    validate_sycl_workflow_structure(mutated)
                self.assertIn(branch_name, str(ctx.exception))

    def test_mutation_omitted_from_aggregator_fails_contract(self) -> None:
        aggregator_text = (WORKFLOWS_DIR / "required-aggregator.yml").read_text(encoding="utf-8")
        mutated = aggregator_text.replace(f"'{SYCL_CHECK_NAME}',", "")
        with self.assertRaises(AssertionError) as ctx:
            validate_sycl_aggregator_declaration(mutated)
        self.assertIn(f"must declare '{SYCL_CHECK_NAME}' in required array", str(ctx.exception))

    def test_mutation_omitted_from_strict_must_report_fails_contract(self) -> None:
        aggregator_text = (WORKFLOWS_DIR / "required-aggregator.yml").read_text(encoding="utf-8")
        strict_match = re.search(
            r"const strictMustReport = \[(.*?)\];",
            aggregator_text,
            re.DOTALL,
        )
        self.assertIsNotNone(strict_match)
        strict_block = strict_match.group(0) if strict_match is not None else ""
        mutated = aggregator_text.replace(
            strict_block,
            strict_block.replace(f"'{SYCL_CHECK_NAME}',", ""),
            1,
        )
        with self.assertRaises(AssertionError) as ctx:
            validate_sycl_aggregator_declaration(mutated)
        self.assertIn("strictMustReport", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
