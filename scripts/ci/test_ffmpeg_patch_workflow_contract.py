#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Guard local refresh, required replay and scheduled stable-release discovery."""

from __future__ import annotations

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/ffmpeg-patch-stack.yml"
HOOKS = ROOT / ".pre-commit-config.yaml"
LINT_WORKFLOW = ROOT / ".github/workflows/lint-and-format.yml"
BASH = shutil.which("bash") or "/bin/bash"


def _job(workflow: str, identifier: str) -> str:
    """Extract a named job without depending on a third-party YAML parser."""
    marker = f"  {identifier}:\n"
    start = workflow.index(marker, workflow.index("\njobs:\n"))
    following = re.search(r"(?m)^  [a-z][a-z0-9_-]*:\s*$", workflow[start + len(marker) :])
    end = start + len(marker) + following.start() if following else len(workflow)
    return workflow[start:end]


def _step(job: str, name: str) -> str:
    """Extract one workflow step, including its condition and shell body."""
    marker = f"      - name: {name}\n"
    start = job.index(marker)
    end = job.find("\n      - ", start + len(marker))
    return job[start:end] if end >= 0 else job[start:]


def _run(step: str) -> str:
    """Read the literal shell block used by a step."""
    body = step.split("        run: |\n", maxsplit=1)[1]
    return "\n".join(line[10:] for line in body.splitlines() if line.startswith("          "))


def _load_planner() -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        "ffmpeg_workflow_impact_planner", ROOT / "scripts/ci/plan-ci-impact.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class FFmpegWorkflowContract(unittest.TestCase):
    """Keep expensive replay scoped while preserving an independent CI gate."""

    workflow: str
    check: str
    refresh: str
    hook: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")
        cls.check = _job(cls.workflow, "check")
        cls.refresh = _job(cls.workflow, "refresh")
        hooks = HOOKS.read_text(encoding="utf-8")
        cls.hook = hooks.split("      - id: ffmpeg-patches-apply-check\n", maxsplit=1)[1].split(
            "\n      - id:", maxsplit=1
        )[0]

    def test_required_job_registers_without_event_path_filters(self) -> None:
        events = self.workflow.split("\non:\n", maxsplit=1)[1].split(
            "\npermissions:\n", maxsplit=1
        )[0]
        self.assertIn("  push:\n    branches: [master]", events)
        self.assertIn("  pull_request:\n    branches: [master]", events)
        self.assertIn("ready_for_review", events)
        self.assertNotRegex(events, r"(?m)^\s+paths(?:-ignore)?:")
        self.assertNotIn("pull_request_target", events)
        header = self.check.split("    steps:\n", maxsplit=1)[0]
        self.assertIn("    name: FFmpeg Patch Stack\n", header)
        self.assertIn("    # required-aggregator\n", header)
        self.assertIn("github.event.pull_request.draft == false", header)
        self.assertNotIn("steps.impact", header)
        self.assertNotIn("needs:", header)

    def test_publication_uses_generated_release_default(self) -> None:
        publication = (ROOT / ".github/workflows/docker-publish-operator-node.yml").read_text()
        self.assertNotRegex(publication, r"(?m)^\s+FFMPEG_TAG=n[0-9]")

    def test_integration_loads_config_before_consumers(self) -> None:
        integration = (ROOT / ".github/workflows/ffmpeg-integration.yml").read_text()
        sycl = _job(integration, "ffmpeg-sycl")
        self.assertLess(sycl.index("uses: actions/checkout@"), sycl.index("load-build-config.sh"))
        self.assertLess(sycl.index("load-build-config.sh"), sycl.index("Install Intel oneAPI"))
        self.assertNotIn(". ./build-config.env", sycl)
        self.assertNotIn("--branch n9.0.1", integration)

    def test_contracts_run_before_impact_or_upstream_network(self) -> None:
        command = "python3 -m unittest discover -s scripts/ci -p 'test_ffmpeg_patch*.py' -v"
        for job in (self.check, self.refresh):
            with self.subTest(job=job.splitlines()[0]):
                contracts = _step(job, "Test patch automation contracts")
                self.assertIn(command, contracts)
                self.assertNotIn("        if:", contracts)
                self.assertLess(job.index(command), job.index("ffmpeg_patch_stack.py"))
        self.assertLess(self.check.index(command), self.check.index("plan-ci-impact.py"))

    def test_pr_and_dispatch_check_only_the_configured_release(self) -> None:
        replay = _step(self.check, "Check configured FFmpeg release")
        self.assertIn("ffmpeg_patch_stack.py --check --output-dir", replay)
        self.assertNotIn("--refresh", self.check)
        self.assertNotIn("--latest", self.check)
        self.assertNotRegex(self.check, r"git\s+(?:ls-remote|fetch|clone)")
        self.assertIn('--event "$EVENT_NAME"', self.check)
        self.assertIn("fetch-depth: 0", self.check)

    def test_scheduled_discovery_has_no_repository_write_authority(self) -> None:
        self.assertIn('cron: "43 5 * * *"', self.workflow)
        header = self.refresh.split("    steps:\n", maxsplit=1)[0]
        self.assertIn("if: github.event_name == 'schedule'", header)
        self.assertIn("--refresh --latest --output-dir", self.refresh)
        self.assertIn("permissions:\n  contents: read", self.workflow)
        self.assertNotRegex(self.workflow, r"(?m)^\s+(?:contents|pull-requests): write")
        self.assertNotIn("continue-on-error", self.workflow)
        self.assertNotRegex(self.workflow, r"git\s+(?:push|tag)|gh\s+pr\s+create")
        self.assertEqual(self.workflow.count("persist-credentials: false"), 2)
        self.assertIn("${{ github.event_name }}-${{ github.ref }}", self.workflow)

    def test_refresh_retains_proposal_even_on_failure(self) -> None:
        record = _step(self.refresh, "Record proposed refresh")
        upload = _step(self.refresh, "Retain refresh proposal")
        self.assertIn("if: always()", record)
        self.assertIn("git diff --binary >", record)
        self.assertIn("proposed-refresh.patch", record)
        self.assertIn("source-revision.txt", record)
        self.assertIn("worktree-status.txt", record)
        self.assertIn("if: always()", upload)
        self.assertIn("if-no-files-found: error", upload)
        self.assertIn("retention-days: 14", upload)

    def test_local_hook_refreshes_without_upstream_discovery(self) -> None:
        self.assertIn("entry: python3 scripts/ci/ffmpeg_patch_stack.py --refresh", self.hook)
        self.assertNotIn("--latest", self.hook)
        self.assertIn("stages: [pre-commit, pre-push]", self.hook)
        self.assertIn("pass_filenames: false", self.hook)
        pattern = re.search(r"(?m)^        files: '([^']+)'$", self.hook)
        assert pattern is not None
        selected = re.compile(pattern.group(1))
        for path in (
            "ffmpeg-patches/series.txt",
            "ffmpeg-patches/0018-example.patch",
            "scripts/ci/ffmpeg_patch_stack.py",
            "scripts/ci/test_ffmpeg_patch_stack.py",
            "build-config.env",
            "core/include/libvmaf/libvmaf.h",
            "core/meson_options.txt",
            "meson_options.txt",
            "core/meson.build",
            "meson.build",
            ".github/workflows/ffmpeg-patch-stack.yml",
        ):
            with self.subTest(path=path):
                self.assertIsNotNone(selected.search(path))
        for path in ("docs/usage/cli.md", "README.md", "pkg/build-config.env", "runtime.env"):
            with self.subTest(path=path):
                self.assertIsNone(selected.search(path))

    def test_broad_ci_precommit_does_not_duplicate_network_replay(self) -> None:
        lint = LINT_WORKFLOW.read_text(encoding="utf-8")
        step = _step(lint, "Run pre-commit on all files")
        self.assertIn("SKIP: ffmpeg-patches-apply-check", step)
        self.assertIn("pre-commit run", step)
        self.assertIn("--all-files", step)
        self.assertNotIn("SKIP:", self.workflow)

    def test_impact_routing_covers_inputs_and_skips_docs(self) -> None:
        planner = _load_planner()
        config = planner.load_config(ROOT / ".github/ci-impact.json")
        replay = _step(self.check, "Check configured FFmpeg release")
        condition_match = re.search(r"(?m)^        if: (.+)$", replay)
        assert condition_match is not None
        condition = condition_match.group(1)
        predicates = []
        for term in condition.split(" || "):
            match = re.fullmatch(r"steps\.impact\.outputs\.([a-z_]+) == '([^']+)'", term)
            assert match is not None, f"unsupported replay condition: {term}"
            predicates.append(match.groups())

        cases = {
            "ffmpeg-patches/series.txt": True,
            "scripts/ci/ffmpeg_patch_stack.py": True,
            "build-config.env": True,
            "core/include/libvmaf/libvmaf.h": True,
            "core/meson_options.txt": True,
            "meson_options.txt": True,
            ".github/workflows/ffmpeg-patch-stack.yml": True,
            "unknown-new-input": True,
            "docs/usage/cli.md": False,
            "changelog.d/fixed/docs.md": False,
            "README.md": False,
            "pkg/score/example.go": False,
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                changes = (planner.Change(status="M", paths=(path,)),)
                plan = planner.build_plan(config, changes, None, "b" * 40, "a" * 40)
                outputs = plan.github_outputs()
                self.assertEqual(
                    any(outputs.get(key) == value for key, value in predicates), expected
                )

    def test_command_failure_survives_diagnostic_tee(self) -> None:
        for job, step_name, output in (
            (self.check, "Check configured FFmpeg release", "ffmpeg-patch-check"),
            (self.refresh, "Refresh to the latest stable release", "ffmpeg-patch-refresh"),
        ):
            with self.subTest(step=step_name), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                python = directory / "python3"
                python.write_text(
                    "#!/bin/sh\necho 'injected replay conflict'\nexit 17\n", encoding="utf-8"
                )
                python.chmod(0o700)
                env = {**os.environ, "RUNNER_TEMP": temporary}
                env["PATH"] = f"{temporary}{os.pathsep}{env.get('PATH', '')}"
                result = subprocess.run(  # noqa: S603 -- fixed workflow block, isolated fake CLI
                    [BASH, "-c", _run(_step(job, step_name))],
                    cwd=temporary,
                    env=env,
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
                self.assertIn(
                    "injected replay conflict",
                    (directory / output / "workflow-command.log").read_text(encoding="utf-8"),
                )


if __name__ == "__main__":
    unittest.main()
