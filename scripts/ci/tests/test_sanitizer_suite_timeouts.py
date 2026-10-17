#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""An instrumented build runs the Meson tests with their timeouts multiplied.

The timeouts of core/test/meson.build (30 s unless a test sets its own) are
sized for an uninstrumented build. In a sanitizer build the slowest tests ran
at 0.64 to 0.93 of their timeout on a quiet hosted runner, and on 2026-10-10
test_vmafx_score hit its 30 s on the thread leg, after 10.27 s to 20.48 s in
four other runs of that leg (T-CI-SANITIZER-LEGS-DEFAULT-TEST-TIMEOUT-2026-10-10;
the measurement is in docs/development/ci.md, "Sanitizer legs").

A gcov build is slowed the same way: on 2026-10-17 test_integer_adm_simd,
19.6 s to 19.8 s in three green Coverage Gate runs, hit its 30 s in run
39006126494 (T-COVERAGE-GCOV-DEFAULT-TEST-TIMEOUT-2026-10-17).

The contract: in every workflow job that configures a build with
`-Db_sanitize=<sanitizer>` or `-Db_coverage=true`, every
scripts/ci/run_meson_test.py command passes `--timeout-multiplier MULTIPLIER`,
and so do the Makefile's test-sanitizers and coverage. One value everywhere: a
leg with a smaller one is the leg that flakes.
"""

from __future__ import annotations

import importlib.util
import re
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"
MULTIPLIER = "3"
SANITIZED_BUILD = re.compile(r"-Db_sanitize=(?!none\b)\S|-Db_coverage=true\b")
TIMEOUT_MULTIPLIER = re.compile(r"(?:--timeout-multiplier|(?<![\w-])-t)[= ]+(\S+)")
MAKE_VARIABLE = "INSTRUMENTED_TEST_TIMEOUT_MULTIPLIER"
MAKE_TARGETS = ("test-sanitizers", "coverage")


def load(path: Path, name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SUITE = load(Path(__file__).with_name("test_meson_suite_checkout_history.py"), "suite_steps")


def problem(arguments: str, multiplier: str = MULTIPLIER) -> str:
    """Why one runner command does not multiply the timeouts as required, or ""."""
    values = TIMEOUT_MULTIPLIER.findall(arguments)
    if not values:
        return "no --timeout-multiplier"
    if values != [multiplier]:
        return f"--timeout-multiplier {' '.join(values)}, not {multiplier}"
    return ""


def sanitizer_commands(workflow: dict[str, Any]) -> list[tuple[str, str]]:
    """(job, arguments) of every runner command in a job with a sanitizer build."""
    found = []
    for job_id, job in (workflow.get("jobs") or {}).items():
        runs = [str(step.get("run", "")) for step in job.get("steps") or []]
        if not any(SANITIZED_BUILD.search(run) for run in runs):
            continue
        found += [(job_id, arguments) for run in runs for arguments in SUITE.suite_commands(run)]
    return found


def offenders(workflow: dict[str, Any], name: str) -> list[str]:
    return [
        f"{name}: job {job_id}: {problem(arguments)}"
        for job_id, arguments in sanitizer_commands(workflow)
        if problem(arguments)
    ]


def workflow(name: str) -> dict[str, Any]:
    loaded: dict[str, Any] = yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8")) or {}
    return loaded


def makefile_problems(text: str) -> list[str]:
    """What the instrumented targets lack: the variable at MULTIPLIER, and its use."""
    problems = []
    if not re.search(rf"(?m)^{MAKE_VARIABLE} := {MULTIPLIER}$", text):
        problems.append(f"Makefile: {MAKE_VARIABLE} is not {MULTIPLIER}")
    for target in MAKE_TARGETS:
        recipe = re.search(rf"(?m)^{re.escape(target)}:\n(?P<body>(?:\t.*\n)+)", text)
        if recipe is None:
            problems.append(f"Makefile: no {target} recipe")
            continue
        commands = SUITE.suite_commands(recipe.group("body"))
        if len(commands) != 1:
            problems.append(f"Makefile: {target} must run the tests once")
        for arguments in commands:
            found = problem(arguments, f"$({MAKE_VARIABLE})")
            if found:
                problems.append(f"Makefile: {target}: {found}")
    return problems


class SanitizerSuiteTimeouts(unittest.TestCase):
    def test_every_sanitizer_test_run_multiplies_the_timeouts(self) -> None:
        found = []
        for path in sorted(WORKFLOWS.glob("*.yml")):
            found += offenders(workflow(path.name), path.name)
        self.assertEqual(found, [])

    def test_the_sanitizer_jobs_are_found(self) -> None:
        # The contract must look at something: one command per sanitizer job.
        jobs = {
            f"{path.name}:{job_id}"
            for path in sorted(WORKFLOWS.glob("*.yml"))
            for job_id, _ in sanitizer_commands(workflow(path.name))
        }
        self.assertEqual(
            jobs,
            {
                "tests-and-quality-gates.yml:sanitizers",
                "sanitizers.yml:asan-ubsan",
                "sanitizers.yml:tsan",
                "nightly.yml:tsan",
                "tests-and-quality-gates.yml:coverage",
                "tests-and-quality-gates.yml:coverage-gpu",
            },
        )

    def test_planted_gcov_run_without_the_multiplier_is_refused(self) -> None:
        build = {"run": "meson setup build-coverage --buildtype=debug -Db_coverage=true"}
        runner = "python3 ../scripts/ci/run_meson_test.py -- -C build-coverage"
        planted = {
            "jobs": {
                "coverage": {"steps": [build, {"run": f"{runner} --num-processes 1"}]},
                "uninstrumented": {
                    "steps": [
                        {"run": "meson setup build -Db_coverage=false"},
                        {"run": f"{runner} --num-processes 1"},
                    ]
                },
            }
        }
        self.assertEqual(
            offenders(planted, "p.yml"), ["p.yml: job coverage: no --timeout-multiplier"]
        )

    def test_planted_sanitizer_runs_without_the_multiplier_are_refused(self) -> None:
        build = {"run": "meson setup build -Db_sanitize=thread --buildtype=debug"}
        runner = "python3 ../scripts/ci/run_meson_test.py -- -C build --print-errorlogs"
        planted = {
            "jobs": {
                "default-timeouts": {"steps": [build, {"run": f"{runner} $TESTS"}]},
                "too-small": {"steps": [build, {"run": f"{runner} --timeout-multiplier 2"}]},
                "only-in-a-comment": {
                    "steps": [build, {"run": f"# --timeout-multiplier 3\n{runner} $TESTS"}]
                },
                "second-run-forgotten": {
                    "steps": [
                        build,
                        {"run": f"{runner} --timeout-multiplier 3 $TESTS"},
                        {"run": f"{runner} --suite=fast"},
                    ]
                },
                "ok": {"steps": [build, {"run": f"{runner} \\\n  --timeout-multiplier 3 $TESTS"}]},
                "ok-short-option": {"steps": [build, {"run": f"{runner} -t 3"}]},
                "plain-build": {
                    "steps": [
                        {"run": "meson setup build -Db_sanitize=none"},
                        {"run": f"{runner} --suite=fast"},
                    ]
                },
            }
        }
        self.assertEqual(
            offenders(planted, "p.yml"),
            [
                "p.yml: job default-timeouts: no --timeout-multiplier",
                "p.yml: job too-small: --timeout-multiplier 2, not 3",
                "p.yml: job only-in-a-comment: no --timeout-multiplier",
                "p.yml: job second-run-forgotten: no --timeout-multiplier",
            ],
        )

    def test_the_makefile_instrumented_targets_multiply_the_timeouts(self) -> None:
        text = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertEqual(makefile_problems(text), [])
        use = f" \\\n\t    --timeout-multiplier $({MAKE_VARIABLE})\n"
        self.assertEqual(text.count(use), len(MAKE_TARGETS))
        self.assertEqual(
            makefile_problems(text.replace(use, "\n")),
            [f"Makefile: {target}: no --timeout-multiplier" for target in MAKE_TARGETS],
        )
        self.assertEqual(
            makefile_problems(text.replace(f"{MAKE_VARIABLE} := 3", f"{MAKE_VARIABLE} := 1")),
            [f"Makefile: {MAKE_VARIABLE} is not 3"],
        )

    def test_the_multiplier_is_the_documented_one(self) -> None:
        docs = (ROOT / "docs" / "development" / "ci.md").read_text(encoding="utf-8")
        stated = f"`--timeout-multiplier {MULTIPLIER}`" in docs
        self.assertTrue(stated, "docs/development/ci.md does not state the multiplier in use")


if __name__ == "__main__":
    unittest.main()
