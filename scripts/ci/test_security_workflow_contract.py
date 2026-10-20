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
EXPECTED_GROUP = (
    "group: security-${{ github.workflow }}-${{ github.event_name }}-"
    "${{ github.ref == 'refs/heads/master' && github.sha || github.ref }}"
)
COLLIDING_GROUP = "group: security-${{ github.workflow }}-${{ github.ref }}"
EXPECTED_DEPENDENCY_LICENSE_ALLOWLIST = {
    "pkg:pypi/python-debian",
    "pkg:pypi/text-unidecode",
}
# Every language master analyses must be analysed on every pull request too:
# GitHub's `CodeQL` check is neutral ("configuration not found") when a
# category of the base is missing from the head, and the required aggregator
# fails a neutral owed check (ADR-3293).
CODEQL_JOBS = {
    "codeql-cpp": "cpp",
    "codeql-python": "python",
    "codeql-actions": "actions",
}
# The CodeQL C++ extractor reads -std=c++26 as C++17 and misparses the C++26
# standard headers into alerts on code that does not exist (ADR-3355). The
# analysed build pins the newest standard it parses.
CODEQL_CPP_STD = "c++23"


def cpp_std_problems(job: str) -> list[str]:
    """Return why the CodeQL C/C++ build is not on CODEQL_CPP_STD, or []."""
    setups = re.findall(r"meson setup (?:[^\n]*\\\n)*[^\n]*", job)
    if not setups:
        return ["no meson setup in the job"]
    problems = []
    for setup in setups:
        stds = re.findall(r"-Dcpp_std=(\S+)", setup)
        if stds != [CODEQL_CPP_STD]:
            problems.append(f"meson setup sets cpp_std {stds or 'not at all'}")
    return problems


def codeql_job_problems(job: str, language: str) -> list[str]:
    """Return why a CodeQL job could skip its analysis, or an empty list."""
    problems = []
    if "steps.impact.outputs" in job:
        problems.append("analysis depends on the impact planner's outputs")
    if "Plan CI impact" in job:
        problems.append("job still runs the impact planner")
    if f"languages: {language}" not in job:
        problems.append(f"init does not name language {language}")
    if f'category: "/language:{language}"' not in job:
        problems.append(f"analyze does not upload category /language:{language}")
    for action in ("init", "analyze"):
        _, found, rest = job.partition(f"- uses: github/codeql-action/{action}@")
        if not found:
            problems.append(f"no codeql-action/{action} step")
            continue
        body = rest.split("\n      - ", maxsplit=1)[0]
        if re.search(r"^\s+if:", body, flags=re.MULTILINE):
            problems.append(f"codeql-action/{action} step is conditional")
    return problems


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
        self.assertIn("cancel-in-progress: ${{ github.ref != 'refs/heads/master' }}", concurrency)

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

    def test_cpp_build_uses_a_standard_codeql_parses(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        job = self._job_block(workflow, "codeql-cpp")
        self.assertEqual(cpp_std_problems(job), [])

    def test_cpp_std_gate_refuses_the_project_default(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        job = self._job_block(workflow, "codeql-cpp")
        dropped = job.replace(f" -Dcpp_std={CODEQL_CPP_STD}", "")
        self.assertNotEqual(dropped, job)
        self.assertEqual(cpp_std_problems(dropped), ["meson setup sets cpp_std not at all"])
        newer = job.replace(f"-Dcpp_std={CODEQL_CPP_STD}", "-Dcpp_std=c++26")
        self.assertEqual(cpp_std_problems(newer), ["meson setup sets cpp_std ['c++26']"])

    def test_codeql_analyses_every_language_on_every_event(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        for job_name, language in CODEQL_JOBS.items():
            with self.subTest(job=job_name):
                job = self._job_block(workflow, job_name)
                self.assertEqual(codeql_job_problems(job, language), [])

    def test_codeql_gate_refuses_an_impact_gated_analysis(self) -> None:
        workflow = SECURITY_WORKFLOW.read_text(encoding="utf-8")
        job = self._job_block(workflow, "codeql-python")
        planted = job.replace(
            "      - uses: github/codeql-action/analyze@",
            "      - if: steps.impact.outputs.python_lint == 'true'\n"
            "        uses: github/codeql-action/analyze@",
        )
        self.assertNotEqual(planted, job)
        self.assertIn(
            "analysis depends on the impact planner's outputs",
            codeql_job_problems(planted, "python"),
        )
        conditional = job.replace(
            "        with:\n          category:",
            "        if: github.event_name == 'push'\n        with:\n          category:",
        )
        self.assertNotEqual(conditional, job)
        self.assertIn(
            "codeql-action/analyze step is conditional",
            codeql_job_problems(conditional, "python"),
        )

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
            "uses: actions/upload-artifact@cf430e030ddbb5b0abf93d22962f4752f3646cd9",
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
        values = re.findall(
            r"^\s+allow-dependencies-licenses:\s*(?P<value>[^#\n]+?)\s*$",
            job,
            re.MULTILINE,
        )

        self.assertEqual(len(values), 1)
        actual = {purl.strip() for purl in values[0].split(",") if purl.strip()}
        self.assertEqual(actual, EXPECTED_DEPENDENCY_LICENSE_ALLOWLIST)


if __name__ == "__main__":
    unittest.main()
