#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Guard the Security Scans concurrency and C/C++ extraction contracts."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SECURITY_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "security-scans.yml"
EXPECTED_GROUP = "group: security-${{ github.workflow }}-${{ github.event_name }}-${{ github.ref }}"
COLLIDING_GROUP = "group: security-${{ github.workflow }}-${{ github.ref }}"


class SecurityWorkflowContractTest(unittest.TestCase):
    """Prevent event collisions and generated-source extraction regressions."""

    @staticmethod
    def _job_block(workflow: str, job_name: str) -> str:
        lines = workflow.splitlines()
        start = lines.index(f"  {job_name}:")
        end = len(lines)
        for index in range(start + 1, len(lines)):
            if re.fullmatch(r"  [A-Za-z0-9_-]+:", lines[index]):
                end = index
                break
        return "\n".join(lines[start:end])

    def test_concurrency_is_scoped_by_event_and_ref(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        concurrency = workflow.split("\nconcurrency:\n", maxsplit=1)[1].split(
            "\njobs:\n", maxsplit=1
        )[0]

        self.assertIn(EXPECTED_GROUP, concurrency)
        self.assertNotIn(COLLIDING_GROUP, concurrency)
        self.assertIn("cancel-in-progress: true", concurrency)

    def test_cpp_configure_is_outside_extraction_and_checkout(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        job = self._job_block(workflow, "codeql-cpp")

        configure = job.index("- name: Configure C/C++ build (outside extraction)")
        initialize = job.index("- uses: github/codeql-action/init@")
        compile_step = job.index("- name: Build")
        analyze = job.index("- uses: github/codeql-action/analyze@")

        self.assertLess(configure, initialize)
        self.assertLess(initialize, compile_step)
        self.assertLess(compile_step, analyze)
        self.assertIn("--require-hashes", job)
        self.assertIn("-r requirements/locks/build.txt", job)
        self.assertIn('meson setup "${{ runner.temp }}/build" core', job)
        self.assertIn('meson compile -C "${{ runner.temp }}/build"', job)
        self.assertNotIn("working-directory: core", job)
        self.assertNotIn("meson setup build", job)

    def test_semgrep_registry_results_stay_advisory(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        job = self._job_block(workflow, "semgrep")

        self.assertIn("--config=.semgrep.yml", job)
        self.assertIn("category: semgrep-local", job)
        self.assertIn("# required-aggregator: Semgrep OSS", job)

        registry_step = job.split(
            "- name: Run semgrep (registry rule packs — advisory)", maxsplit=1
        )[1].split("- name:", maxsplit=1)[0]
        self.assertIn("continue-on-error: true", registry_step)
        self.assertIn("--output=semgrep-registry.sarif", registry_step)

        self.assertNotIn("category: semgrep-registry", job)
        self.assertNotIn("name: Upload registry-rules SARIF", job)
        archive_step = job.split("- name: Archive registry-rules SARIF (advisory)", maxsplit=1)[
            1
        ].split("- name:", maxsplit=1)[0]
        self.assertIn("hashFiles('semgrep-registry.sarif')", archive_step)
        self.assertIn(
            "uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
            archive_step,
        )
        self.assertIn("name: semgrep-registry-sarif", archive_step)
        self.assertIn("path: semgrep-registry.sarif", archive_step)
        self.assertIn("if-no-files-found: error", archive_step)
        self.assertIn("retention-days: 14", archive_step)
        self.assertNotIn("github/codeql-action/upload-sarif", archive_step)

    def test_dependency_review_allows_only_reviewed_dual_use_tooling(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        job = self._job_block(workflow, "dependency-review")

        self.assertIn("pkg:pypi/text-unidecode", job)
        self.assertIn("pkg:pypi/python-debian", job)


if __name__ == "__main__":
    unittest.main()
