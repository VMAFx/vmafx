#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests for Rust CI impact routing, trigger registration, and gates.

The workflow must start for every pull request so its required contexts cannot
disappear. The shared impact planner decides whether the expensive Rust work
runs; public libvmaf headers remain an input because bindgen consumes them.
Workflows hosting required checks must not narrow pull_request to master-only,
ensuring stacked and non-master PRs register their gates.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Mapping

import yaml  # type: ignore[import-untyped]

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[3]
RUST_CI_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "rust-ci.yml"
CONFIG = REPO_ROOT / ".github" / "ci-impact.json"
PLANNER = REPO_ROOT / "scripts" / "ci" / "plan-ci-impact.py"
GIT = shutil.which("git") or "/usr/bin/git"

AFFECTED_REQUIRED_WORKFLOWS = (
    "rust-ci.yml",
    "doxygen-public-api.yml",
    "helm-chart.yml",
)


def validate_workflow_pr_registration(raw_yaml: str) -> None:
    """Validate that pull_request trigger is unrestricted for branch targets."""
    parsed = yaml.safe_load(raw_yaml)
    if not isinstance(parsed, dict):
        raise AssertionError("workflow YAML must parse to a dict")
    on = parsed.get("on") or parsed.get(True) or {}
    if "pull_request" not in on:
        raise AssertionError("pull_request trigger must be declared")
    pr = on["pull_request"]
    if not isinstance(pr, dict):
        raise AssertionError("pull_request trigger must be a mapping, not a branch list")
    if "branches" in pr:
        raise AssertionError(f"pull_request must not narrow branches: {pr['branches']}")
    if "branches-ignore" in pr:
        raise AssertionError(f"pull_request must not use branches-ignore: {pr['branches-ignore']}")
    if "paths" in pr:
        raise AssertionError(f"pull_request must not use paths filter: {pr['paths']}")
    if "paths-ignore" in pr:
        raise AssertionError(f"pull_request must not use paths-ignore: {pr['paths-ignore']}")
    expected_types = {"opened", "synchronize", "reopened", "ready_for_review"}
    actual_types = set(pr.get("types") or [])
    if not expected_types.issubset(actual_types):
        missing = expected_types - actual_types
        raise AssertionError(f"pull_request.types missing required types: {missing}")


def _setup_stacked_repo(repo_dir: str, env: Mapping[str, str]) -> tuple[str, str, str]:
    """Create master, feature/base, and two stacked feature branches."""

    def git(*args: str) -> str:
        return run_command(
            [GIT, "-C", repo_dir, *args],
            allowed_executables=(GIT,),
            capture_output=True,
            text=True,
            check=True,
            env=env,
            timeout_seconds=60,
        ).stdout.strip()

    git("init", "-q", "-b", "master")
    for directory in ("docs", "core/include/libvmaf", "bindings/rust", ".github"):
        (Path(repo_dir) / directory).mkdir(parents=True, exist_ok=True)
    (Path(repo_dir) / ".github" / "ci-impact.json").write_text(
        CONFIG.read_text(encoding="utf-8"), encoding="utf-8"
    )
    (Path(repo_dir) / "docs" / "intro.md").write_text("initial docs\n", encoding="utf-8")
    git("add", "-A")
    git("commit", "-q", "-m", "initial master commit")

    git("checkout", "-q", "-b", "feature/base")
    (Path(repo_dir) / "docs" / "base.md").write_text("base feature\n", encoding="utf-8")
    git("add", "-A")
    git("commit", "-q", "-m", "base feature commit")
    base_sha = git("rev-parse", "HEAD")

    git("checkout", "-q", "-b", "feature/stacked-rust")
    (Path(repo_dir) / "core" / "include" / "libvmaf" / "libvmaf.h").write_text(
        "int vmaf;\n", encoding="utf-8"
    )
    git("add", "-A")
    git("commit", "-q", "-m", "rust header commit")
    rust_sha = git("rev-parse", "HEAD")

    git("checkout", "-q", "feature/base")
    git("checkout", "-q", "-b", "feature/stacked-docs")
    (Path(repo_dir) / "docs" / "stacked.md").write_text("docs only\n", encoding="utf-8")
    git("add", "-A")
    git("commit", "-q", "-m", "stacked docs commit")
    docs_sha = git("rev-parse", "HEAD")

    return base_sha, rust_sha, docs_sha


class RustCIWorkflowContractTest(unittest.TestCase):
    """Ensure Rust work is routed in-job and its required checks always report."""

    def test_license_header_and_mechanical_classification_guard(self) -> None:
        test_file = Path(__file__).resolve()
        raw = test_file.read_text(encoding="utf-8")
        header = "\n".join(raw.splitlines()[:10])
        self.assertIn("# SPDX-License-Identifier: EUPL-1.2", header)
        self.assertNotIn("BSD-2-Clause-Patent", header)
        self.assertIn("# Copyright 2026 Lusoris", header)

        spec = importlib.util.spec_from_file_location(
            "relicense_fork_files", REPO_ROOT / "scripts/dev/relicense_fork_files.py"
        )
        self.assertIsNotNone(spec)
        assert spec is not None and spec.loader is not None
        mod = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = mod
        spec.loader.exec_module(mod)

        rel_path = test_file.relative_to(REPO_ROOT).as_posix()
        prov = mod.load_provenance(REPO_ROOT / "scripts/dev/relicense_provenance.toml", REPO_ROOT)
        up = mod.Upstream(frozenset(), frozenset(), frozenset())
        verdict_reason = mod.static_verdict(rel_path, raw, up, prov)
        self.assertIsNone(verdict_reason, f"static verdict rejected: {verdict_reason}")
        self.assertFalse(mod.is_foreign(mod.declarations(raw)))

        wanted = mod.wanted_content(
            REPO_ROOT, "upstream/master", prov, mod.Verdict(rel_path, "moves"), raw
        )
        self.assertEqual(raw, wanted, "classification guard required content changes")

        spdx_tag = "SPDX-License" + "-Identifier:"
        mutated_bsd = raw.replace(
            f"{spdx_tag} EUPL-1.2",
            f"{spdx_tag} BSD-2-Clause-Patent",
        )
        mutated_wanted = mod.wanted_content(
            REPO_ROOT, "upstream/master", prov, mod.Verdict(rel_path, "moves"), mutated_bsd
        )
        self.assertNotEqual(mutated_bsd, mutated_wanted, "classifier failed to catch BSD mutation")

    def test_rust_ci_has_no_workflow_path_filters(self) -> None:
        self.assertTrue(RUST_CI_WORKFLOW.exists(), f"missing {RUST_CI_WORKFLOW}")
        raw = RUST_CI_WORKFLOW.read_text(encoding="utf-8")
        parsed = yaml.safe_load(raw)

        on = parsed.get("on") or parsed.get(True) or {}
        for event_name in ("push", "pull_request"):
            event = on.get(event_name, {})
            with self.subTest(event=event_name):
                self.assertNotIn("paths", event)
                self.assertNotIn("paths-ignore", event)

    def test_affected_workflows_do_not_narrow_pull_request_branches(self) -> None:
        """Required workflows must not narrow pull_request branches to master only."""
        workflow_dir = REPO_ROOT / ".github" / "workflows"
        for name in AFFECTED_REQUIRED_WORKFLOWS:
            wf_path = workflow_dir / name
            self.assertTrue(wf_path.exists(), f"missing workflow {wf_path}")
            raw = wf_path.read_text(encoding="utf-8")
            with self.subTest(workflow=name):
                validate_workflow_pr_registration(raw)

    def test_adversarial_mutations_reject_pull_request_branches_narrowing(self) -> None:
        """Mutations introducing branches or paths to pull_request must fail validation."""
        workflow_dir = REPO_ROOT / ".github" / "workflows"
        mutations = (
            "    branches: [master]\n",
            '    branches: ["master"]\n',
            "    branches: [main]\n",
            "    branches: [release/1.0]\n",
            "    branches-ignore: [temp]\n",
            '    paths: ["bindings/rust/**"]\n',
            '    paths-ignore: ["docs/**"]\n',
        )
        for name in AFFECTED_REQUIRED_WORKFLOWS:
            raw = (workflow_dir / name).read_text(encoding="utf-8")
            for mut in mutations:
                mutated = raw.replace("  pull_request:\n", f"  pull_request:\n{mut}")
                with self.subTest(workflow=name, mutation=mut.strip()):
                    with self.assertRaises(AssertionError):
                        validate_workflow_pr_registration(mutated)

            mutated_list = raw.replace(
                "  pull_request:\n    types: [opened, synchronize, reopened, ready_for_review]",
                "  pull_request: [master]",
            )
            with self.subTest(workflow=name, mutation="list-syntax"):
                with self.assertRaises(AssertionError):
                    validate_workflow_pr_registration(mutated_list)

    def test_rust_ci_uses_fail_closed_planner_work_gate_contract(self) -> None:
        parsed = yaml.safe_load(RUST_CI_WORKFLOW.read_text(encoding="utf-8"))
        jobs = parsed["jobs"]
        self.assertEqual(jobs["impact"]["outputs"]["selected"], "${{ steps.impact.outputs.rust }}")

        for work_job in ("rust-vmafx-sys-work", "cargo-deny-work"):
            with self.subTest(work_job=work_job):
                self.assertEqual(jobs[work_job]["needs"], "impact")
                self.assertIn("needs.impact.outputs.selected == 'true'", jobs[work_job]["if"])

        gates = {
            "vmafx-sys-gate": ("vmafx-sys CI", "rust-vmafx-sys-work"),
            "cargo-deny-gate": ("cargo-deny", "cargo-deny-work"),
        }
        for gate_job, (check_name, work_job) in gates.items():
            with self.subTest(gate_job=gate_job):
                gate = jobs[gate_job]
                self.assertEqual(gate["name"], check_name)
                self.assertEqual(gate["needs"], ["impact", work_job])
                self.assertEqual(gate["if"], "always()")
                script = gate["steps"][0]["run"]
                self.assertIn('if [ "$PLAN_RESULT" != success ]', script)
                self.assertIn("true:success|false:skipped", script)

    def test_ci_impact_json_rust_selector_includes_libvmaf_headers(self) -> None:
        self.assertTrue(CONFIG.exists(), f"missing {CONFIG}")
        config = json.loads(CONFIG.read_text(encoding="utf-8"))
        rust_patterns = config.get("selectors", {}).get("rust", {}).get("patterns", [])
        self.assertIn(
            "core/include/libvmaf/**",
            rust_patterns,
            "core/include/libvmaf/** missing from selectors.rust.patterns in ci-impact.json",
        )

    def test_stacked_and_non_master_target_pr_semantics(self) -> None:
        """Validate merge-base aware impact planning for stacked/non-master PRs."""
        with tempfile.TemporaryDirectory() as repo_dir, tempfile.TemporaryDirectory() as out_dir:
            env = {
                **os.environ,
                "GIT_AUTHOR_NAME": "t",
                "GIT_AUTHOR_EMAIL": "t@t",
                "GIT_COMMITTER_NAME": "t",
                "GIT_COMMITTER_EMAIL": "t@t",
            }
            base_sha, rust_sha, docs_sha = _setup_stacked_repo(repo_dir, env)

            cases = (
                ("out_rust", rust_sha, "true"),
                ("out_docs", docs_sha, "false"),
            )
            for out_name, head_sha, expected_rust in cases:
                out_path = Path(out_dir) / out_name
                run_command(
                    [
                        sys.executable,
                        str(PLANNER),
                        "--event",
                        "pull_request",
                        "--base",
                        base_sha,
                        "--head",
                        head_sha,
                        "--repo-root",
                        repo_dir,
                        "--github-output",
                        str(out_path),
                    ],
                    allowed_executables=(sys.executable,),
                    capture_output=True,
                    text=True,
                    check=True,
                    timeout_seconds=60,
                )
                kv = dict(line.split("=", 1) for line in out_path.read_text().splitlines())
                self.assertEqual(kv.get("mode"), "impact")
                self.assertEqual(kv.get("rust"), expected_rust)


if __name__ == "__main__":
    unittest.main()
