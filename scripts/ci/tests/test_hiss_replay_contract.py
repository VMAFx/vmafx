#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the fail-closed HISS-20/21 replay wiring from ADR-1274."""

from __future__ import annotations

import json
import re
import shutil
import sys
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
SUBPROCESS_TIMEOUT_S = 120
STRICT_CONTEXTS = {
    "Standards & Invariant Verification Gate",
    "HISS Replay Evidence (Linux)",
    "HISS Replay Evidence (macOS)",
    "HISS Replay Evidence (Windows)",
}

# ADR-1297 extended the same fail-closed contract to the gates it promoted that
# have no trigger path filter, no conditional skip, and report on both
# `pull_request` and `push` to master. For these, "the check never appeared" is
# a broken workflow or a vanished runner, not an ADR-0313 path skip. Kept as a
# separate set so the ADR-1274 pin above stays an exact statement about the
# replay contexts.
ADR_1297_STRICT_CONTEXTS = {
    "Coverage Gate",
    "MCP Smoke",
    "Markdown Lint",
    "No Conflict Markers",
    "Tiny-Model Registry Validate",
    # The SYCL path decision is step-level. The job itself always reports on
    # eligible PRs and master pushes, so absence cannot mean path-filter skip.
    "Tidy SYCL",
    "Windows ARM64 MSVC",
}

# BUG-098 replaced trigger-level path filters on these required consumer
# workflows with unconditional planner -> work -> gate jobs. They now report on
# every pull request and master push, so absence is a registration failure, not
# an ADR-0313 not-applicable result.
BUG_098_STRICT_CONTEXTS = {
    "Linux Intel LLVM",
    "macOS Clang+Metal",
    "Windows MSVC+CUDA (full)",
    "FFmpeg Ubuntu gcc",
    "FFmpeg macOS clang",
    "FFmpeg SYCL",
    "Docker Image Build",
    "Dev Container Build",
    "vmafx-sys CI",
    "cargo-deny",
    "helm lint + template",
    "Doxygen Public API",
}

BUG_098_GATE_DEPENDENCIES = {
    # ADR-1319: self-hosted checks register only after hosted admission probes.
    "Coverage GPU": ["Probe GPU Full Runner"],
    "SYCL Parity (Arc A380)": ["Probe SYCL Runner"],
    "Linux Intel LLVM": [
        "Plan build impact",
        "Linux Intel LLVM work",
        "macOS Clang+Metal work",
        "Windows MSVC+CUDA (full) work",
    ],
    "macOS Clang+Metal": [
        "Plan build impact",
        "Linux Intel LLVM work",
        "macOS Clang+Metal work",
        "Windows MSVC+CUDA (full) work",
    ],
    "Windows MSVC+CUDA (full)": [
        "Plan build impact",
        "Linux Intel LLVM work",
        "macOS Clang+Metal work",
        "Windows MSVC+CUDA (full) work",
    ],
    "FFmpeg Ubuntu gcc": [
        "Plan FFmpeg impact",
        "FFmpeg Ubuntu gcc work",
        "FFmpeg macOS clang work",
    ],
    "FFmpeg macOS clang": [
        "Plan FFmpeg impact",
        "FFmpeg Ubuntu gcc work",
        "FFmpeg macOS clang work",
    ],
    "FFmpeg SYCL": ["Plan FFmpeg impact", "FFmpeg SYCL work"],
    "Docker Image Build": ["Plan Docker image impact", "Docker Image Build work"],
    "Dev Container Build": ["Plan dev-container impact", "Dev Container Build work"],
    "vmafx-sys CI": ["Plan Rust impact", "vmafx-sys CI work"],
    "cargo-deny": ["Plan Rust impact", "cargo-deny work"],
    "helm lint + template": ["Plan Helm impact", "helm lint + template work"],
    "Doxygen Public API": ["Plan Doxygen impact", "Doxygen Public API work"],
}


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def javascript_array(source: str, name: str) -> set[str]:
    match = re.search(rf"const {re.escape(name)} = \[(.*?)\];", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing JavaScript array: {name}")
    return set(re.findall(r"'([^']+)'", match.group(1)))


def javascript_json_object(source: str, name: str) -> object:
    match = re.search(rf"const {re.escape(name)} = (\{{.*?\}});", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing JavaScript object: {name}")
    return json.loads(match.group(1))


def run_aggregator_scenario(mode: str) -> list[str]:
    workflow = read(".github/workflows/required-aggregator.yml")
    script = textwrap.dedent(workflow.split("          script: |\n", 1)[1])
    required = sorted(javascript_array(script, "required"))
    node = shutil.which("node")
    if node is None:
        raise AssertionError("Node.js is needed to exercise the Actions JavaScript")
    driver = r"""
const input = JSON.parse(require('fs').readFileSync(0, 'utf8'));
const origin = Date.now(); let clock = origin; let poll = 0;
class VirtualDate extends Date { static now() { return clock; } }
const completed = name => ({name, status: 'completed', conclusion: 'success',
  started_at: new Date(origin).toISOString(), completed_at: new Date(origin).toISOString()});
const target = 'Dev Container Build';
const baseline = input.required.filter(name => name !== target).map(completed);
const noise = Array.from({length: 100 - baseline.length}, (_, i) => completed(`noise-${i}`));
const github = {rest: {
  actions: {getWorkflowRun: async () => ({data: {created_at: new Date(origin).toISOString()}})},
  checks: {listForRef: async ({page = 1}) => {
    if (input.mode === 'pagination') {
      return {data: {check_runs: page === 1 ? baseline.concat(noise) :
        page === 2 ? [completed(target)] : []}};
    }
    if (page !== 1) return {data: {check_runs: []}};
    poll += 1;
    if (poll <= 6) {
      return {data: {check_runs: baseline.concat({name: 'Dev Container Build work',
        status: 'in_progress', conclusion: null, started_at: new Date(origin).toISOString(),
        completed_at: null})}};
    }
    return {data: {check_runs: baseline.concat(
      completed(target), completed('Dev Container Build work'))}};
  }},
}};
const context = {eventName: 'pull_request', repo: {owner: 'test', repo: 'test'},
  payload: {pull_request: {head: {ref: 'fix/example', sha: 'abc'}}}};
const failures = [];
const core = {info: () => {}, setFailed: message => failures.push(message)};
const AsyncFunction = Object.getPrototypeOf(async function(){}).constructor;
new AsyncFunction('github', 'context', 'core', 'process', 'Date', 'setTimeout', input.script)(
  github, context, core, {env: {GITHUB_RUN_ID: '1'}}, VirtualDate,
  callback => { clock += 30000; callback(); }
).then(() => process.stdout.write(JSON.stringify(failures)))
 .catch(error => { console.error(error); process.exitCode = 1; });
"""
    result = run_command(
        [node, "-e", driver],
        allowed_executables=(node,),
        input_data=json.dumps({"script": script, "required": required, "mode": mode}),
        text=True,
        capture_output=True,
        check=True,
        timeout_seconds=SUBPROCESS_TIMEOUT_S,
    )
    payload: object = json.loads(result.stdout)
    if not isinstance(payload, list) or not all(isinstance(item, str) for item in payload):
        raise AssertionError("aggregator driver returned malformed failures")
    return [str(item) for item in payload]


class HissReplayContractTests(unittest.TestCase):
    def test_catalog_and_local_entrypoints_exist(self) -> None:
        catalog = read(".config/hiss/coverage.yaml")
        self.assertIn("# Replayable enforcement evidence", catalog)
        self.assertRegex(catalog, r"(?m)^version: 1$")
        self.assertGreaterEqual(catalog.count("  - id: HISS-"), 6)

        makefile = read("Makefile")
        self.assertRegex(makefile, r"(?m)^verify-all:\n\t.*hiss coverage --verify$")
        self.assertRegex(makefile, r"(?m)^hiss-coverage:\n\t.*hiss coverage --verify$")

        hooks = read("lefthook.yml")
        self.assertEqual(hooks.count("    hiss-evidence:\n"), 2)
        self.assertGreaterEqual(hooks.count("hiss coverage --verify"), 2)

    def test_platform_workflow_always_reports_all_replay_contexts(self) -> None:
        workflow = read(".github/workflows/standards-gate.yml")
        self.assertNotRegex(workflow, r"(?m)^\s+paths(?:-ignore)?:")
        self.assertIn("name: Standards & Invariant Verification Gate", workflow)
        self.assertIn("name: HISS Replay Evidence (${{ matrix.name }})", workflow)
        self.assertEqual(workflow.count("standardsctl hiss coverage --verify"), 2)
        for name, runner in (
            ("Linux", "ubuntu-26.04"),
            ("macOS", "macos-15"),
            ("Windows", "windows-2025"),
        ):
            self.assertIn(f"- name: {name}\n            os: {runner}", workflow)

    def test_aggregator_rejects_absent_skipped_or_neutral_replay(self) -> None:
        aggregator = read(".github/workflows/required-aggregator.yml")
        required = javascript_array(aggregator, "required")
        strict = javascript_array(aggregator, "strictMustReport")
        self.assertEqual(
            strict,
            STRICT_CONTEXTS | ADR_1297_STRICT_CONTEXTS | BUG_098_STRICT_CONTEXTS,
        )
        self.assertTrue(strict >= STRICT_CONTEXTS)
        self.assertTrue(strict <= required)
        self.assertIn("if (strictMustReport.includes(name))", aggregator)
        self.assertIn("if (!run)", aggregator)
        self.assertIn("run.conclusion !== 'success'", aggregator)
        self.assertIn("never reported (strict required context)", aggregator)

    def test_delayed_gate_dependencies_match_the_converted_workflows(self) -> None:
        aggregator = read(".github/workflows/required-aggregator.yml")
        dependencies = javascript_json_object(aggregator, "delayedStrictDependencies")
        self.assertEqual(dependencies, BUG_098_GATE_DEPENDENCIES)

    def test_aggregator_waits_for_gate_behind_running_work(self) -> None:
        self.assertEqual(run_aggregator_scenario("delayed-gate"), [])

    def test_aggregator_reads_check_runs_beyond_the_first_page(self) -> None:
        self.assertEqual(run_aggregator_scenario("pagination"), [])

    def test_public_claim_matches_the_enforced_revision(self) -> None:
        readme = read("README.md")
        agents = read("AGENTS.md")
        declared_revisions = {int(value) for value in re.findall(r"HISS-(\d+)", agents)}
        self.assertTrue(declared_revisions)
        enforced_revision = max(declared_revisions)

        self.assertIn(f"HISS--{enforced_revision}", readme)
        self.assertIn(f"(HISS-{enforced_revision})", readme)
        advertised_revisions = {int(value) for value in re.findall(r"HISS-(?:-)?(\d+)", readme)}
        self.assertEqual(advertised_revisions, {enforced_revision})


if __name__ == "__main__":
    unittest.main()
