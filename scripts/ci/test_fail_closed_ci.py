#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Keep test, coverage, benchmark, and scan failures observable in CI."""

from __future__ import annotations

import importlib.util
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

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


def executable_body(step: str) -> str:
    """Return a step with its full-line YAML/shell comments removed.

    The fail-open checks below look for literal strings like ``|| true`` in a
    step's text. A comment is not executed, so a comment that *names* the
    construct — for instance one recording why a suffix was removed — is not a
    fail-open and must not be reported as one. Only lines whose first
    non-whitespace character is ``#`` are dropped; an inline trailing comment
    stays, because the command in front of it runs.
    """
    return "\n".join(line for line in step.splitlines() if not line.lstrip().startswith("#"))


def step_script(step: str) -> str:
    """Return the dedented `run: |` script of one workflow step."""
    match = re.search(r"(?ms)^        run: \|\n(?P<body>(?:^          .*\n|^\n)+)", step)
    if match is None:
        raise AssertionError("step has no `run: |` block")
    return "".join(line[10:] if line.strip() else "\n" for line in match["body"].splitlines(True))


def workflow_job(text: str, job_id: str) -> str:
    """Return one top-level job block by its identifier."""
    match = re.search(
        rf"(?ms)^  {re.escape(job_id)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        text,
    )
    if match is None:
        raise AssertionError(f"workflow job not found: {job_id}")
    return match.group(0)


def python_lint_job_problems(job: str) -> list[str]:
    """Return fail-open defects in the required Python Lint job."""
    problems: list[str] = []
    if "fetch-depth: 0" not in job:
        problems.append("Python Lint checkout must fetch merge-base history")
    if "pip install --require-hashes -r requirements/locks/mypy.txt" not in job:
        problems.append("Python Lint must install the hash-locked mypy toolchain")
    try:
        check = workflow_step(job, "Run merge-base mypy gate")
    except AssertionError:
        problems.append("Python Lint must invoke the merge-base mypy gate")
        return problems
    body = executable_body(check)
    if (
        "VMAFX_MYPY_BASE_REF:" not in check
        or "github.event.before" not in check
        or "origin/master" not in check
    ):
        problems.append("Python Lint must select a PR or push merge-base authority")
    if "python3 scripts/git-hooks/pre-push-mypy.py" not in body:
        problems.append("Python Lint must use the canonical merge-base mypy gate")
    if "continue-on-error:" in check or re.search(r"(?m)^\s*run:.*\|\|", body):
        problems.append("Python Lint must propagate the mypy gate status")
    if re.search(r"\bmypy\s+(?:ai/|scripts/)", body):
        problems.append("Python Lint must not bypass canonical module-identity splitting")
    return problems


class FailClosedCIContract(unittest.TestCase):
    def test_contract_is_wired_into_required_ci_and_local_hooks(self) -> None:
        rules = (WORKFLOWS / "rule-enforcement.yml").read_text(encoding="utf-8")
        hooks = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        # A test runs once in CI (ADR-1568): Tooling Tests runs this file.
        sys.path.insert(0, str(ROOT))
        from scripts.ci.suite_registry import suite_members  # noqa: PLC0415

        self.assertIn("scripts/ci/test_fail_closed_ci.py", suite_members(ROOT, "tooling"))
        self.assertNotIn("test_fail_closed_ci.py", rules)
        self.assertIn("id: fail-closed-ci-contract", hooks)
        self.assertRegex(hooks, r"entry: python3(?: -B)? scripts/ci/test_fail_closed_ci\.py")
        hook = hooks.split("      - id: fail-closed-ci-contract\n", maxsplit=1)[1].split(
            "      - id:", maxsplit=1
        )[0]
        self.assertIn("lint-and-format", hook)

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
        assertion = workflow_step(workflow, "Assert every Python coverage shard passed")
        self.assertIn('!= "success"', assertion)
        for index in range(1, 5):
            with self.subTest(shard=index):
                pytest_step = workflow_step(workflow, f"Run Python test suite, shard {index}/4")
                self.assertIn(f"id: python_coverage_shard_{index}", pytest_step)
                self.assertIn(f"--shard {index}/4", pytest_step)
                self.assertIn("continue-on-error: true", pytest_step)
                self.assertNotIn("|| true", executable_body(pytest_step))
                self.assertIn(f"steps.python_coverage_shard_{index}.outcome", assertion)
                # #2823 wrote PYTHO1PATH..PYTHO4PATH, so no shard got the path.
                self.assertRegex(
                    pytest_step, r"(?m)^          PYTHONPATH: \$\{\{ github\.workspace \}\}/python$"
                )
                self.assertNotRegex(pytest_step, r"PYTHO\dPATH")

        gpu_coverage = workflow_job(workflow, "coverage-gpu")
        self.assertIn("name: Coverage GPU", gpu_coverage)
        self.assertIn("# required-aggregator", gpu_coverage)
        self.assertNotIn("continue-on-error", gpu_coverage)
        aggregator = (WORKFLOWS / "required-aggregator.yml").read_text(encoding="utf-8")
        self.assertIn("'Coverage GPU',", aggregator)

    def test_coverage_shard_assertion_names_why_a_shard_was_skipped(self) -> None:
        workflow = (WORKFLOWS / "tests-and-quality-gates.yml").read_text(encoding="utf-8")
        assertion = workflow_step(workflow, "Assert every Python coverage shard passed")
        coverage = workflow_job(workflow, "coverage")
        for step_id in re.findall(r"\$\{\{ steps\.(\w+)\.outcome \}\}", assertion):
            with self.subTest(step=step_id):
                self.assertIn(f"        id: {step_id}\n", coverage)
        self.assertIn("steps.coverage_meson_suite.outcome", assertion)
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash not on PATH; the workflow step runs on ubuntu")
        script = step_script(assertion)
        earlier = "Build-with-gcov=success Meson-unit-suite=failure Prepare-Python-suite=skipped"

        def run(shards: str, before: str) -> subprocess.CompletedProcess[str]:
            env = {"SHARD_OUTCOMES": shards, "EARLIER_OUTCOMES": before, "PATH": "/usr/bin:/bin"}
            return subprocess.run(  # noqa: S603 - argv is bash plus the tracked workflow step
                [bash, "-c", script], env=env, capture_output=True, text=True, check=False
            )

        skipped = run("skipped skipped skipped skipped", earlier)
        self.assertEqual(skipped.returncode, 1)
        self.assertIn(
            "shard 1/4 outcome was skipped: step Meson-unit-suite outcome was failure",
            skipped.stderr,
        )
        self.assertEqual(skipped.stderr.count("::error::"), 4)
        failed = run("success failure success success", earlier.replace("failure", "success"))
        self.assertEqual(failed.returncode, 1)
        self.assertIn("shard 2/4 outcome was failure\n", failed.stderr)
        self.assertNotIn("skipped", failed.stderr)
        passed = run("success success success success", "Meson-unit-suite=success")
        self.assertEqual((passed.returncode, passed.stderr), (0, ""))
        unexplained = run("success skipped success success", "Meson-unit-suite=success")
        self.assertIn("shard 2/4 outcome was skipped: no earlier step failed", unexplained.stderr)

    def test_nightly_benchmark_failure_remains_a_failure(self) -> None:
        workflow = (WORKFLOWS / "nightly.yml").read_text(encoding="utf-8")
        benchmark = workflow_step(workflow, "Run benchmark")
        self.assertIn("VMAF_BIN: ${{ github.workspace }}/build/tools/vmaf", benchmark)
        self.assertIn("bash testdata/bench_all.sh", benchmark)
        self.assertNotIn("|| true", executable_body(benchmark))
        upload = workflow.split("name: nightly-benchmark-results", maxsplit=1)[0]
        self.assertRegex(upload, r"(?m)^        if: always\(\)\s*$")

    def test_nightly_clang_tidy_configuration_and_toolchain(self) -> None:
        workflow = (WORKFLOWS / "nightly.yml").read_text(encoding="utf-8")
        tidy_job = workflow_job(workflow, "clang-tidy-full")
        self.assertIn("ppa:ubuntu-toolchain-r/test", tidy_job)
        self.assertIn("https://apt.llvm.org/llvm.sh", tidy_job)
        self.assertIn("sudo /tmp/llvm.sh 22", tidy_job)
        self.assertIn("sudo apt-get install -y clang-tidy-22", tidy_job)
        # The job runs the cpu lane's Makefile targets, as the required job and
        # the dev container do (T-TIDY-RATCHET-UNMEASURED-AS-CLEAN-2026-10-06):
        # gcc-15, no LTO and the hashed build requirements come from there.
        self.assertIn("make tidy-ratchet-build LANE=cpu", tidy_job)
        self.assertIn("make tidy-ratchet LANE=cpu", tidy_job)
        self.assertIn("CLANG_TIDY_BIN=/usr/bin/clang-tidy-22", tidy_job)
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn("TIDY_RATCHET_COMPILERS_cpu := CC=gcc-15 CXX=g++-15", makefile)
        setup_cpu = makefile.split("TIDY_RATCHET_SETUP_cpu :=", 1)[1].split("\nTIDY_", 1)[0]
        self.assertIn("-Db_lto=false", setup_cpu)
        self.assertIn("install --require-hashes -r requirements/locks/build.txt", makefile)

    def test_changed_tidy_excludes_windows_only_translation_unit(self) -> None:
        workflow = (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8")
        tidy_job = workflow_job(workflow, "clang-tidy")
        self.assertIn("^core/tools/test/test_vmaf_windows_utf8_argv\\.cpp$", tidy_job)

    def test_tidy_ratchet_build_stays_outside_repository(self) -> None:
        workflow = (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8")
        ratchet_job = workflow_job(workflow, "clang-tidy-ratchet")
        build_step = workflow_step(ratchet_job, "Build (compile_commands.json + generated headers)")
        run_step = workflow_step(
            ratchet_job, "Ratchet — whole-tree clang-tidy vs committed baseline"
        )
        self.assertIn('TIDY_RATCHET_BUILD_DIR="$RUNNER_TEMP/vmafx-tidy-cpu"', build_step)
        self.assertIn("make tidy-ratchet-build LANE=cpu", build_step)
        self.assertIn('TIDY_RATCHET_BUILD_DIR="$RUNNER_TEMP/vmafx-tidy-cpu"', run_step)
        self.assertNotIn("meson setup build core", executable_body(build_step))

    def test_fuzz_workflows_have_adequate_timeout_budget(self) -> None:
        fuzz_wf = (WORKFLOWS / "fuzz.yml").read_text(encoding="utf-8")
        fuzz_job = workflow_job(fuzz_wf, "fuzz")
        self.assertRegex(fuzz_job, r"(?m)^    timeout-minutes: 30\s*$")

        sanitizers_wf = (WORKFLOWS / "sanitizers.yml").read_text(encoding="utf-8")
        sanitizer_fuzz_job = workflow_job(sanitizers_wf, "fuzz-nightly")
        self.assertRegex(sanitizer_fuzz_job, r"(?m)^    timeout-minutes: 30\s*$")

    def test_semgrep_advisory_preserves_failure_outcome(self) -> None:
        workflow = (WORKFLOWS / "security-scans.yml").read_text(encoding="utf-8")
        registry = workflow_step(workflow, "Run semgrep (registry rule packs — advisory)")
        self.assertIn("continue-on-error: true", registry)
        self.assertNotIn("|| true", executable_body(registry))

    def test_python_lint_uses_the_fail_closed_merge_base_gate(self) -> None:
        workflow = (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8")
        job = workflow_job(workflow, "python-lint")
        self.assertEqual(python_lint_job_problems(job), [])

    def test_python_lint_contract_detects_fail_open_mutations(self) -> None:
        workflow = (WORKFLOWS / "lint-and-format.yml").read_text(encoding="utf-8")
        job = workflow_job(workflow, "python-lint")
        mutations = {
            "shallow checkout": job.replace("fetch-depth: 0", "fetch-depth: 1", 1),
            "unhashed checker": job.replace(" --require-hashes", "", 1),
            "missing push base": job.replace("github.event.before", "github.sha", 1),
            "advisory status": job.replace(
                "python3 scripts/git-hooks/pre-push-mypy.py",
                'python3 scripts/git-hooks/pre-push-mypy.py || echo "advisory"',
                1,
            ),
            "raw directory scan": job.replace(
                "python3 scripts/git-hooks/pre-push-mypy.py", "mypy ai/ scripts/", 1
            ),
        }
        for name, mutated in mutations.items():
            with self.subTest(name=name):
                self.assertTrue(python_lint_job_problems(mutated))

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
                self.assertNotIn("|| true", executable_body(enumeration), workflow_name)
                self.assertNotIn("set +o pipefail", executable_body(enumeration), workflow_name)

    def test_executable_body_drops_comments_but_keeps_commands(self) -> None:
        """A comment naming a fail-open is not one; a real suffix still is."""
        commented = (
            "        run: |\n          # removed the trailing || true here\n          pytest\n"
        )
        self.assertNotIn("|| true", executable_body(commented))
        real = "        run: |\n          # documented\n          pytest || true\n"
        self.assertIn("|| true", executable_body(real))
        inline = "        run: pytest || true  # keep going\n"
        self.assertIn("|| true", executable_body(inline))

    def test_workflows_require_checkout_before_consuming_repo_resources(self) -> None:
        """Every workflow consuming repo requirements or helpers must run actions/checkout first."""
        checker_path = ROOT / "scripts" / "ci" / "check_python_dependency_locks.py"
        spec = importlib.util.spec_from_file_location("check_python_dependency_locks", checker_path)
        self.assertIsNotNone(spec)
        self.assertIsNotNone(spec.loader)  # type: ignore[union-attr]
        checker = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
        spec.loader.exec_module(checker)  # type: ignore[union-attr]
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            text = workflow.read_text(encoding="utf-8")
            findings = checker.scan_workflow_checkout_ordering(workflow, text, root=ROOT)
            self.assertEqual(
                findings, [], f"{workflow.name} consumes repo resources before actions/checkout"
            )
            with mock.patch.object(checker, "yaml", None):
                fallback_findings = checker.scan_workflow_checkout_ordering(
                    workflow, text, root=ROOT
                )
                self.assertEqual(
                    fallback_findings,
                    [],
                    f"{workflow.name} consumes repo resources before actions/checkout (fallback)",
                )


if __name__ == "__main__":
    unittest.main()
