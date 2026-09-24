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


if __name__ == "__main__":
    unittest.main()
