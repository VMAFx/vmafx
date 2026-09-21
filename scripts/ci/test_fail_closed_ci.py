#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep test, coverage, benchmark, and scan failures observable in CI."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"


def workflow_step(text: str, name: str) -> str:
    """Return one top-level step block by its exact display name."""
    match = re.search(
        rf"(?ms)^      - name: {re.escape(name)}\n(?P<body>.*?)(?=^      - |\Z)",
        text,
    )
    if match is None:
        raise AssertionError(f"workflow step not found: {name}")
    return match.group(0)


def workflow_job(text: str, job_id: str) -> str:
    """Return one top-level job block by its identifier."""
    match = re.search(
        rf"(?ms)^  {re.escape(job_id)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        text,
    )
    if match is None:
        raise AssertionError(f"workflow job not found: {job_id}")
    return match.group(0)


class FailClosedCIContract(unittest.TestCase):
    def test_contract_is_wired_into_required_ci_and_local_hooks(self) -> None:
        rules = (WORKFLOWS / "rule-enforcement.yml").read_text(encoding="utf-8")
        hooks = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        command = "python3 scripts/ci/test_fail_closed_ci.py"
        self.assertIn(command, rules)
        self.assertIn("id: fail-closed-ci-contract", hooks)
        self.assertRegex(hooks, r"entry: python3(?: -B)? scripts/ci/test_fail_closed_ci\.py")

    def test_python_tox_warnings_and_coverage_fail_closed(self) -> None:
        tox = (ROOT / "python" / "tox.ini").read_text(encoding="utf-8")
        self.assertNotRegex(tox, r"(?m)^ignore_outcome\s*=\s*true\s*$")
        self.assertNotIn("-p no:warnings", tox)
        self.assertIn("filterwarnings = error", tox)
        coverage = tox.split("[testenv:coverage]", maxsplit=1)[1].split("\n[testenv:", maxsplit=1)[
            0
        ]
        self.assertIn("depends = py314", coverage)
        self.assertNotRegex(coverage, r"(?m)^\s*coverage (?:report|xml|html) .*\s-i(?:\s|$)")

    def test_cpu_coverage_records_and_reasserts_pytest_failure(self) -> None:
        workflow = (WORKFLOWS / "tests-and-quality-gates.yml").read_text(encoding="utf-8")
        self.assertNotRegex(workflow, r"(?m)^    if: false\s*$")
        pytest_step = workflow_step(
            workflow, "Run full Python test suite (instrumented libvmaf.so + vmaf CLI)"
        )
        self.assertIn("id: python_coverage_tests", pytest_step)
        self.assertIn("continue-on-error: true", pytest_step)
        self.assertNotIn("|| true", pytest_step)

        assertion = workflow_step(workflow, "Assert Python coverage suite passed")
        self.assertIn("steps.python_coverage_tests.outcome", assertion)
        self.assertIn('[[ "$PYTEST_OUTCOME" == "success" ]]', assertion)

        gpu_coverage = workflow_job(workflow, "coverage-gpu")
        self.assertIn("name: Coverage GPU", gpu_coverage)
        self.assertIn("# required-aggregator", gpu_coverage)
        self.assertNotIn("continue-on-error", gpu_coverage)
        aggregator = (WORKFLOWS / "required-aggregator.yml").read_text(encoding="utf-8")
        self.assertIn("'Coverage GPU',", aggregator)

    def test_nightly_benchmark_failure_remains_a_failure(self) -> None:
        workflow = (WORKFLOWS / "nightly.yml").read_text(encoding="utf-8")
        benchmark = workflow_step(workflow, "Run benchmark")
        self.assertIn("bash testdata/bench_all.sh", benchmark)
        self.assertNotIn("|| true", benchmark)
        upload = workflow.split("name: nightly-benchmark-results", maxsplit=1)[0]
        self.assertRegex(upload, r"(?m)^        if: always\(\)\s*$")

    def test_semgrep_advisory_preserves_failure_outcome(self) -> None:
        workflow = (WORKFLOWS / "security-scans.yml").read_text(encoding="utf-8")
        registry = workflow_step(workflow, "Run semgrep (registry rule packs — advisory)")
        self.assertIn("continue-on-error: true", registry)
        self.assertNotIn("|| true", registry)

    def test_sanitizer_enumeration_does_not_mask_producer_errors(self) -> None:
        expected_counts = {"tests-and-quality-gates.yml": 1, "sanitizers.yml": 2}
        for workflow_name, expected_count in expected_counts.items():
            workflow = (WORKFLOWS / workflow_name).read_text(encoding="utf-8")
            starts = list(re.finditer(r"TESTS=\$\(meson introspect build --tests", workflow))
            self.assertEqual(expected_count, len(starts), workflow_name)
            for start in starts:
                end = workflow.find('\n          echo "test count:', start.start())
                self.assertNotEqual(-1, end, workflow_name)
                enumeration = workflow[start.start() : end]
                self.assertNotIn("|| true", enumeration, workflow_name)
                self.assertNotIn("set +o pipefail", enumeration, workflow_name)


if __name__ == "__main__":
    unittest.main()
