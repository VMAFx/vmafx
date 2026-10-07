#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The pull-request release legs are required contexts (ADR-1687).

ADR-1595 gave the tester image, the Windows tester zip and the release workflows a
pull-request run; ADR-1687 makes those runs block a merge through the Required
Checks Aggregator:

* `Tester Image` and `Windows Tester Zip` are planner -> work -> gate contexts
  (ADR-1140 / BUG-098): the workflow starts on every pull request and master push,
  the impact planner selects the build from the inputs the former trigger path
  filters listed, and the gate passes an unselected run without work;
* `Release Dry Run` routes its three groups through
  scripts/ci/release-dry-run-plan.sh and reports on every pull request; it has no
  push trigger, so the aggregator requires it on a pull request only.

`release_leg_problems()` is the contract; the mutation tests plant each defect it
must refuse. The aggregator itself is executed through the shared harness.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import sys
import unittest
from pathlib import Path
from types import ModuleType

import yaml  # type: ignore[import-untyped]

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci.required_aggregator_harness import run_required_aggregator
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"
AGGREGATOR = WORKFLOWS / "required-aggregator.yml"
CONFIG = ROOT / ".github" / "ci-impact.json"
DRY_RUN_PLAN = ROOT / "scripts" / "ci" / "release-dry-run-plan.sh"
PLANNER = ROOT / "scripts" / "ci" / "plan-ci-impact.py"
BASH = shutil.which("bash") or "/bin/bash"

# workflow -> (gate job id, required context, jobs the gate needs)
GATES = {
    # ADR-2169: every gate also needs the tier and runs only in the full tier.
    "docker-publish-tester.yml": ("tester-image", "Tester Image", ("tier", "impact", "build")),
    "windows-tester-bundle.yml": (
        "windows-tester-zip",
        "Windows Tester Zip",
        ("tier", "impact", "verify"),
    ),
    "release-dry-run.yml": ("gate", "Release Dry Run", ("tier", "plan", "images", "gpu", "mcp")),
}
PULL_REQUEST_ONLY = {"Release Dry Run"}
OWN_INPUT_LANES = {
    lane["context"]
    for lane in json.loads((ROOT / ".github" / "ci-tier.json").read_text("utf-8"))[
        "own_input_lanes"
    ]
}

# The `paths:` lists the two tester workflows carried on their `pull_request` and
# `push` triggers before ADR-1687, in the planner's spelling (fnmatch `*` crosses
# `/`, so `X/**` becomes the configuration's usual `X/*` + `X/**` pair).
FORMER_TRIGGER_PATHS = {
    "tester_image": {
        "docker/Dockerfile.tester",
        "tools/rc1-tester/*",
        "tools/rc1-tester/**",
        "scripts/ci/install-intel-ocloc.sh",
        "scripts/ci/install-cuda-toolkit.sh",
        "scripts/ci/install-rocm-from-image.sh",
        "build-config.env",
        "dev/scripts/fetch-intel-neo.py",
        "python/requirements-test-lock.txt",
        "REUSE.toml",
        "LICENSES/*",
        "LICENSES/**",
        "docs/hardware-reports/report.schema.json",
        ".github/workflows/docker-publish-tester.yml",
    },
    "windows_tester_zip": {
        ".github/workflows/windows-tester-bundle.yml",
        "scripts/ci/build-windows-tester-bundle.py",
        "requirements/locks/windows-tester-zip.txt",
        "scripts/ci/check-windows-bundle-imports.py",
        "tools/rc1-tester/image/windows/*",
        "tools/rc1-tester/image/windows/**",
        "tools/rc1-tester/image/unit-tests-windows.txt",
        "tools/rc1-tester/image/cuda-tests.txt",
        "tools/rc1-tester/image/sycl-tests.txt",
        "tools/rc1-tester/image/sycl-runtime-windows.json",
    },
}


def _js_block(source: str, pattern: str) -> str | None:
    match = re.search(pattern, source, re.DOTALL)
    return None if match is None else match.group(1)


def js_array(source: str, name: str) -> set[str]:
    """The single-quoted literals of `const <name> = [...]`, comment lines removed."""
    block = _js_block(source, rf"const {re.escape(name)} = \[(.*?)\];")
    if block is None:
        return set()
    return set(re.findall(r"'([^']+)'", re.sub(r"(?m)^\s*//.*$", "", block)))


def delayed_dependencies(source: str) -> dict[str, list[str]]:
    block = _js_block(source, r"const delayedStrictDependencies = (\{.*?\});")
    if block is None:
        return {}
    loaded: dict[str, list[str]] = json.loads(block)
    return loaded


def job_name_patterns(workflow_text: str) -> list[re.Pattern[str]]:
    """Each job display name of a workflow as a regex; a matrix expression matches anything."""
    patterns = []
    for job in (yaml.safe_load(workflow_text).get("jobs") or {}).values():
        name = str(job.get("name", ""))
        parts = re.split(r"\$\{\{[^}]*\}\}", name)
        patterns.append(re.compile("^" + ".+".join(re.escape(p) for p in parts) + "$"))
    return patterns


def _gate_problems(workflow: str, text: str) -> list[str]:
    gate_id, context, needs = GATES[workflow]
    jobs = yaml.safe_load(text).get("jobs") or {}
    gate = jobs.get(gate_id)
    if gate is None:
        return [f"{workflow}: no gate job {gate_id}"]
    problems = []
    if gate.get("name") != context:
        problems.append(f"{workflow}: gate {gate_id} is not named {context!r}")
    # ADR-2198: an own-input lane of .github/ci-tier.json runs in the light tier too.
    tier = "light" if context in OWN_INPUT_LANES else "full"
    if gate.get("if") != f"always() && needs.tier.outputs.{tier} == 'true'":
        problems.append(f"{workflow}: gate {gate_id} does not run always() in the {tier} tier")
    if tuple(gate.get("needs") or ()) != needs:
        problems.append(f"{workflow}: gate {gate_id} needs {gate.get('needs')}, not {list(needs)}")
    block = re.search(rf"(?ms)^  {re.escape(gate_id)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", text)
    if block is None or not re.search(
        rf"(?m)^    # required-aggregator\n    name: {re.escape(context)}$", block.group(1)
    ):
        problems.append(f"{workflow}: gate {gate_id} lacks its # required-aggregator marker")
    return problems


def release_leg_problems(aggregator: str, workflows: dict[str, str]) -> list[str]:
    """Every way the three release legs fail to be required, or [] when they are."""
    problems: list[str] = []
    required = js_array(aggregator, "required")
    strict = js_array(aggregator, "strictMustReport")
    pull_request_only = js_array(aggregator, "pullRequestOnly")
    delayed = delayed_dependencies(aggregator)
    for workflow, (_gate_id, context, _needs) in GATES.items():
        if context not in required:
            problems.append(f"{context}: missing from required")
        if context not in strict:
            problems.append(f"{context}: missing from strictMustReport")
        if (context in PULL_REQUEST_ONLY) != (context in pull_request_only):
            problems.append(f"{context}: pullRequestOnly membership is wrong")
        dependencies = delayed.get(context)
        if not dependencies:
            problems.append(f"{context}: no delayedStrictDependencies entry")
        else:
            names = job_name_patterns(workflows[workflow])
            for dependency in dependencies:
                if not any(p.match(dependency) for p in names):
                    problems.append(f"{context}: {dependency!r} is no job of {workflow}")
        problems.extend(_gate_problems(workflow, workflows[workflow]))
    if "if (context.eventName !== 'pull_request')" not in aggregator:
        problems.append("pullRequestOnly is not applied to runs that are not pull requests")
    return problems


def current_files() -> tuple[str, dict[str, str]]:
    workflows = {name: (WORKFLOWS / name).read_text(encoding="utf-8") for name in GATES}
    return AGGREGATOR.read_text(encoding="utf-8"), workflows


def _required_block_replace(aggregator: str, array: str, old: str, new: str) -> str:
    match = re.search(rf"const {re.escape(array)} = \[(.*?)\];", aggregator, re.DOTALL)
    if match is None or old not in match.group(1):
        raise AssertionError(f"{old!r} not in {array}")
    start, end = match.span(1)
    return aggregator[:start] + match.group(1).replace(old, new, 1) + aggregator[end:]


def aggregator_mutations(aggregator: str) -> dict[str, str]:
    """Each way of removing one release leg from the aggregator, by label."""
    mutations = {}
    for _gate, context, _needs in GATES.values():
        literal = f"'{context}',"
        mutations[f"{context} dropped from required"] = _required_block_replace(
            aggregator, "required", literal, ""
        )
        mutations[f"{context} dropped from strictMustReport"] = _required_block_replace(
            aggregator, "strictMustReport", literal, ""
        )
        mutations[f"{context} dropped from delayedStrictDependencies"] = aggregator.replace(
            f'"{context}": [', f'"{context} (gone)": [', 1
        )
    mutations["Release Dry Run dropped from pullRequestOnly"] = aggregator.replace(
        "const pullRequestOnly = ['Release Dry Run'];", "const pullRequestOnly = [];", 1
    )
    mutations["pullRequestOnly never applied"] = aggregator.replace(
        "if (context.eventName !== 'pull_request')", "if (false)", 1
    )
    return mutations


# label -> (workflow, old text, new text): each breaks one gate job.
WORKFLOW_MUTATIONS = {
    "tester gate waits only for the plan": (
        "docker-publish-tester.yml",
        "needs: [tier, impact, build]",
        "needs: [tier, impact]",
    ),
    "tester gate renamed": (
        "docker-publish-tester.yml",
        "    name: Tester Image\n",
        "    name: Tester Image (amd64)\n",
    ),
    "Windows gate marker removed": (
        "windows-tester-bundle.yml",
        "    # required-aggregator\n    name: Windows Tester Zip\n",
        "    name: Windows Tester Zip\n",
    ),
    "dry-run gate skips on failure": (
        "release-dry-run.yml",
        "needs: [tier, plan, images, gpu, mcp]\n    if: always() && needs.tier.outputs.full == 'true'",
        "needs: [tier, plan, images, gpu, mcp]\n    if: success()",
    ),
}


class Contract(unittest.TestCase):
    def test_the_three_legs_are_required(self) -> None:
        aggregator, workflows = current_files()
        self.assertEqual(release_leg_problems(aggregator, workflows), [])

    def test_each_planted_aggregator_defect_is_refused(self) -> None:
        aggregator, workflows = current_files()
        for label, mutated in aggregator_mutations(aggregator).items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, aggregator, "mutation did not apply")
                self.assertTrue(release_leg_problems(mutated, workflows))

    def test_each_planted_workflow_defect_is_refused(self) -> None:
        aggregator, workflows = current_files()
        for label, (name, old, new) in WORKFLOW_MUTATIONS.items():
            with self.subTest(mutation=label):
                self.assertEqual(workflows[name].count(old), 1, "mutation did not apply")
                mutated = {**workflows, name: workflows[name].replace(old, new, 1)}
                self.assertTrue(release_leg_problems(aggregator, mutated))


class AggregatorRun(unittest.TestCase):
    """The real Actions JavaScript, with one gate result chosen per run."""

    def test_a_pull_request_needs_every_gate_to_report_success(self) -> None:
        for context in sorted(c for _g, c, _n in GATES.values()):
            with self.subTest(context=context):
                self.assertEqual(run_required_aggregator(context, "success"), [])
                for conclusion in (None, "skipped", "neutral", "failure", "cancelled"):
                    failures = run_required_aggregator(context, conclusion)
                    self.assertEqual(len(failures), 1, conclusion)
                    self.assertIn(f"{context}: ", failures[0])
                    self.assertIn("(strict required context)", failures[0])

    def test_the_harness_reads_every_required_name(self) -> None:
        # An apostrophe in a comment of the required block ("praetor's") once hid
        # every name after it from the harness, Go API Compatibility first.
        failures = run_required_aggregator("Go API Compatibility", "failure")
        self.assertEqual(len(failures), 1)
        self.assertIn("Go API Compatibility: failure", failures[0])

    def test_a_master_push_needs_the_tester_gates_but_not_the_dry_run(self) -> None:
        self.assertEqual(run_required_aggregator("Release Dry Run", None, event="push"), [])
        for context in ("Tester Image", "Windows Tester Zip"):
            with self.subTest(context=context):
                self.assertEqual(run_required_aggregator(context, "success", event="push"), [])
                failures = run_required_aggregator(context, None, event="push")
                self.assertEqual(len(failures), 1)
                self.assertIn(f"{context}: never reported (strict required context)", failures[0])


def _load_planner() -> ModuleType:
    spec = importlib.util.spec_from_file_location("plan_ci_impact", PLANNER)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load {PLANNER}")
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolve the module through sys.modules; register first.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


planner = _load_planner()


def _plan_for(paths: list[str]) -> dict[str, bool]:
    """The planner's verdict for a pull request that modifies `paths`."""
    config = planner.load_config(CONFIG)
    changes = tuple(planner.Change(status="M", paths=(p,)) for p in paths)
    plan = planner.build_plan(config, changes, None, "b" * 40, "h" * 40)
    selectors: dict[str, bool] = dict(plan.selectors)
    selectors["__full__"] = plan.mode == "full"
    return selectors


def _dry_run_groups(paths: list[str]) -> dict[str, str]:
    result = run_command(
        [BASH, str(DRY_RUN_PLAN)],
        allowed_executables=(BASH,),
        input_data="".join(f"{p}\n" for p in paths),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=60,
    )
    return dict(line.split("=", 1) for line in result.stdout.split())


# Inputs the tester builds gained after ADR-1687: NOTICE holds the
# BSD-2-Clause-Patent text the image's licence stages bind-mount (ADR-1699).
# What the x64 SYCL zip reads and the x64 zip does not (ADR-2198), derived from the leg:
# sycl-rows.json (`stage_sycl`), prepare_build.py (`stage`, `twins`, `intel-runtime`),
# build-config.env (the oneAPI and Level Zero pins of the install steps) and the scratch
# ratchet list the scratch audit reads.
SYCL_ONLY_INPUTS = {
    "tools/rc1-tester/image/sycl-rows.json",
    "tools/rc1-tester/image/prepare_build.py",
    "build-config.env",
    "core/src/sycl/scratch_ratchet.txt",
}
ADDED_TRIGGER_PATHS = {"tester_image": {"NOTICE"}, "windows_tester_zip": set()}


class Routing(unittest.TestCase):
    def test_selectors_are_the_former_trigger_path_filters(self) -> None:
        selectors = json.loads(CONFIG.read_text(encoding="utf-8"))["selectors"]
        for name, expected in FORMER_TRIGGER_PATHS.items():
            with self.subTest(selector=name):
                self.assertEqual(
                    set(selectors[name]["patterns"]), expected | ADDED_TRIGGER_PATHS[name]
                )
                self.assertNotIn("inherits", selectors[name])

    def test_the_sycl_selector_is_the_zip_selector_plus_what_only_the_sycl_leg_reads(self) -> None:
        """ADR-2198: a change the x64 zip reads also builds the SYCL zip; the extras are SYCL's."""
        selectors = json.loads(CONFIG.read_text(encoding="utf-8"))["selectors"]
        zip_patterns = set(selectors["windows_tester_zip"]["patterns"])
        sycl_patterns = set(selectors["windows_tester_zip_sycl"]["patterns"])
        self.assertLess(zip_patterns, sycl_patterns)
        self.assertEqual(sycl_patterns - zip_patterns, SYCL_ONLY_INPUTS)
        self.assertNotIn("inherits", selectors["windows_tester_zip_sycl"])

    def test_a_change_only_the_sycl_leg_reads_selects_the_sycl_zip_alone(self) -> None:
        for path in sorted(SYCL_ONLY_INPUTS):
            with self.subTest(path=path):
                plan = _plan_for([path])
                self.assertFalse(plan["windows_tester_zip"])
                self.assertTrue(plan["windows_tester_zip_sycl"])

    def test_a_shared_input_selects_both_zips(self) -> None:
        plan = _plan_for(["scripts/ci/check-windows-bundle-imports.py"])
        self.assertTrue(plan["windows_tester_zip"])
        self.assertTrue(plan["windows_tester_zip_sycl"])

    def test_a_notice_change_runs_the_tester_image(self) -> None:
        """The image reads NOTICE for its BSD-2-Clause-Patent text (ADR-1699)."""
        plan = _plan_for(["NOTICE"])
        self.assertTrue(plan["tester_image"])
        self.assertFalse(plan["windows_tester_zip"])

    def test_docs_only_change_runs_no_tester_build_and_reports_the_dry_run(self) -> None:
        paths = ["docs/development/ci.md", "changelog.d/changed/x.md"]
        plan = _plan_for(paths)
        self.assertFalse(plan["__full__"])
        self.assertFalse(plan["tester_image"])
        self.assertFalse(plan["windows_tester_zip"])
        # The dry run starts and its gate reports; no group needs a build.
        self.assertEqual(
            _dry_run_groups(paths), {"images": "false", "gpu": "false", "mcp": "false"}
        )

    def test_tester_dockerfile_change_runs_the_tester_image_only(self) -> None:
        plan = _plan_for(["docker/Dockerfile.tester"])
        self.assertFalse(plan["__full__"])
        self.assertTrue(plan["tester_image"])
        self.assertFalse(plan["windows_tester_zip"])

    def test_windows_zip_input_runs_both_tester_builds(self) -> None:
        plan = _plan_for(["tools/rc1-tester/image/windows/run.cmd"])
        self.assertTrue(plan["tester_image"])  # tools/rc1-tester/** feeds the image too
        self.assertTrue(plan["windows_tester_zip"])

    def test_workflow_edit_is_a_full_plan_that_runs_only_its_own_build(self) -> None:
        for workflow, own in (
            ("docker-publish-tester.yml", "tester_image"),
            ("windows-tester-bundle.yml", "windows_tester_zip"),
        ):
            with self.subTest(workflow=workflow):
                plan = _plan_for([f".github/workflows/{workflow}"])
                self.assertTrue(plan["__full__"])
                self.assertTrue(plan[own])
                other = ({"tester_image", "windows_tester_zip"} - {own}).pop()
                self.assertFalse(plan[other])

    def test_ci_script_change_is_a_full_plan_without_tester_builds(self) -> None:
        # ADR-1700: own_paths_only; the fallback alone never selects them.
        plan = _plan_for(["scripts/ci/check-aggregator-names.sh"])
        self.assertTrue(plan["__full__"])
        self.assertFalse(plan["tester_image"])
        self.assertFalse(plan["windows_tester_zip"])
        self.assertTrue(plan["c_core"])


class WorkChains(unittest.TestCase):
    """What lets each gate need a single job."""

    def test_tester_build_needs_every_pull_request_job_before_it(self) -> None:
        jobs = yaml.safe_load((WORKFLOWS / "docker-publish-tester.yml").read_text())["jobs"]
        self.assertEqual(jobs["build"]["needs"], ["validate", "refs-x86"])
        self.assertEqual(jobs["refs-x86"]["needs"], "validate")
        self.assertNotIn("if", jobs["build"])
        self.assertNotIn("if", jobs["refs-x86"])

    def test_windows_verify_implies_its_build(self) -> None:
        jobs = yaml.safe_load((WORKFLOWS / "windows-tester-bundle.yml").read_text())["jobs"]
        last = jobs["build"]["steps"][-1]
        self.assertTrue(last.get("uses", "").startswith("actions/upload-artifact@"))
        self.assertNotIn("if", last)
        self.assertEqual(last["with"]["if-no-files-found"], "error")
        self.assertEqual(jobs["verify"]["needs"], ["validate", "build"])
        self.assertIn("needs.validate.result == 'success'", jobs["verify"]["if"])
        download = next(
            s for s in jobs["verify"]["steps"] if "download-artifact" in s.get("uses", "")
        )
        self.assertEqual(download["with"]["name"], last["with"]["name"])


class DryRunGateScript(unittest.TestCase):
    """The gate passes only a successful plan with every group in a valid state."""

    def _run(self, plan: str, states: dict[str, tuple[str, str]]) -> bool:
        gate = yaml.safe_load((WORKFLOWS / "release-dry-run.yml").read_text())["jobs"]["gate"]
        script = gate["steps"][0]["run"]
        env = {**os.environ, "PLAN_RESULT": plan}
        for group, (selected, result) in states.items():
            env[f"{group}_SELECTED"] = selected
            env[f"{group}_RESULT"] = result
        completed = run_command(
            [BASH, "-eu", "-o", "pipefail", "-c", script],
            allowed_executables=(BASH,),
            capture_output=True,
            text=True,
            env=env,
            timeout_seconds=60,
            check=False,
        )
        return completed.returncode == 0

    def test_each_group_state_with_the_others_valid(self) -> None:
        idle = {
            "IMAGES": ("false", "skipped"),
            "GPU": ("false", "skipped"),
            "MCP": ("false", "skipped"),
        }
        for plan in ("success", "failure", "skipped", "cancelled"):
            for group in idle:
                for selected in ("true", "false", ""):
                    for result in ("success", "skipped", "failure", "cancelled"):
                        expected = plan == "success" and (selected, result) in {
                            ("true", "success"),
                            ("false", "skipped"),
                        }
                        with self.subTest(plan=plan, group=group, state=(selected, result)):
                            states = {**idle, group: (selected, result)}
                            self.assertEqual(self._run(plan, states), expected)


if __name__ == "__main__":
    unittest.main()
