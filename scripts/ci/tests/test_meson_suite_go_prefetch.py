#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""A CI job that runs the Meson suite downloads the Go modules before it.

Four tests of the suite run Go tooling. On a cold module cache they downloaded
what they needed inside the test, once: on 2026-10-10 a dropped stream of the
module proxy failed test_crd_compat and with it a required sanitizer job
(T-CI-GO-MODULES-DOWNLOADED-IN-TESTS-2026-10-10).

The contract:

- every step that runs scripts/ci/run_meson_test.py over the fast suite or the
  whole suite has a step before it, in the same job, that runs
  scripts/ci/prefetch_go_modules.py under the same `if`, in the same shell and
  with the same sudo form. The module cache and the tool cache belong to a
  user, so a prefetch as another user fills a cache the suite never reads;
- the scripts the suite's tests run launch Go only where
  prefetch_go_modules.GO_TOOL_TESTS says, and never through `go run
  <package>@<version>` alone, which asks the proxy on every run.
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
PREFETCH_SCRIPT = "prefetch_go_modules.py"
RUNNER = "run_meson_test.py"
SUDO = re.compile(r"(?:^|\s)sudo(\s+-\S+)*")
# Scripts a Meson test runs (core/test/meson.build names them) that start Go.
GO_LAUNCH = re.compile(r"""shutil\.which\(["']go["']\)|\[\s*["']go["']\s*,""")
TEST_SCRIPT_DIRS = ("scripts/codegen", "core/test")
GO_SCRIPTS = {
    "scripts/codegen/crd_generate.py": ("test_crd_generated_current", "test_crd_compat"),
    "scripts/codegen/proto_generate.py": ("test_proto_generated_current",),
    "core/test/run_score_contract.py": ("test_vmafx_score_contract",),
}


def load(path: Path, name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SUITE = load(Path(__file__).with_name("test_meson_suite_checkout_history.py"), "suite_steps")
PREFETCH = load(ROOT / "scripts" / "ci" / PREFETCH_SCRIPT, "prefetch_go_modules")


def sudo_form(run: str, script: str) -> str:
    """How the command that runs `script` is elevated: "", "sudo" or "sudo -E"."""
    for line in SUITE.CONTINUATION.sub(" ", run).splitlines():
        if script not in line or line.lstrip().startswith("#"):
            continue
        found = SUDO.search(line.split(script, 1)[0])
        if found is None:
            return ""
        return "sudo -E" if re.search(r"\s-\w*E", found.group(0)) else "sudo"
    return ""


def launch(step: dict[str, Any], script: str) -> tuple[str, str, str]:
    """What decides whose caches a step's Go commands use, and when it runs."""
    run = str(step.get("run", ""))
    return (str(step.get("if", "")), str(step.get("shell", "")), sudo_form(run, script))


def offenders(workflow: dict[str, Any], name: str) -> list[str]:
    out = []
    for job_id, job in (workflow.get("jobs") or {}).items():
        steps = job.get("steps") or []
        for index, step in enumerate(steps):
            if not SUITE.runs_suite(str(step.get("run", ""))):
                continue
            label = f"{name}: job {job_id}, step {step.get('name', index)!r}"
            before = [s for s in steps[:index] if PREFETCH_SCRIPT in str(s.get("run", ""))]
            if not before:
                out.append(f"{label}: no {PREFETCH_SCRIPT} step before it")
            elif launch(before[-1], PREFETCH_SCRIPT) != launch(step, RUNNER):
                out.append(
                    f"{label}: prefetch runs as {launch(before[-1], PREFETCH_SCRIPT)}, "
                    f"the suite as {launch(step, RUNNER)}"
                )
    return out


def all_offenders() -> list[str]:
    found = []
    for path in sorted(WORKFLOWS.glob("*.yml")):
        workflow = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        found += offenders(workflow, path.name)
    return found


def go_launching_scripts() -> set[str]:
    """The Python scripts of the suite's test directories that start Go."""
    found = set()
    for directory in TEST_SCRIPT_DIRS:
        for path in sorted((ROOT / directory).glob("*.py")):
            if GO_LAUNCH.search(path.read_text(encoding="utf-8")):
                found.add(path.relative_to(ROOT).as_posix())
    return found


# Planted jobs: the steps of a job, in order.
SUITE_RUN = "python3 scripts/ci/run_meson_test.py -- -C core/build --print-errorlogs"
PREFETCH_RUN = "python3 scripts/ci/prefetch_go_modules.py"
SUDO_SUITE_RUN = (
    'sudo "$(command -v python3)" scripts/ci/run_meson_test.py -- -C core/build --print-errorlogs'
)
KEEP_ENV_SUITE_RUN = (
    'sudo -E PATH="$PATH" \\\n  "$(command -v python3)" scripts/ci/run_meson_test.py \\\n'
    "  -- -C core/build"
)
IMPACTED = "steps.impact.outputs.c_core == 'true'"
REFUSED_JOBS: dict[str, Any] = {
    "none": {"steps": [{"run": SUITE_RUN}]},
    "after": {"steps": [{"run": SUITE_RUN}, {"run": PREFETCH_RUN}]},
    "as-another-user": {"steps": [{"run": PREFETCH_RUN}, {"run": SUDO_SUITE_RUN}]},
    "env-dropped": {"steps": [{"run": f"sudo {PREFETCH_RUN}"}, {"run": KEEP_ENV_SUITE_RUN}]},
    "other-condition": {"steps": [{"run": PREFETCH_RUN}, {"run": SUITE_RUN, "if": IMPACTED}]},
    "other-shell": {"steps": [{"run": PREFETCH_RUN}, {"run": SUITE_RUN, "shell": "cmd"}]},
    "commented": {"steps": [{"run": f"# {PREFETCH_RUN}"}, {"run": f"sudo {SUITE_RUN}"}]},
}
ACCEPTED_JOBS: dict[str, Any] = {
    "ok": {"steps": [{"run": PREFETCH_RUN}, {"run": "ninja -C build"}, {"run": SUITE_RUN}]},
    "ok-sudo": {
        "steps": [
            {"run": 'sudo "$(command -v python3)" scripts/ci/prefetch_go_modules.py'},
            {"run": SUDO_SUITE_RUN},
        ]
    },
    "ok-keep-env": {
        "steps": [
            {"run": f'sudo -E PATH="$PATH" {PREFETCH_RUN}', "if": IMPACTED},
            {"run": KEEP_ENV_SUITE_RUN, "if": IMPACTED},
        ]
    },
    "ok-windows": {
        "steps": [
            {"run": "python scripts\\ci\\prefetch_go_modules.py", "shell": "cmd"},
            {"run": "python scripts\\ci\\run_meson_test.py -- ^\n  --suite fast", "shell": "cmd"},
        ]
    },
    "not-the-suite": {
        "steps": [
            {"run": "python3 scripts/ci/run_meson_test.py -- -C b --suite rust"},
            {"run": "python3 scripts/ci/run_meson_test.py -- -C b test_mcp_smoke"},
        ]
    },
}


class MesonSuiteGoPrefetch(unittest.TestCase):
    def test_every_suite_step_has_a_prefetch_before_it(self) -> None:
        self.assertEqual(all_offenders(), [])

    def test_the_suite_steps_of_every_workflow_are_found(self) -> None:
        # The contract must look at something: these jobs run the suite.
        found = set()
        for path in sorted(WORKFLOWS.glob("*.yml")):
            workflow = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            for job_id, job in (workflow.get("jobs") or {}).items():
                if any(SUITE.runs_suite(str(s.get("run", ""))) for s in job.get("steps") or []):
                    found.add(f"{path.name}:{job_id}")
        for job in (
            "tests-and-quality-gates.yml:sanitizers",
            "tests-and-quality-gates.yml:coverage",
            "sanitizers.yml:asan-ubsan",
            "sanitizers.yml:tsan",
            "libvmaf-build-matrix.yml:windows-arm64",
            "libvmaf-build-matrix.yml:libvmaf-build",
        ):
            self.assertIn(job, found)

    def test_planted_jobs_without_a_matching_prefetch_are_refused(self) -> None:
        planted = {"jobs": {**REFUSED_JOBS, **ACCEPTED_JOBS}}
        refused = [line.split(": ", 2)[1].split(",")[0] for line in offenders(planted, "p.yml")]
        self.assertEqual(refused, [f"job {name}" for name in REFUSED_JOBS])
        self.assertEqual(offenders({"jobs": ACCEPTED_JOBS}, "p.yml"), [])
        # An accepted job is accepted for its prefetch, not for a suite step
        # the contract failed to see.
        for name, job in ACCEPTED_JOBS.items():
            seen = any(SUITE.runs_suite(step["run"]) for step in job["steps"])
            self.assertEqual(seen, name != "not-the-suite", name)

    def test_the_tests_that_launch_go_are_the_ones_the_prefetch_covers(self) -> None:
        # A new script that starts Go in a Meson test: make prefetch_go_modules.py
        # download what it needs, then list it in GO_SCRIPTS and GO_TOOL_TESTS.
        self.assertEqual(go_launching_scripts(), set(GO_SCRIPTS))
        named = [test for tests in GO_SCRIPTS.values() for test in tests]
        self.assertEqual(sorted(named), sorted(PREFETCH.GO_TOOL_TESTS))
        meson = (ROOT / "core" / "test" / "meson.build").read_text(encoding="utf-8")
        for script, tests in GO_SCRIPTS.items():
            self.assertIn(Path(script).name, meson)
            for test in tests:
                self.assertIn(f"test('{test}'", meson)

    def test_no_test_script_depends_on_go_run_of_a_versioned_package_alone(self) -> None:
        # `go run <package>@<version>` asks the module proxy for the module's
        # versions on every run. Only proto_generate.py may spell it, as the
        # fallback behind the copy the prefetch installs.
        versioned = re.compile(r"""["']run["']\s*,\s*f?["'][^"']*@""")
        for script in sorted(go_launching_scripts()):
            text = (ROOT / script).read_text(encoding="utf-8")
            if script == "scripts/codegen/proto_generate.py":
                self.assertRegex(text, versioned)
                self.assertIn("if buf_installed(root):", text)
            else:
                self.assertNotRegex(text, versioned, script)


if __name__ == "__main__":
    unittest.main()
