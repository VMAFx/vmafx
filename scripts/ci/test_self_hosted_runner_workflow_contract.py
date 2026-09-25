#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin self-hosted GPU job admission and ownership to executable contracts."""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci.required_aggregator_harness import run_required_aggregator

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
TESTS_WORKFLOW = WORKFLOWS / "tests-and-quality-gates.yml"
SYCL_WORKFLOW = WORKFLOWS / "sycl-parity.yml"
AGGREGATOR_WORKFLOW = WORKFLOWS / "required-aggregator.yml"
HARDWARE_LANES = (
    ("Coverage GPU", "GPU_COVERAGE_ENABLED"),
    ("SYCL Parity (Arc A380)", "SYCL_ARC_RUNNER_ENABLED"),
)


def _job_block(workflow: str, job_id: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(job_id)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
        workflow,
    )
    if match is None:
        raise AssertionError(f"workflow must define job {job_id!r}")
    return match.group(1)


class SelfHostedRunnerWorkflowContract(unittest.TestCase):
    def setUp(self) -> None:
        self.tests_workflow = TESTS_WORKFLOW.read_text(encoding="utf-8")
        self.sycl_workflow = SYCL_WORKFLOW.read_text(encoding="utf-8")
        self.aggregator = AGGREGATOR_WORKFLOW.read_text(encoding="utf-8")

    def test_gpu_full_job_has_hosted_admission_probe(self) -> None:
        probe = _job_block(self.tests_workflow, "gpu-full-runner-available")
        coverage = _job_block(self.tests_workflow, "coverage-gpu")

        self.assertIn("runs-on: ubuntu-26.04", probe)
        self.assertIn("RUNNER_ENABLED: ${{ vars.GPU_COVERAGE_ENABLED }}", probe)
        self.assertIn('RUNNER_LABELS: "self-hosted linux gpu-full"', probe)
        self.assertIn("bash scripts/ci/check-runner-available.sh", probe)
        self.assertIn("github.event.pull_request.head.repo.full_name == github.repository", probe)

        self.assertIn("needs: [gpu-full-runner-available]", coverage)
        self.assertIn(
            "if: needs.gpu-full-runner-available.outputs.available == 'true'",
            coverage,
        )
        self.assertIn("runs-on: [self-hosted, linux, gpu-full]", coverage)

    def test_gpu_full_has_one_hardware_owner_and_sycl_has_one_parity_owner(self) -> None:
        gpu_targets = re.findall(
            r"(?m)^\s+runs-on: \[self-hosted, linux, gpu-full\]\s*$",
            self.tests_workflow,
        )
        self.assertEqual(gpu_targets, ["    runs-on: [self-hosted, linux, gpu-full]"])
        self.assertNotIn("sycl-float-ssim-parity:", self.tests_workflow)
        self.assertNotIn("name: SYCL float_ssim Parity", self.tests_workflow)

        parity = _job_block(self.sycl_workflow, "sycl-parity")
        self.assertIn("runs-on: [self-hosted, linux, x64, sycl-arc]", parity)
        probe = _job_block(self.sycl_workflow, "runner-available")
        self.assertIn(
            'RUNNER_LABELS: "self-hosted linux x64 sycl-arc"',
            probe,
        )
        self.assertIn("--features float_ssim", parity)
        self.assertNotIn("gpu-full", self.sycl_workflow)

    def test_aggregator_models_gpu_lane_switch_and_drops_duplicate_sycl_name(self) -> None:
        self.assertIn("GPU_COVERAGE_ENABLED: ${{ vars.GPU_COVERAGE_ENABLED }}", self.aggregator)
        self.assertIn("'Coverage GPU': {", self.aggregator)
        self.assertIn("const hardwareLane = hardwareLanes[name]", self.aggregator)
        self.assertIn('"Coverage GPU": ["Probe GPU Full Runner"]', self.aggregator)
        self.assertIn(
            '"SYCL Parity (Arc A380)": ["Probe SYCL Runner"]',
            self.aggregator,
        )
        self.assertNotIn("'SYCL float_ssim Parity'", self.aggregator)

    def test_enabled_hardware_lanes_require_success(self) -> None:
        for check_name, switch_name in HARDWARE_LANES:
            env = {switch_name: "true"}
            for conclusion in (None, "skipped", "neutral", "failure"):
                with self.subTest(check_name=check_name, conclusion=conclusion):
                    failures = run_required_aggregator(check_name, conclusion, env=env)
                    self.assertEqual(len(failures), 1)
                    self.assertIn(check_name, failures[0])
            self.assertEqual(
                run_required_aggregator(check_name, "success", env=env),
                [],
            )

    def test_disabled_hardware_lanes_accept_absence_or_skip(self) -> None:
        for check_name, switch_name in HARDWARE_LANES:
            for value in ("", "false"):
                env = {switch_name: value}
                for conclusion in (None, "skipped"):
                    with self.subTest(
                        check_name=check_name,
                        value=value,
                        conclusion=conclusion,
                    ):
                        self.assertEqual(
                            run_required_aggregator(check_name, conclusion, env=env),
                            [],
                        )


if __name__ == "__main__":
    unittest.main()
