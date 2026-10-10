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

from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/ffmpeg-patch-stack.yml"
INTEGRATION_WORKFLOW = ROOT / ".github/workflows/ffmpeg-integration.yml"
SMOKE_SCRIPT = ROOT / "ffmpeg-patches/test/build-and-run.sh"
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
        self.assertEqual(self.workflow.count("persist-credentials: false"), 2)
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


if __name__ == "__main__":
    unittest.main()
