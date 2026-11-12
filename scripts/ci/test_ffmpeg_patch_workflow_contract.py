#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Guard local refresh, required replay and scheduled stable-release discovery."""

from __future__ import annotations

import importlib.util
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from typing import ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.ci import ffmpeg_shared_series_fixture as shared_series
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/ffmpeg-patch-stack.yml"
INTEGRATION_WORKFLOW = ROOT / ".github/workflows/ffmpeg-integration.yml"
SMOKE_SCRIPT = ROOT / "ffmpeg-patches/test/build-and-run.sh"
HOOKS = ROOT / ".pre-commit-config.yaml"
LINT_WORKFLOW = ROOT / ".github/workflows/lint-and-format.yml"
AGGREGATOR = ROOT / ".github/workflows/required-aggregator.yml"
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


FILTER_CHECKS = ("parity", "windows", "contract", "provenance")


# The build-log warning gate of the filter job, as the planted case removes it.
WARNING_GATE = (
    "          warnings=\"$(grep -Ei '(^|[[:space:]])warning([[:space:]#:])' "
    '"$RUNNER_TEMP/ffmpeg-build.log" || [[ $? -eq 1 ]])"\n'
    '          if [[ -n "$warnings" ]]; then\n'
    "            printf '%s\\n' \"$warnings\" >&2\n"
    '            echo "FATAL: FFmpeg emitted compiler warnings" >&2\n'
    "            exit 1\n"
    "          fi\n"
)
# Only grep exit 1 (no match) passes; exit 2 must fail the step, not read as clean.
GREP_STATUS_GUARD = "|| [[ $? -eq 1 ]]"


def _filter_job_findings(workflow: str, aggregator: str) -> list[str]:
    """What the required vmafx filter job lacks; empty when it builds and runs every check."""
    try:
        job = _job(workflow, "filter")
    except ValueError:
        return ["no filter job"]
    findings: list[str] = []
    header = job.split("    steps:\n", maxsplit=1)[0]
    for line in (
        "    # required-aggregator\n",
        "    name: FFmpeg vmafx Filter\n",
        "    needs: tier\n",
    ):
        if line not in header:
            findings.append(f"filter job header lacks {line.strip()!r}")
    if "'FFmpeg vmafx Filter'," not in aggregator:
        findings.append("aggregator does not require FFmpeg vmafx Filter")
    build = _run(_step(job, "Build FFmpeg with the vmafx filter"))
    first_line = next((line for line in build.splitlines() if line.strip()), "")
    if first_line != "set -euo pipefail":
        findings.append("filter build does not start with set -euo pipefail")
    for needle in (
        "--enable-libvmafx",
        "CONFIG_VMAFX_FILTER 1",
        "--keep-tree",
        "--fatal-warnings",
        "2>&1 | tee",
        "grep -Ei '(^|[[:space:]])warning([[:space:]#:])'",
        GREP_STATUS_GUARD,
        "FATAL: FFmpeg emitted compiler warnings",
    ):
        if needle not in build:
            findings.append(f"filter build lacks {needle}")
    _, found, rest = _run(_step(job, "Check the vmafx filter against the CLI")).partition(
        "for check in "
    )
    checks = rest.split(";", 1)[0].split() if found else []
    findings += [f"filter check {name} not run" for name in FILTER_CHECKS if name not in checks]
    return findings


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
        # ADR-2169: the tier job owns the draft gate and the light tier owns this check.
        self.assertIn("needs.tier.outputs.light == 'true'", header)
        self.assertNotIn("steps.impact", header)
        self.assertIn("    needs: tier\n", header)

    def test_publication_uses_generated_release_default(self) -> None:
        publication = (ROOT / ".github/workflows/docker-publish-operator-node.yml").read_text()
        self.assertNotRegex(publication, r"(?m)^\s+FFMPEG_TAG=n[0-9]")

    def test_integration_loads_config_before_consumers(self) -> None:
        integration = INTEGRATION_WORKFLOW.read_text()
        sycl = _job(integration, "ffmpeg-sycl-work")
        self.assertLess(sycl.index("uses: actions/checkout@"), sycl.index("load-build-config.sh"))
        self.assertLess(sycl.index("load-build-config.sh"), sycl.index("Install Intel oneAPI"))
        self.assertNotIn(". ./build-config.env", sycl)
        self.assertNotIn("--branch n9.0.1", integration)

    def test_integration_and_smoke_builds_fail_on_diagnostics(self) -> None:
        integration = INTEGRATION_WORKFLOW.read_text(encoding="utf-8")
        ordinary = _job(integration, "ffmpeg-work")
        sycl = _job(integration, "ffmpeg-sycl-work")
        smoke = SMOKE_SCRIPT.read_text(encoding="utf-8")
        warning_pattern = "grep -Ei '(^|[[:space:]])warning([[:space:]#:])'"

        for name, source in (("ordinary", ordinary), ("sycl", sycl), ("smoke", smoke)):
            with self.subTest(name=name):
                self.assertIn("--fatal-warnings", source)
                self.assertIn("2>&1 | tee", source)
                self.assertRegex(source, r"make .*build")
                self.assertIn("make -s fate-list", source)
                self.assertIn("awk '/^fate-/'", source)
                self.assertIn('"${fate_targets[@]}"', source)
                self.assertIn(warning_pattern, source)

        # The diagnostics patch is the shared fix series' (ADR-3143).
        self.assertIn("../scripts/ci/ffmpeg-shared-series.sh apply --method am .", ordinary)
        self.assertNotIn("patch -p1", integration)
        self.assertIn("FATAL: ffmpeg-patches/$line did not apply", sycl)

    def test_warning_gates_fail_closed_on_scan_errors(self) -> None:
        """Issue #3361: only grep exit 1 is clean; a broken scan fails the gate."""
        integration = INTEGRATION_WORKFLOW.read_text(encoding="utf-8")
        integration_gates = re.findall(
            r"(?ms)^          if grep -Ei '[^\n]*/tmp/ffmpeg-build\.log; then\n.*?^          fi$",
            integration,
        )
        self.assertEqual(len(integration_gates), 2, "integration workflow has two warning gates")
        for gate in integration_gates:
            with self.subTest(surface="ffmpeg-integration.yml"):
                self.assertIn("elif [ $? -gt 1 ]; then", gate)
                self.assertIn("FATAL: the warning scan failed to read the build log", gate)

        ffmpeg_dockerfile = (ROOT / "Dockerfile.ffmpeg").read_text(encoding="utf-8")
        self.assertIn("elif [ $? -gt 1 ]; then", ffmpeg_dockerfile)
        self.assertIn("FATAL: the warning scan failed to read the build log", ffmpeg_dockerfile)

        for name, source in (
            ("Dockerfile", (ROOT / "Dockerfile").read_text(encoding="utf-8")),
            ("dev/Containerfile", (ROOT / "dev/Containerfile").read_text(encoding="utf-8")),
            (
                "docker/Dockerfile.node",
                (ROOT / "docker/Dockerfile.node").read_text(encoding="utf-8"),
            ),
        ):
            with self.subTest(surface=name):
                # The scan writes its status into the chain: exit 2 aborts the
                # build instead of feeding an empty match list to the filter.
                self.assertIn(
                    "> /tmp/ffmpeg-warnings.txt || [ $? -eq 1 ]",
                    source,
                    "a broken scan would read as clean",
                )
                # The filter is fail-closed in its own right: grep -Ff exit 2
                # (a missing patched-file list) must abort the build, not read
                # as "no patched file warned" through the old combined
                # `[ -s … ] && grep -Ff …` condition.
                self.assertNotIn(
                    "] && grep -Ff /tmp/ffmpeg-patched-files.txt",
                    source,
                    "the filter is not its own guarded if",
                )
                self.assertIn(
                    "if grep -Ff /tmp/ffmpeg-patched-files.txt /tmp/ffmpeg-warnings.txt; then",
                    source,
                )
                self.assertIn("FATAL: the patched-file filter failed to run", source)
                self.assertIn(
                    "rm /tmp/ffmpeg-build.log /tmp/ffmpeg-warnings.txt",
                    source,
                )

    def test_integration_and_ffmpeg_dockerfile_warning_gates_on_sample_logs(self) -> None:
        """Run the unfiltered gates on sample logs: warnings and a broken scan fail."""
        integration = INTEGRATION_WORKFLOW.read_text(encoding="utf-8")
        gates = re.findall(
            r"(?ms)^          (if grep -Ei '[^\n]*/tmp/ffmpeg-build\.log; then\n.*?^          fi)$",
            integration,
        )
        self.assertEqual(len(gates), 2)
        dockerfile_gate = re.search(
            r"(?ms)^    (if grep -Ei '[^\n]*/tmp/ffmpeg-build\.log; then \\\n.*?^    fi) && \\$",
            (ROOT / "Dockerfile.ffmpeg").read_text(encoding="utf-8"),
        )
        self.assertIsNotNone(dockerfile_gate, "Dockerfile.ffmpeg has no warning gate block")
        assert dockerfile_gate is not None
        cases = {
            "gcc warning": (
                "libavfilter/vf_libvmaf.c:211:13: warning: unused variable 'n' "
                "[-Wunused-variable]\n",
                1,
            ),
            "clean build": ("CC\tlibavfilter/vf_libvmaf.o\nLD\tffmpeg_g\n", 0),
            "missing log": (None, 1),
        }
        for index, block in enumerate([*gates, dockerfile_gate.group(1)]):
            for case, (log, expected) in cases.items():
                with (
                    self.subTest(gate=index, case=case),
                    tempfile.TemporaryDirectory() as temporary,
                ):
                    if log is not None:
                        Path(temporary, "ffmpeg-build.log").write_text(log, encoding="utf-8")
                    script = block.replace("\\\n", "\n")
                    # The gates hard-code the container scratch prefix; read it
                    # from the block instead of a hard-coded host constant
                    # (Bandit B108) and redirect it into the temporary directory.
                    log_dir = re.search(r"(\S+)/ffmpeg-build\.log", block)
                    if log_dir is None:
                        self.fail("gate reads no ffmpeg-build.log")
                    script = script.replace(log_dir.group(1), temporary.replace("\\", "/") + "/")
                    result = run_command(
                        [BASH, "-c", "set -e\n" + script + "\n"],
                        allowed_executables=(BASH,),
                        cwd=temporary,
                        env={**os.environ, "LC_ALL": "C"},
                        check=False,
                        capture_output=True,
                        text=True,
                        timeout_seconds=60,
                    )
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                    fatal = "FATAL: FFmpeg emitted compiler warnings" in result.stderr
                    self.assertEqual(fatal, expected == 1 and log is not None, result.stderr)

    def test_filtered_warning_gate_on_sample_logs(self) -> None:
        """Run the root Dockerfile's filtered gate on sample logs.

        A matched patched file, a broken scan and a broken filter fail the
        build; a warning on an unpatched file and a clean build pass it.
        """
        dockerfile = (ROOT / "Dockerfile").read_text(encoding="utf-8")
        block = re.search(
            r"(?ms)^    \{ grep -Ei '[^\n]*/tmp/ffmpeg-build\.log > /tmp/ffmpeg-warnings\.txt"
            r" \|\| \[ \$\? -eq 1 \]; \} && \\\n"
            r"    if \[ -s /tmp/ffmpeg-warnings\.txt \]; then \\\n"
            r".*?^    fi && \\$",
            dockerfile,
        )
        self.assertIsNotNone(block, "root Dockerfile has no filtered warning gate")
        assert block is not None
        body = block.group(0).removesuffix(" && \\").replace("\\\n", "\n")
        log_dir = re.search(r"(\S+)/ffmpeg-build\.log", body)
        if log_dir is None:
            self.fail("filtered gate reads no ffmpeg-build.log")
        prefix = log_dir.group(1)
        cases = {
            "matched patched file": (
                "libavfilter/vf_libvmaf.c:211:13: warning: unused variable 'n' "
                "[-Wunused-variable]\n",
                True,
                1,
                "FATAL: a file this fork patches emitted compiler warnings",
            ),
            "unpatched file warns": (
                "libavfilter/vf_unpatched.c:88:5: warning: implicit conversion loses "
                "integer precision\n",
                True,
                0,
                None,
            ),
            "clean build": ("CC\tlibavfilter/vf_libvmaf.o\nLD\tffmpeg_g\n", True, 0, None),
            "missing build log": (None, True, 1, None),
            "missing patched-file list": (
                "libavfilter/vf_libvmaf.c:211:13: warning: unused variable 'n' "
                "[-Wunused-variable]\n",
                False,
                1,
                "FATAL: the patched-file filter failed to run",
            ),
        }
        for case, (log, with_list, expected, fatal) in cases.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                if log is not None:
                    (directory / "ffmpeg-build.log").write_text(log, encoding="utf-8")
                if with_list:
                    (directory / "ffmpeg-patched-files.txt").write_text(
                        "libavfilter/vf_libvmaf.c\n", encoding="utf-8"
                    )
                # The gate hard-codes the container scratch prefix; read it from
                # the block instead of a hard-coded host constant (Bandit B108)
                # and redirect it into the temporary directory.
                script = body.replace(prefix, temporary.replace("\\", "/") + "/")
                result = run_command(
                    [BASH, "-c", "set -e\n" + script],
                    allowed_executables=(BASH,),
                    cwd=temporary,
                    env={**os.environ, "LC_ALL": "C"},
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout_seconds=60,
                )
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                self.assertEqual(
                    fatal is not None and fatal in result.stderr,
                    fatal is not None,
                    result.stderr,
                )

    def test_msvc_leg_builds_the_series_against_the_static_msvc_install(self) -> None:
        # ADR-2783: the configured release with the whole series, cl.exe through
        # FFmpeg's configure, linked against vmaf.lib / vmafx.lib (ADR-2752).
        integration = INTEGRATION_WORKFLOW.read_text(encoding="utf-8")
        work = _job(integration, "ffmpeg-msvc-work")
        gate = _job(integration, "ffmpeg-msvc-gate")
        self.assertIn("    name: FFmpeg Windows MSVC work\n", work)
        self.assertIn("    # required-aggregator\n    name: FFmpeg Windows MSVC\n", gate)
        self.assertIn("needs.ffmpeg-msvc-work.result", gate)
        self.assertLess(work.index("core.autocrlf false"), work.index("uses: actions/checkout@"))
        self.assertLess(work.index("core.eol lf"), work.index("uses: actions/checkout@"))
        build = _step(work, "Build libvmaf (static, MSVC)")
        self.assertIn("--default-library=static", build)
        self.assertNotIn("b_vscrt", build)
        self.assertIn(
            "check_msvc_library_names.py --prefix install",
            _step(work, "Check installed library names (MSVC)"),
        )
        smoke = _step(work, "Build FFmpeg with the patch series against vmaf.lib")
        self.assertIn("FFMPEG_TOOLCHAIN: msvc", smoke)
        self.assertIn("VMAF_SCORE_CHECK: '1'", smoke)
        self.assertIn("bash ffmpeg-patches/test/build-and-run.sh", smoke)
        self.assertNotIn("SMOKE_FATE", work)
        self.assertNotIn("continue-on-error", work)
        self.assertLess(work.index("check_msvc_library_names.py"), work.index("build-and-run.sh"))

    def test_all_release_consumers_use_warning_clean_tag_checkout(self) -> None:
        helper = "scripts/ci/checkout-annotated-tag.sh"
        integration = INTEGRATION_WORKFLOW.read_text(encoding="utf-8")
        smoke = SMOKE_SCRIPT.read_text(encoding="utf-8")
        node = (ROOT / "docker/Dockerfile.node").read_text(encoding="utf-8")
        dev = (ROOT / "dev/Containerfile").read_text(encoding="utf-8")

        self.assertEqual(integration.count(helper), 2)
        self.assertIn(helper, smoke)
        self.assertIn("checkout-annotated-tag", node)
        self.assertEqual(dev.count(helper), 2)
        for source in (integration, smoke, node, dev):
            self.assertNotRegex(source, r"git clone[^\n]+(?:FFMPEG|AMF)")

    def test_contracts_run_once_in_the_tooling_suite(self) -> None:
        # A test runs once in CI (ADR-1568): Tooling Tests runs the patch
        # automation tests on every pull request and push, before any of this
        # workflow's runs can use the scripts they cover.
        sys.path.insert(0, str(ROOT))
        from scripts.ci.suite_registry import suite_members  # noqa: PLC0415

        tooling = suite_members(ROOT, "tooling")
        for name in (
            "test_ffmpeg_patch_stack",
            "test_ffmpeg_patch_smoke_safety",
            "test_ffmpeg_patch_workflow_contract",
        ):
            self.assertIn(f"scripts/ci/{name}.py", tooling)
        for job in (self.check, self.refresh):
            self.assertNotIn("test_ffmpeg_patch", job)

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
        self.assertEqual(self.workflow.count("persist-credentials: false"), 3)
        self.assertIn(
            "${{ github.event_name }}-"
            "${{ github.ref == 'refs/heads/master' && github.sha || github.ref }}",
            self.workflow,
        )

    def test_refresh_retains_proposal_even_on_failure(self) -> None:
        record = _step(self.refresh, "Record proposed refresh")
        upload = _step(self.refresh, "Retain refresh proposal")
        self.assertIn("if: always()", record)
        self.assertIn("git diff --binary >", record)
        self.assertIn("proposed-refresh.patch", record)
        self.assertIn("source-revision.txt", record)
        self.assertIn("worktree-status.txt", record)
        self.assertIn("if: ${{ (always()) && env.GITEA_ACTIONS != 'true' }}", upload)
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
        step = _step(lint, "Run pre-commit on the diff (pull request) or the whole tree")
        self.assertIn("SKIP: ffmpeg-patches-apply-check", step)
        self.assertIn("scripts/ci/precommit_scope.py", step)
        scope = (ROOT / "scripts/ci/precommit_scope.py").read_text(encoding="utf-8")
        self.assertIn('"pre-commit", "run"', scope)
        self.assertIn('"--all-files"', scope)
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
            "scripts/ci/ffmpeg-shared-series.sh": True,
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
                result = run_command(
                    [BASH, "-c", _run(_step(job, step_name))],
                    allowed_executables=(BASH,),
                    cwd=temporary,
                    env=env,
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout_seconds=120,
                )
                self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
                self.assertIn(
                    "injected replay conflict",
                    (directory / output / "workflow-command.log").read_text(encoding="utf-8"),
                )

    def test_vmafx_filter_job_builds_and_checks_the_filter(self) -> None:
        # Issue #2475: the vmafx filter is built against libvmafx and checked against the CLI.
        aggregator = AGGREGATOR.read_text(encoding="utf-8")
        self.assertEqual(_filter_job_findings(self.workflow, aggregator), [])

    def test_vmafx_filter_job_check_refuses_planted_gaps(self) -> None:
        """Each planted gap in the filter job or the aggregator yields exactly its finding."""
        aggregator = AGGREGATOR.read_text(encoding="utf-8")
        no_strict = self.workflow.replace(
            '          set -euo pipefail\n          tree="$RUNNER_TEMP/ffmpeg"\n',
            '          tree="$RUNNER_TEMP/ffmpeg"\n',
            1,
        )
        self.assertNotEqual(no_strict, self.workflow, "no strict mode replace was a no-op")
        planted = {
            "no strict mode": (
                no_strict,
                aggregator,
                ["filter build does not start with set -euo pipefail"],
            ),
            "dropped check": (
                self.workflow.replace("parity windows contract provenance", "parity windows"),
                aggregator,
                ["filter check contract not run", "filter check provenance not run"],
            ),
            "not required": (
                self.workflow,
                aggregator.replace("'FFmpeg vmafx Filter',", ""),
                ["aggregator does not require FFmpeg vmafx Filter"],
            ),
            "no libvmafx": (
                self.workflow.replace("--enable-libvmafx", ""),
                aggregator,
                ["filter build lacks --enable-libvmafx"],
            ),
            "no fatal warnings": (
                self.workflow.replace(
                    "./configure --fatal-warnings --enable-version3",
                    "./configure --enable-version3",
                ),
                aggregator,
                ["filter build lacks --fatal-warnings"],
            ),
            "no warning grep": (
                self.workflow.replace(WARNING_GATE, ""),
                aggregator,
                [
                    "filter build lacks grep -Ei '(^|[[:space:]])warning([[:space:]#:])'",
                    f"filter build lacks {GREP_STATUS_GUARD}",
                    "filter build lacks FATAL: FFmpeg emitted compiler warnings",
                ],
            ),
            "grep error read as clean": (
                self.workflow.replace(GREP_STATUS_GUARD, "|| true"),
                aggregator,
                [f"filter build lacks {GREP_STATUS_GUARD}"],
            ),
        }
        for case, (workflow, required, expected) in planted.items():
            with self.subTest(case=case):
                self.assertEqual(_filter_job_findings(workflow, required), expected)

    def test_vmafx_filter_warning_gate_on_sample_build_logs(self) -> None:
        """Run the job's own warning gate on sample logs: warnings and a missing log fail it."""
        build = _run(_step(_job(self.workflow, "filter"), "Build FFmpeg with the vmafx filter"))
        block = re.search(r'(?ms)^warnings="\$\(grep .*?^fi$', build)
        self.assertIsNotNone(block, "filter build has no warning gate block")
        assert block is not None
        line = next((entry for entry in build.splitlines() if entry.strip()), "")
        self.assertEqual(line, "set -euo pipefail", "filter build has no strict mode")
        gate = line + "\n" + block.group(0) + "\n"
        cases = {
            "gcc warning": (
                "libavfilter/vf_vmafx.c:211:13: warning: unused variable 'n' "
                "[-Wunused-variable]\n",
                1,
            ),
            "clang warning": (
                "libavfilter/vf_vmafx.c:88:5: warning: implicit conversion loses "
                "integer precision\n1 warning generated.\n",
                1,
            ),
            "linker warning": ("/usr/bin/ld: warning: libvmafx.so: missing .note.GNU-stack\n", 1),
            "upper-case line start": ("WARNING: unknown option ignored\n", 1),
            "numbered diagnostic": ("vf_vmafx.c(12): warning #177: variable unused\n", 1),
            "clean build": ("CC\tlibavfilter/vf_vmafx.o\nLD\tffmpeg_g\nSTRIP\tffmpeg\n", 0),
            "empty log": ("", 0),
            "option name only": ("configure --fatal-warnings --enable-libvmafx\n", 0),
            "word inside a path": ("CC\tlibavutil/warning_helper.o\n", 0),
            "missing log": (None, 1),
        }
        for case, (log, expected) in cases.items():
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                if log is not None:
                    Path(temporary, "ffmpeg-build.log").write_text(log, encoding="utf-8")
                result = run_command(
                    [BASH, "-c", gate],
                    allowed_executables=(BASH,),
                    cwd=temporary,
                    env={**os.environ, "RUNNER_TEMP": temporary, "LC_ALL": "C"},
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout_seconds=60,
                )
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                fatal = "FATAL: FFmpeg emitted compiler warnings" in result.stderr
                self.assertEqual(fatal, expected == 1 and log is not None, result.stderr)


def _setup_cosign_test_fixture(p: Path, record: Path) -> dict[str, str]:
    bin_dir = p / "bin"
    bin_dir.mkdir()
    tarball, sha = shared_series.tarball(
        p / "release",
        "n9.0.2",
        "946fcce07b6dcd0331c8cc609192aeff5e1924f8",
        {"0001-fix.patch": "diff\n"},
    )
    curl = bin_dir / "curl"
    curl.write_text(
        "#!/bin/sh\n"
        'dest=""\nurl=""\n'
        "while [ $# -gt 0 ]; do\n"
        '  case "$1" in\n'
        '    -o|--output) dest="$2"; shift 2 ;;\n'
        '    http*|file*|ftp*) url="$1"; shift ;;\n'
        "    *) shift ;;\n"
        "  esac\ndone\n"
        f'case "$url" in *tar.gz) cp "{tarball}" "$dest" ;; '
        f'*SHA256SUMS) cp "{tarball.parent / "SHA256SUMS"}" "$dest" ;; '
        '*sigstore.json) echo "{}" > "$dest" ;; esac\n'
    )
    curl.chmod(0o755)
    cosign = bin_dir / "cosign"
    cosign.write_text(
        f'#!/bin/sh\n[ "$1" = "verify-blob" ] && printf "%s\\n" "${{TUF_ROOT:-UNSET}}" > "{record}"\nexit 0\n'
    )
    cosign.chmod(0o755)
    config = p / "build-config.env"
    config.write_text(
        'FFMPEG_REMOTE="https://github.com/FFmpeg/FFmpeg.git"\n'
        'FFMPEG_TAG="n9.0.2"\n'
        'FFMPEG_COMMIT="946fcce07b6dcd0331c8cc609192aeff5e1924f8"\n'
        'FFMPEG_FIX_SERIES_REPO="https://github.com/VMAFx/ffmpeg-patches"\n'
        f'FFMPEG_FIX_SERIES_TAG="v0.0.0-fixture"\nFFMPEG_FIX_SERIES_SHA256="{sha}"\n'
    )
    env = {
        k: v
        for k, v in os.environ.items()
        if not k.startswith("FFMPEG_FIX_SERIES_") and k != "TUF_ROOT"
    }
    env.update(
        PATH=f"{bin_dir}:{env.get('PATH', '')}",
        BUILD_CONFIG=str(config),
        FFMPEG_FIX_SERIES_CACHE="none",
        FFMPEG_FIX_SERIES_VERIFY="cosign",
    )
    return env


class SharedFixSeriesContract(unittest.TestCase):
    """Every FFmpeg build applies the pinned shared fix series before ours (ADR-3143)."""

    SCRIPT = "ffmpeg-shared-series.sh"
    # Where each build applies ffmpeg-patches/: the text that marks the place.
    SITES: ClassVar[dict[str, str]] = {
        "Dockerfile": "done < /tmp/ffmpeg-patches/series.txt",
        "Dockerfile.ffmpeg": "done < /tmp/ffmpeg-patches/series.txt",
        "dev/Containerfile": "done < /build/vmaf/ffmpeg-patches/series.txt",
        "docker/Dockerfile.node": "done < /src/ffmpeg-patches/series.txt",
        "ffmpeg-patches/test/build-and-run.sh": 'done <"$PATCHES_DIR/series.txt"',
        "scripts/ci/ffmpeg_patch_stack.py": 'replay.git("am", "--3way", str(repo / "ffmpeg-patches" / name))',
    }

    def test_pins_are_complete_and_well_formed(self) -> None:
        config = (ROOT / "build-config.env").read_text()
        self.assertRegex(
            config, r'(?m)^FFMPEG_FIX_SERIES_REPO="https://github\.com/[\w.-]+/[\w.-]+"$'
        )
        self.assertRegex(config, r'(?m)^FFMPEG_FIX_SERIES_TAG="v[0-9][\w.-]*"$')
        self.assertRegex(config, r'(?m)^FFMPEG_FIX_SERIES_SHA256="[0-9a-f]{64}"$')
        # Only the tests point the script at another tarball.
        self.assertNotIn("FFMPEG_FIX_SERIES_URL=", config)

    def test_every_build_applies_the_series_before_ours(self) -> None:
        for name, ours in self.SITES.items():
            with self.subTest(site=name):
                text = (ROOT / name).read_text()
                self.assertIn(ours, text)
                self.assertIn(self.SCRIPT, text)
                self.assertLess(text.index(self.SCRIPT), text.index(ours))
        integration = INTEGRATION_WORKFLOW.read_text()
        sycl = _job(integration, "ffmpeg-sycl-work")
        self.assertLess(sycl.index(self.SCRIPT), sycl.index("done < ../ffmpeg-patches/series.txt"))
        # The stock-FFmpeg matrix takes the fix series and none of ours.
        stock = _job(integration, "ffmpeg-work")
        self.assertIn(f"{self.SCRIPT} apply --method am .", stock)
        self.assertNotIn("ffmpeg-patches/0", stock)

    def test_container_builds_give_the_script_its_configuration(self) -> None:
        """The script reads build-config.env beside itself, two levels up, or BUILD_CONFIG."""
        root = (ROOT / "Dockerfile").read_text()
        self.assertIn("COPY . /vmaf\n", root)
        self.assertIn("bash /vmaf/scripts/ci/ffmpeg-shared-series.sh apply", root)
        # These two copy the file and the script into one directory and run it there.
        for name in ("Dockerfile.ffmpeg", "docker/Dockerfile.node"):
            with self.subTest(site=name):
                text = (ROOT / name).read_text()
                copied = re.search(
                    rf"(?m)^COPY build-config\.env scripts/ci/{re.escape(self.SCRIPT)} (\S+)/$",
                    text,
                )
                assert copied is not None, f"{name} does not copy the pin file beside the script"
                self.assertIn(f"bash {copied.group(1)}/{self.SCRIPT} apply", text)
        # The dev image: the repository tree of the stage has no build-config.env,
        # an ancestor stage copied it to /opt/vmafx, and the step names that copy.
        dev = (ROOT / "dev/Containerfile").read_text()
        stage = dev.split("FROM codec-deps AS ffmpeg-compile\n", 1)[1].split("\nFROM ", 1)[0]
        self.assertIn("BUILD_CONFIG=/opt/vmafx/build-config.env", stage)
        self.assertLess(
            stage.index(f"{self.SCRIPT} apply --method am /build/ffmpeg"),
            stage.index("done < /build/vmaf/ffmpeg-patches/series.txt"),
        )
        before = dev.split("FROM codec-deps AS ffmpeg-compile\n", 1)[0]
        self.assertIn("COPY build-config.env /opt/vmafx/build-config.env\n", before)
        self.assertNotRegex(before, r"COPY [^\n]*build-config\.env[^\n]* /build/vmaf/?\n")

    def test_no_other_file_applies_ours_without_the_series(self) -> None:
        git = shutil.which("git")
        if git is None:
            self.skipTest("git is not installed")
        listed = run_command(
            [
                git,
                "-C",
                str(ROOT),
                "ls-files",
                "-z",
                "--",
                "*.sh",
                "*.py",
                "*.yml",
                "*Dockerfile*",
                "*Containerfile*",
            ],
            allowed_executables=(git,),
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=60,
        )
        assert isinstance(listed.stdout, str)
        known = set(self.SITES) | {".github/workflows/ffmpeg-integration.yml"}
        for name in filter(None, listed.stdout.split("\0")):
            if name in known or "/test" in name or name.startswith(("docs/", "scripts/ci/test")):
                continue
            text = (ROOT / name).read_text(errors="replace")
            if re.search(r"git (?:am|apply)[^\n]*ffmpeg-patches/", text):
                self.assertIn(
                    self.SCRIPT, text, f"{name} applies ffmpeg-patches/ without the shared series"
                )

    def test_the_diagnostics_patch_is_the_series_not_ours(self) -> None:
        series = (ROOT / "ffmpeg-patches/series.txt").read_text()
        self.assertNotIn("diagnostics", series)
        self.assertEqual(list((ROOT / "ffmpeg-patches").glob("*diagnostics*.patch")), [])

    def test_the_required_gate_checks_the_release_signature(self) -> None:
        workflow = WORKFLOW.read_text(encoding="utf-8")
        for job in ("check", "refresh"):
            with self.subTest(job=job):
                text = _job(workflow, job)
                self.assertIn("sigstore/cosign-installer@", text)
                self.assertIn("FFMPEG_FIX_SERIES_VERIFY: cosign", text)
                self.assertLess(
                    text.index("sigstore/cosign-installer@"), text.index("ffmpeg_patch_stack.py")
                )

    def test_script_refuses_missing_pins_and_unknown_modes(self) -> None:
        script = ROOT / "scripts/ci" / self.SCRIPT
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "build-config.env"
            config.write_text('FFMPEG_TAG="n9.0.2"\nFFMPEG_COMMIT="' + "0" * 40 + '"\n')
            environment = {
                key: value
                for key, value in os.environ.items()
                if not key.startswith("FFMPEG_FIX_SERIES_")
            }
            environment["BUILD_CONFIG"] = str(config)
            for argv, needle in (
                (["apply", directory], "FFMPEG_FIX_SERIES_REPO is not set"),
                (["frobnicate"], "unknown command"),
                (["apply", "--method", "rebase", directory], "--method must be am or apply"),
            ):
                with self.subTest(argv=argv):
                    result = run_command(
                        [BASH, str(script), *argv],
                        allowed_executables=(BASH,),
                        env=environment,
                        capture_output=True,
                        text=True,
                        timeout_seconds=60,
                    )
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn(needle, str(result.stderr))

    def test_the_script_isolates_cosign_tuf_root(self) -> None:
        """The script isolates cosign's TUF cache so read-only home sandboxes succeed."""
        text = (ROOT / "scripts/ci" / self.SCRIPT).read_text(encoding="utf-8")
        needle = 'export TUF_ROOT="${TUF_ROOT:-$work/sigstore}"'
        self.assertIn(needle, text)
        self.assertLess(text.index(needle), text.index("cosign verify-blob"))

    def test_cosign_invocation_inherits_or_defaults_tuf_root(self) -> None:
        """Verify cosign executes with TUF_ROOT defaulted to work/sigstore or inherited."""
        script = ROOT / "scripts/ci" / self.SCRIPT
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            record = p / "tuf_root.txt"
            env = _setup_cosign_test_fixture(p, record)
            res = run_command(
                [BASH, str(script), "fetch", str(p / "out1")],
                allowed_executables=(BASH,),
                env=env,
                capture_output=True,
                text=True,
                timeout_seconds=60,
            )
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertTrue(record.read_text().strip().endswith("/sigstore"))

            res2 = run_command(
                [BASH, str(script), "fetch", str(p / "out2")],
                allowed_executables=(BASH,),
                env=dict(env, TUF_ROOT=str(p / "custom_tuf")),
                capture_output=True,
                text=True,
                timeout_seconds=60,
            )
            self.assertEqual(res2.returncode, 0, res2.stderr)
            self.assertEqual(record.read_text().strip(), str(p / "custom_tuf"))


MSYS2_STEP = re.compile(r"(?m)^([ \t]*)- uses: msys2/setup-msys2@")


def _msys2_inherit_cache_findings(name: str, workflow: str) -> list[str]:
    """A setup-msys2 step with path-type: inherit that keeps the package cache on.

    path-type: inherit hands the shell the whole runner environment; behind an MSVC
    environment step that leaves less than LINE_MAX of MSYS2's ARG_MAX, and the
    action's cache pruning (paccache, which runs xargs) aborts on the findutils
    assertion bc_ctl.arg_max >= LINE_MAX. cache: false skips the pruning.
    """
    found = []
    for match in MSYS2_STEP.finditer(workflow):
        indent = match.group(1)
        end = workflow.find(f"\n{indent}- ", match.end())
        step = workflow[match.start() : end if end >= 0 else len(workflow)]
        inherit = re.search(r"(?m)^\s+path-type:\s*inherit\s*$", step)
        cache_off = re.search(r"(?m)^\s+cache:\s*false\s*$", step)
        if inherit and not cache_off:
            line = workflow.count("\n", 0, match.start()) + 1
            found.append(f"{name}:{line}: setup-msys2 with path-type: inherit needs cache: false")
    return found


class Msys2InheritContract(unittest.TestCase):
    def test_inherited_environment_steps_skip_the_cache_pruning(self) -> None:
        findings = [
            item
            for path in sorted((ROOT / ".github/workflows").glob("*.yml"))
            for item in _msys2_inherit_cache_findings(path.name, path.read_text(encoding="utf-8"))
        ]
        self.assertEqual(findings, [])

    def test_check_refuses_a_planted_inherit_step_with_the_cache_on(self) -> None:
        step = "      - uses: msys2/setup-msys2@abc  # v2\n        with:\n          path-type: inherit\n"
        planted = "jobs:\n  a:\n    steps:\n" + step + "      - name: next\n        run: true\n"
        self.assertEqual(
            _msys2_inherit_cache_findings("w.yml", planted),
            ["w.yml:4: setup-msys2 with path-type: inherit needs cache: false"],
        )
        fixed = planted.replace(
            "path-type: inherit\n", "path-type: inherit\n          cache: false\n"
        )
        self.assertEqual(_msys2_inherit_cache_findings("w.yml", fixed), [])
        minimal = planted.replace("path-type: inherit", "path-type: minimal")
        self.assertEqual(_msys2_inherit_cache_findings("w.yml", minimal), [])

    def test_a_blank_line_before_the_step_does_not_widen_it(self) -> None:
        # A blank line in the captured indent made the step run to the end of the file,
        # so a later step's cache: false hid the missing one.
        step = "      - uses: msys2/setup-msys2@abc  # v2\n        with:\n          path-type: inherit\n"
        later = "      - uses: other/action@abc  # v1\n        with:\n          cache: false\n"
        planted = "jobs:\n  a:\n    steps:\n\n" + step + later
        self.assertEqual(
            _msys2_inherit_cache_findings("w.yml", planted),
            ["w.yml:5: setup-msys2 with path-type: inherit needs cache: false"],
        )


if __name__ == "__main__":
    unittest.main()
