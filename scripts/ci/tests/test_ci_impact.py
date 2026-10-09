#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests for scripts/ci/plan-ci-impact.py and .github/ci-impact.json.

Run with:  python3 -m unittest scripts/ci/tests/test_ci_impact.py
No third-party dependencies (the CI runners have only the stdlib).
"""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from typing import Protocol, cast

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import gate_leg_result as glr
from scripts.lib.safe_subprocess import run as run_command

REPO_ROOT = Path(__file__).resolve().parents[3]
PLANNER = REPO_ROOT / "scripts" / "ci" / "plan-ci-impact.py"
CONFIG = REPO_ROOT / ".github" / "ci-impact.json"
REQUIRED_AGGREGATOR = REPO_ROOT / ".github" / "workflows" / "required-aggregator.yml"
GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"

REQUIRED_CONSUMER_CONTRACTS = {
    "build.yml": (
        "c_core",
        ("build-work",),
        (
            ("linux-intel-llvm-gate", "Linux Intel LLVM", "build-work"),
            ("macos-clang-metal-gate", "macOS Clang+Metal", "build-work"),
            ("windows-msvc-cuda-full-gate", "Windows MSVC+CUDA (full)", "build-work"),
        ),
    ),
    "dev-container-build.yml": (
        "dev_container",
        ("dev-container-build-work",),
        (("dev-container-build", "Dev Container Build", "dev-container-build-work"),),
    ),
    "docker-image.yml": (
        "docker_image",
        ("docker-work",),
        (("docker", "Docker Image Build", "docker-work"),),
    ),
    # ADR-1687: the gate needs the last pull-request job of the chain; `build`
    # needs `validate` and `refs-x86`, so it is skipped when either fails.
    "docker-publish-tester.yml": (
        "tester_image",
        ("validate",),
        (("tester-image", "Tester Image", "build"),),
    ),
    "doxygen-public-api.yml": (
        "doxygen",
        ("doxygen-work",),
        (("doxygen", "Doxygen Public API", "doxygen-work"),),
    ),
    "ffmpeg-integration.yml": (
        "c_core",
        ("ffmpeg-work", "ffmpeg-sycl-work", "ffmpeg-msvc-work"),
        (
            ("ffmpeg-ubuntu-gate", "FFmpeg Ubuntu gcc", "ffmpeg-work"),
            ("ffmpeg-macos-gate", "FFmpeg macOS clang", "ffmpeg-work"),
            ("ffmpeg-sycl-gate", "FFmpeg SYCL", "ffmpeg-sycl-work"),
            ("ffmpeg-msvc-gate", "FFmpeg Windows MSVC", "ffmpeg-msvc-work"),
        ),
    ),
    "helm-chart.yml": (
        "helm",
        ("helm-chart-work",),
        (("helm-chart", "helm lint + template", "helm-chart-work"),),
    ),
    "rust-ci.yml": (
        "rust",
        ("rust-vmafx-sys-work", "cargo-deny-work"),
        (
            ("vmafx-sys-gate", "vmafx-sys CI", "rust-vmafx-sys-work"),
            ("cargo-deny-gate", "cargo-deny", "cargo-deny-work"),
        ),
    ),
    # ADR-1687: `verify` runs after `validate` passed and downloads what the last
    # step of `build` uploads, so a failed build leg fails its verify leg.
    "windows-tester-bundle.yml": (
        "windows_tester_zip",
        ("validate",),
        (("windows-tester-zip", "Windows Tester Zip", "verify"),),
    ),
}


class ImpactPlan(Protocol):
    """Read-only result fields consumed from the dynamically loaded CLI."""

    @property
    def mode(self) -> str: ...

    @property
    def reason(self) -> str: ...

    @property
    def changed_paths(self) -> tuple[str, ...]: ...

    @property
    def selectors(self) -> dict[str, bool]: ...


class GitCommand(Protocol):
    """A Git command bound to a disposable repository and author environment."""

    def __call__(self, *args: str) -> str: ...


def _load_planner() -> ModuleType:
    spec = importlib.util.spec_from_file_location("plan_ci_impact", PLANNER)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    # dataclasses resolve `cls.__module__` through sys.modules when the module
    # uses `from __future__ import annotations`; register before executing.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


planner = _load_planner()


# ADR-1700: the selectors that the full-mode fallback does not set by itself.
TIDY_LANES = ("cuda", "hip", "sycl", "arm64", "clang", "metal")
TIDY_SELECTORS = {f"tidy_{lane}" for lane in TIDY_LANES}
OWN_PATHS_ONLY = {
    "tester_image",
    "windows_tester_zip",
    "windows_tester_zip_sycl",
} | TIDY_SELECTORS


def _plan_for(paths: list[str], statuses: list[str] | None = None) -> ImpactPlan:
    config = planner.load_config(CONFIG)
    statuses = statuses or ["M"] * len(paths)
    changes = tuple(
        planner.Change(status=s, paths=(p,)) for s, p in zip(statuses, paths, strict=True)
    )
    return cast(ImpactPlan, planner.build_plan(config, changes, None, "b" * 40, "h" * 40))


class ConfigContract(unittest.TestCase):
    def test_config_is_canonical_json(self) -> None:
        raw = CONFIG.read_text(encoding="utf-8")
        parsed = json.loads(raw)
        self.assertEqual(raw, json.dumps(parsed, indent=2, ensure_ascii=False) + "\n")

    def test_config_loads_and_every_selector_has_patterns_or_inherits(self) -> None:
        config = planner.load_config(CONFIG)
        for name, selector in config["selectors"].items():
            self.assertTrue(
                selector.get("patterns") or selector.get("inherits"),
                f"selector {name} selects nothing",
            )

    def test_every_top_level_repo_entry_is_known(self) -> None:
        """A path the planner cannot classify forces mode=full (fail-closed).
        Keep the map in step with the tree so routing actually happens."""
        config = planner.load_config(CONFIG)
        tracked = run_command(
            [GIT, "-C", str(REPO_ROOT), "ls-tree", "--name-only", "HEAD"],
            allowed_executables=(GIT,),
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=60,
        ).stdout.split()
        known_prefixes = {p.rstrip("/") for p in config["known_prefixes"]}
        known_files = set(config["known_files"])
        unknown = [e for e in tracked if e not in known_prefixes and e not in known_files]
        self.assertEqual(unknown, [], f"top-level entries missing from ci-impact.json: {unknown}")

    def test_ci_authority_files_are_full_patterns(self) -> None:
        config = planner.load_config(CONFIG)
        for path in (
            ".agents/agents/reviewer.md",
            ".codex/config.toml",
            ".config/archetypes/policy.yaml",
            ".cursor/rules/hiss.mdc",
            ".dir-locals.el",
            ".fleet/settings.json",
            ".gemini/settings.json",
            ".github/ci-impact.json",
            ".github/workflows/build.yml",
            ".github/workflows/dev-container-build.yml",
            ".github/workflows/docker-image.yml",
            ".github/workflows/docker-publish-tester.yml",
            ".github/workflows/doxygen-public-api.yml",
            ".github/workflows/ffmpeg-integration.yml",
            ".github/workflows/helm-chart.yml",
            ".github/workflows/required-aggregator.yml",
            ".github/workflows/rust-ci.yml",
            ".github/workflows/scorecard-policy.yml",
            ".github/workflows/windows-tester-bundle.yml",
            ".pre-commit-config.yaml",
            ".gosec.json",
            ".helix/languages.toml",
            ".nvim.lua",
            ".paperclip/config.yaml",
            ".standards-baseline.json",
            ".standards.lock",
            ".standards.yaml",
            ".windsurfrules",
            "PRE_MIGRATION_EPIC.md",
            "REUSE.toml",
            "Makefile",
            "lefthook.yml",
            "lua/vmafx/init.lua",
            "osv-scanner.toml",
            "scripts/ci/plan-ci-impact.py",
            "standards.sublime-project",
        ):
            self.assertTrue(planner._matches(path, tuple(config["full_patterns"])), path)


class RoutingContract(unittest.TestCase):
    def test_docs_only_change_is_impact_mode_with_no_c_lane(self) -> None:
        plan = _plan_for(["docs/usage/cli.md", "changelog.d/fixed/x.md"])
        self.assertEqual(plan.mode, "impact")
        self.assertTrue(plan.selectors["docs"])
        for lane in ("c_core", "python", "go", "go_checks", "rust", "golden_harness", "tiny_ai"):
            self.assertFalse(plan.selectors[lane], lane)

    def test_c_change_selects_core_and_its_dependents(self) -> None:
        plan = _plan_for(["core/src/feature/adm_tools.c"])
        self.assertEqual(plan.mode, "impact")
        self.assertTrue(plan.selectors["c_core"])
        self.assertTrue(plan.selectors["golden_harness"])
        self.assertTrue(plan.selectors["tiny_ai"])
        self.assertFalse(plan.selectors["go"])
        self.assertTrue(plan.selectors["go_checks"])
        self.assertFalse(plan.selectors["docs"])

    def test_libvmaf_public_header_change_selects_rust_and_c_core(self) -> None:
        plan = _plan_for(["core/include/libvmaf/libvmaf.h"])
        self.assertEqual(plan.mode, "impact")
        self.assertTrue(plan.selectors["c_core"])
        self.assertTrue(plan.selectors["rust"])
        self.assertTrue(plan.selectors["doxygen"])
        self.assertFalse(plan.selectors["go"])
        self.assertFalse(plan.selectors["docs"])

    def test_required_consumer_workflows_have_exact_impact_routes(self) -> None:
        cases = {
            "Dockerfile": {"docker_image"},
            "python/requirements.txt": {"docker_image", "dev_container"},
            "dev/Containerfile": {"dev_container"},
            "core/doc/Doxyfile.public-api": {"doxygen"},
            "deploy/helm/vmafx/Chart.yaml": {"helm"},
            "docker/Dockerfile.tester": {"tester_image"},
            "tools/rc1-tester/image/windows/run.cmd": {"tester_image", "windows_tester_zip"},
            "requirements/locks/windows-tester-zip.txt": {"windows_tester_zip"},
        }
        for path, selected in cases.items():
            with self.subTest(path=path):
                plan = _plan_for([path])
                self.assertEqual(plan.mode, "impact")
                for selector in selected:
                    self.assertTrue(plan.selectors[selector], selector)

    def test_required_consumer_workflow_edits_force_full_plan(self) -> None:
        for path in (
            ".github/workflows/build.yml",
            ".github/workflows/dev-container-build.yml",
            ".github/workflows/docker-image.yml",
            ".github/workflows/doxygen-public-api.yml",
            ".github/workflows/ffmpeg-integration.yml",
            ".github/workflows/helm-chart.yml",
            ".github/workflows/rust-ci.yml",
            ".github/workflows/docker-publish-tester.yml",
            ".github/workflows/windows-tester-bundle.yml",
        ):
            with self.subTest(path=path):
                plan = _plan_for([path])
                self.assertEqual(plan.mode, "full")
                for name, selected in plan.selectors.items():
                    if name not in OWN_PATHS_ONLY:
                        self.assertTrue(selected, name)

    def test_model_json_change_runs_goldens(self) -> None:
        plan = _plan_for(["model/vmaf_v0.6.1.json"])
        self.assertTrue(plan.selectors["c_core"])
        self.assertTrue(plan.selectors["golden_harness"])

    def test_golden_fixture_change_runs_goldens(self) -> None:
        plan = _plan_for(["python/test/resource/yuv/src01_hrc00_576x324.yuv"])
        self.assertTrue(plan.selectors["golden_harness"])

    def test_go_change_selects_only_go(self) -> None:
        plan = _plan_for(["pkg/predictor/predictor.go", "go.mod"])
        self.assertTrue(plan.selectors["go"])
        self.assertTrue(plan.selectors["go_checks"])
        self.assertFalse(plan.selectors["c_core"])
        self.assertFalse(plan.selectors["python"])

    def test_mcp_tool_contract_change_runs_go_checks(self) -> None:
        # The Go MCP server's parity tests read the Python server's tool
        # contract; a Python-only PR that changes it must run them.
        plan = _plan_for(["mcp-server/vmaf-mcp/tool-contract.json"])
        self.assertTrue(plan.selectors["go"])
        self.assertTrue(plan.selectors["go_checks"])
        self.assertTrue(plan.selectors["python"])

    def test_python_harness_change_runs_goldens_but_not_c_builds(self) -> None:
        plan = _plan_for(["python/vmaf/core/result.py"])
        self.assertTrue(plan.selectors["python"])
        self.assertTrue(plan.selectors["golden_harness"])
        self.assertFalse(plan.selectors["c_core"])

    def test_requirements_lock_change_selects_python(self) -> None:
        plan = _plan_for(["requirements/locks/manifest.json"])
        self.assertTrue(plan.selectors["python"])

    def test_shell_change_selects_shell_lane_only(self) -> None:
        plan = _plan_for(["dev/scripts/probe.sh"])
        self.assertTrue(plan.selectors["shell"])
        self.assertTrue(plan.selectors["container"])
        self.assertFalse(plan.selectors["c_core"])

    def test_workflow_hosting_required_context_forces_full(self) -> None:
        plan = _plan_for([".github/workflows/lint-and-format.yml"])
        self.assertEqual(plan.mode, "full")
        self.assertTrue(plan.reason.startswith("global-ci-input:"))
        self.assertTrue(
            all(on for name, on in plan.selectors.items() if name not in OWN_PATHS_ONLY)
        )
        self.assertFalse(any(plan.selectors[name] for name in OWN_PATHS_ONLY - TIDY_SELECTORS))
        # The workflow hosts the lanes, so it is one of their own paths (not Metal's).
        self.assertEqual(
            {name for name in TIDY_SELECTORS if plan.selectors[name]},
            TIDY_SELECTORS - {"tidy_metal"},
        )

    def test_go_workflow_change_forces_full(self) -> None:
        plan = _plan_for([".github/workflows/go-ci.yml"])
        self.assertEqual(plan.mode, "full")
        self.assertTrue(plan.selectors["go_checks"])

    def test_release_version_change_runs_go_checks(self) -> None:
        plan = _plan_for([".release-please-manifest.json"])
        self.assertTrue(plan.selectors["go_checks"])

    def test_model_change_runs_go_checks(self) -> None:
        plan = _plan_for(["model/predictor_libx264.onnx"])
        self.assertTrue(plan.selectors["go_checks"])

    def test_ci_script_change_forces_full(self) -> None:
        plan = _plan_for(["scripts/ci/assertion-density.sh"])
        self.assertEqual(plan.mode, "full")

    def test_cppcheck_model_and_control_changes_run_native_gate(self) -> None:
        for path in (
            "scripts/ci/cppcheck-public-entrypoints.cfg",
            "scripts/ci/lint-configured.py",
            "scripts/ci/tests/test_cppcheck_posix_model.py",
            "scripts/ci/tests/test_lint_configured.py",
        ):
            with self.subTest(path=path):
                plan = _plan_for([path])
                self.assertEqual(plan.mode, "full")
                self.assertTrue(plan.selectors["c_core"])

    def test_unknown_root_forces_full(self) -> None:
        plan = _plan_for(["brand-new-top-level/thing.c"])
        self.assertEqual(plan.mode, "full")
        self.assertEqual(plan.reason, "unknown-path:brand-new-top-level/thing.c")

    def test_every_non_additive_status_forces_full(self) -> None:
        for status in ("D", "R100", "C75", "T", "U"):
            plan = _plan_for(["docs/x.md"], [status])
            self.assertEqual(plan.mode, "full", status)
            self.assertTrue(plan.reason.startswith("non-additive-change:"), status)

    def test_empty_diff_and_missing_enumeration_force_full(self) -> None:
        config = planner.load_config(CONFIG)
        self.assertEqual(planner.build_plan(config, (), None, "b", "h").mode, "full")
        self.assertEqual(
            planner.build_plan(config, None, "no-merge-base", "b", "h").reason, "no-merge-base"
        )

    def test_mixed_paths_are_sorted_and_deduplicated(self) -> None:
        plan = _plan_for(["docs/b.md", "docs/a.md", "docs/b.md"])
        self.assertEqual(plan.changed_paths, ("docs/a.md", "docs/b.md"))


class ParserContract(unittest.TestCase):
    def test_name_status_parser_is_nul_safe_and_preserves_rename_pairs(self) -> None:
        raw = b"M\0core/src/a.c\0R090\0old name.c\0new name.c\0A\0docs/x.md\0"
        changes = planner.parse_name_status(raw, max_paths=10)
        self.assertEqual([c.status for c in changes], ["M", "R090", "A"])
        self.assertEqual(changes[1].paths, ("old name.c", "new name.c"))

    def test_name_status_parser_rejects_missing_delimiter_and_bounds(self) -> None:
        with self.assertRaises(planner.PlanError):
            planner.parse_name_status(b"M\0core/src/a.c", max_paths=10)
        with self.assertRaises(planner.PlanError):
            planner.parse_name_status(b"M\0a\0M\0b\0", max_paths=1)

    def test_unsafe_paths_are_refused(self) -> None:
        for bad in (b"../x", b"/abs", b"a/../b"):
            with self.assertRaises(planner.PlanError):
                planner.parse_name_status(b"M\0" + bad + b"\0", max_paths=10)


class OutputContract(unittest.TestCase):
    def test_github_output_is_single_line_exact_booleans(self) -> None:
        plan = _plan_for(["docs/x.md"])
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            planner.write_github_output(out, plan)
            lines = out.read_text(encoding="utf-8").splitlines()
        kv = dict(line.split("=", 1) for line in lines)
        self.assertEqual(kv["mode"], "impact")
        self.assertEqual(kv["docs"], "true")
        self.assertEqual(kv["c_core"], "false")
        self.assertEqual(json.loads(kv["changed_paths_json"]), ["docs/x.md"])
        for value in kv.values():
            self.assertNotIn("\n", value)


class GitIntegration(unittest.TestCase):
    """Drive the real CLI against a throwaway repository."""

    def _repo(self) -> tuple[str, GitCommand]:
        tmp = tempfile.mkdtemp()
        env = {
            **os.environ,
            "GIT_AUTHOR_NAME": "t",
            "GIT_AUTHOR_EMAIL": "t@t",
            "GIT_COMMITTER_NAME": "t",
            "GIT_COMMITTER_EMAIL": "t@t",
        }

        def git(*a: str) -> str:
            result = run_command(
                [GIT, "-C", tmp, *a],
                allowed_executables=(GIT,),
                capture_output=True,
                text=True,
                check=True,
                env=env,
                timeout_seconds=60,
            ).stdout.strip()
            assert isinstance(result, str)
            return result

        git("init", "-q", "-b", "master")
        for d in ("docs", "core/src", ".github", "scripts/ci"):
            (Path(tmp) / d).mkdir(parents=True, exist_ok=True)
        (Path(tmp) / ".github" / "ci-impact.json").write_text(
            CONFIG.read_text(encoding="utf-8"), encoding="utf-8"
        )
        (Path(tmp) / "docs" / "a.md").write_text("a\n")
        (Path(tmp) / "core" / "src" / "a.c").write_text("int a;\n")
        git("add", "-A")
        git("commit", "-q", "-m", "base")
        return tmp, git

    def _run(self, tmp: str, event: str, base: str, head: str) -> dict[str, str]:
        with tempfile.TemporaryDirectory() as t:
            out = Path(t) / "gh"
            proc = run_command(
                [
                    sys.executable,
                    str(PLANNER),
                    "--event",
                    event,
                    "--base",
                    base,
                    "--head",
                    head,
                    "--repo-root",
                    tmp,
                    "--github-output",
                    str(out),
                ],
                allowed_executables=(sys.executable,),
                capture_output=True,
                text=True,
                timeout_seconds=60,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            return dict(line.split("=", 1) for line in out.read_text().splitlines())

    def test_pull_request_uses_merge_base_aware_diff(self) -> None:
        tmp, git = self._repo()
        base = git("rev-parse", "HEAD")
        git("checkout", "-q", "-b", "feature")
        (Path(tmp) / "docs" / "a.md").write_text("changed\n")
        git("commit", "-qam", "docs")
        head = git("rev-parse", "HEAD")
        # master moves on with a C change the PR does NOT contain.
        git("checkout", "-q", "master")
        (Path(tmp) / "core" / "src" / "a.c").write_text("int a = 1;\n")
        git("commit", "-qam", "c on master")
        new_master = git("rev-parse", "HEAD")
        kv = self._run(tmp, "pull_request", new_master, head)
        self.assertEqual(kv["mode"], "impact")
        self.assertEqual(kv["docs"], "true")
        self.assertEqual(
            kv["c_core"], "false", "merge-base diff must exclude master's own C change"
        )
        self.assertEqual(kv["base_sha"], base)

    def test_linear_push_uses_exact_before_and_head(self) -> None:
        tmp, git = self._repo()
        before = git("rev-parse", "HEAD")
        (Path(tmp) / "core" / "src" / "a.c").write_text("int a = 2;\n")
        git("commit", "-qam", "c")
        head = git("rev-parse", "HEAD")
        kv = self._run(tmp, "push", before, head)
        self.assertEqual(kv["mode"], "impact")
        self.assertEqual(kv["c_core"], "true")
        self.assertEqual(kv["docs"], "false")

    def test_zero_before_and_non_linear_push_fall_back_to_full(self) -> None:
        tmp, git = self._repo()
        head = git("rev-parse", "HEAD")
        self.assertEqual(self._run(tmp, "push", "0" * 40, head)["mode"], "full")
        git("checkout", "-q", "-b", "other")
        (Path(tmp) / "docs" / "b.md").write_text("b\n")
        git("add", "-A")
        git("commit", "-qm", "other")
        other = git("rev-parse", "HEAD")
        self.assertEqual(self._run(tmp, "push", other, head)["mode"], "full")

    def test_unrouted_event_is_full(self) -> None:
        tmp, git = self._repo()
        head = git("rev-parse", "HEAD")
        kv = self._run(tmp, "workflow_dispatch", head, head)
        self.assertEqual(kv["mode"], "full")
        self.assertTrue(kv["reason"].startswith("event-not-routed:"))


class TidyLaneRouting(unittest.TestCase):
    """Q-315: every hosted tidy lane runs only when a file of that lane changes."""

    def _lanes(self, plan: ImpactPlan) -> set[str]:
        return {lane for lane in TIDY_LANES if plan.selectors[f"tidy_{lane}"]}

    def test_a_cuda_file_selects_the_cuda_lane_and_the_cpu_gate_only(self) -> None:
        plan = _plan_for(["core/src/feature/cuda/integer_ssim/ssim_score.cu"])
        self.assertEqual(plan.mode, "impact")
        self.assertEqual(self._lanes(plan), {"cuda"})
        self.assertTrue(plan.selectors["c_core"])  # the cpu lane ("Tidy Ratchet")

    def test_each_lane_is_selected_by_its_own_files_and_no_other_lane(self) -> None:
        cases = {
            "cuda": "core/src/cuda/picture.c",
            "hip": "core/src/feature/hip/ciede_hip.c",
            "sycl": "core/src/feature/sycl/integer_ssim_sycl.cpp",
            "arm64": "core/src/feature/arm64/vif_neon.c",
            "clang": "core/test/fuzz/fuzz_json.c",
            "metal": "core/src/feature/metal/psnr_metal.mm",
        }
        for lane, path in cases.items():
            with self.subTest(lane=lane):
                self.assertEqual(self._lanes(_plan_for([path])), {lane})

    def test_a_docs_only_change_selects_no_lane_and_not_the_cpu_gate(self) -> None:
        plan = _plan_for(["docs/development/tidy-ratchet.md", "changelog.d/fixed/x.md"])
        self.assertEqual(self._lanes(plan), set())
        self.assertFalse(plan.selectors["c_core"])

    def test_a_shared_cpu_file_selects_no_device_lane(self) -> None:
        plan = _plan_for(["core/src/libvmaf.c"])
        self.assertEqual(self._lanes(plan), set())
        self.assertTrue(plan.selectors["c_core"])

    def test_the_ratchet_and_a_lane_baseline_select_the_lanes_they_define(self) -> None:
        self.assertEqual(self._lanes(_plan_for(["scripts/ci/tidy-baseline-hip.json"])), {"hip"})
        plan = _plan_for(["scripts/ci/tidy-ratchet.py"])
        self.assertEqual(self._lanes(plan), set(TIDY_LANES))

    def test_a_fallback_plan_keeps_the_lanes_off_unless_a_path_matches(self) -> None:
        plan = _plan_for(["scripts/ci/plan-ci-impact.py"])
        self.assertEqual(plan.mode, "full")
        self.assertEqual(self._lanes(plan), set())

    def test_every_baseline_only_source_matches_its_lane_patterns(self) -> None:
        """A lane's selector must see every file the lane alone measures (drift guard)."""
        root = CONFIG.parents[1]
        baselines = {
            lane: set(
                json.loads(
                    (root / f"scripts/ci/tidy-baseline-{lane}.json").read_text(encoding="utf-8")
                ).get("measured_sources", [])
            )
            for lane in ("cpu", *TIDY_LANES)
        }
        selectors = planner.load_config(CONFIG)["selectors"]
        for lane in TIDY_LANES:
            patterns = tuple(selectors[f"tidy_{lane}"]["patterns"])
            alone = {p for p in baselines[lane] - baselines["cpu"] if (root / p).exists()}
            unmatched = sorted(p for p in alone if not planner._matches(p, patterns))
            with self.subTest(lane=lane):
                self.assertEqual(unmatched, [], f"{lane}: files only this lane measures")


class OwnPathsOnlyContract(unittest.TestCase):
    """ADR-1700: `own_paths_only` selectors follow their own paths in full mode."""

    @staticmethod
    def _plan(config: dict[str, object], changes: tuple[object, ...] | None) -> ImpactPlan:
        fallback = None if changes is not None else "event-not-routed:schedule"
        return cast(ImpactPlan, planner.build_plan(config, changes, fallback, "b" * 40, "h" * 40))

    @staticmethod
    def _changes(*records: tuple[str, tuple[str, ...]]) -> tuple[object, ...]:
        return tuple(planner.Change(status=status, paths=paths) for status, paths in records)

    def test_the_exception_is_declared_for_exactly_the_tester_and_tidy_lane_selectors(self) -> None:
        selectors = planner.load_config(CONFIG)["selectors"]
        declared = {name for name, sel in selectors.items() if sel.get("own_paths_only")}
        self.assertEqual(declared, OWN_PATHS_ONLY)

    def test_ci_authority_change_alone_selects_neither_tester_build(self) -> None:
        plan = _plan_for(["scripts/ci/plan-ci-impact.py", ".standards-baseline.json"])
        self.assertEqual(plan.mode, "full")
        self.assertTrue(plan.reason.startswith("global-ci-input:"))
        for name, selected in plan.selectors.items():
            self.assertEqual(selected, name not in OWN_PATHS_ONLY, name)

    def test_a_full_plan_still_selects_a_tester_build_on_its_own_paths(self) -> None:
        cases = {
            ".github/workflows/docker-publish-tester.yml": {"tester_image"},
            ".github/workflows/windows-tester-bundle.yml": {
                "windows_tester_zip",
                "windows_tester_zip_sycl",
            },
            "scripts/ci/install-cuda-toolkit.sh": {"tester_image"},
            "scripts/ci/build-windows-tester-bundle.py": {
                "windows_tester_zip",
                "windows_tester_zip_sycl",
            },
        }
        for path, selected in cases.items():
            with self.subTest(path=path):
                plan = _plan_for([path, "Makefile"])
                self.assertEqual(plan.mode, "full")
                testers = OWN_PATHS_ONLY - TIDY_SELECTORS
                self.assertEqual({n for n in testers if plan.selectors[n]}, selected)

    def test_other_fallbacks_with_known_paths_follow_own_paths(self) -> None:
        config = planner.load_config(CONFIG)
        deleted = self._plan(config, self._changes(("D", ("docker/Dockerfile.tester",))))
        renamed = self._plan(
            config, self._changes(("R100", ("tools/rc1-tester/README.md", "docs/x.md")))
        )
        unknown = self._plan(config, self._changes(("A", ("new-root/x",))))
        empty = self._plan(config, ())
        for plan, expected in ((deleted, True), (renamed, True), (unknown, False), (empty, False)):
            with self.subTest(reason=plan.reason):
                self.assertEqual(plan.mode, "full")
                self.assertEqual(plan.selectors["tester_image"], expected)
                self.assertFalse(plan.selectors["windows_tester_zip"])
                self.assertTrue(plan.selectors["c_core"])

    def test_unknown_change_set_keeps_the_tester_builds_on(self) -> None:
        # A dispatch (publish) or the nightly schedule has no diff to read.
        plan = self._plan(planner.load_config(CONFIG), None)
        self.assertEqual(plan.mode, "full")
        self.assertTrue(all(plan.selectors.values()))

    def test_planted_mutation_without_the_property_runs_the_tester_builds(self) -> None:
        config = copy.deepcopy(planner.load_config(CONFIG))
        for name in OWN_PATHS_ONLY:
            del config["selectors"][name]["own_paths_only"]
        plan = cast(
            ImpactPlan,
            planner.build_plan(
                config,
                self._changes(("M", ("scripts/ci/plan-ci-impact.py",))),
                None,
                "b" * 40,
                "h" * 40,
            ),
        )
        self.assertTrue(all(plan.selectors[name] for name in OWN_PATHS_ONLY))

    def test_config_refuses_a_wide_or_malformed_exception(self) -> None:
        base = json.loads(CONFIG.read_text(encoding="utf-8"))
        mutations = {
            "inherits": {"own_paths_only": True, "patterns": ["a"], "inherits": ["c_core"]},
            "no patterns": {"own_paths_only": True, "patterns": []},
            "not a boolean": {"own_paths_only": "yes", "patterns": ["a"]},
        }
        for label, selector in mutations.items():
            with self.subTest(mutation=label), tempfile.TemporaryDirectory() as tmp:
                config = copy.deepcopy(base)
                config["selectors"]["tester_image"] = selector
                path = Path(tmp) / "ci-impact.json"
                path.write_text(json.dumps(config), encoding="utf-8")
                with self.assertRaises(planner.PlanError):
                    planner.load_config(path)

    def test_config_refuses_an_inheritance_cycle(self) -> None:
        selectors = {"a": {"inherits": ["b"]}, "b": {"inherits": ["a"]}, "c": {"patterns": ["x"]}}
        with self.assertRaises(planner.PlanError):
            planner.inheritance_order(selectors)
        self.assertEqual(planner.inheritance_order({"a": {"inherits": ["c"]}, "c": {}}), ["c", "a"])


class WorkflowContract(unittest.TestCase):
    @staticmethod
    def _job_block(workflow_text: str, job_id: str) -> str:
        match = re.search(
            rf"(?ms)^  {re.escape(job_id)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
            workflow_text,
        )
        if match is None:
            raise AssertionError(f"missing job {job_id}")
        return match.group(1)

    @staticmethod
    def _literal_run(job_block: str) -> str:
        """Extract the first literal run script from a raw workflow job."""
        lines = job_block.splitlines()
        try:
            start = lines.index("        run: |") + 1
        except ValueError as exc:
            raise AssertionError("job has no literal run script") from exc
        body = []
        for line in lines[start:]:
            if line.startswith("          "):
                body.append(line[10:])
            elif line:
                break
            else:
                body.append("")
        if not body:
            raise AssertionError("literal run script is empty")
        return "\n".join(body)

    def test_required_contexts_workflows_have_no_path_filters(self) -> None:
        """Every workflow hosting an aggregator-required check must always start;
        routing happens inside the job via the planner, never via `paths:`."""
        required = [
            line.strip().strip("',")
            for line in REQUIRED_AGGREGATOR.read_text(encoding="utf-8").splitlines()
            if line.strip().startswith("'")
        ]
        self.assertGreater(len(required), 10)
        hosting = set()
        for wf in (REPO_ROOT / ".github" / "workflows").glob("*.yml"):
            text = wf.read_text(encoding="utf-8")
            if any(f'name: "{name}"' in text or f"name: {name}" in text for name in required):
                hosting.add(wf)
        self.assertTrue(hosting)
        for wf in hosting:
            head = wf.read_text(encoding="utf-8").split("\njobs:", 1)[0]
            with self.subTest(workflow=wf.name):
                self.assertNotRegex(
                    head,
                    r"(?m)^\s+paths(-ignore)?:",
                    f"{wf.name} still uses a workflow-level path filter",
                )

    def _assert_consumer_contract(
        self,
        filename: str,
        selector: str,
        work_jobs: tuple[str, ...],
        gates: tuple[tuple[str, str, str], ...],
    ) -> set[str]:
        workflow_root = REPO_ROOT / ".github" / "workflows"
        text = (workflow_root / filename).read_text(encoding="utf-8")
        trigger = text.split("\njobs:", 1)[0]
        impact = self._job_block(text, "impact")
        with self.subTest(workflow=filename, job="impact"):
            self.assertRegex(trigger, r"(?m)^  push:\s*$")
            self.assertRegex(trigger, r"(?m)^  pull_request:\s*$")
            self.assertIn(f"selected: ${{{{ steps.impact.outputs.{selector} }}}}", impact)
            self.assertIn("fetch-depth: 0", impact)
            self.assertIn("scripts/ci/plan-ci-impact.py", impact)
        for work_job in work_jobs:
            block = self._job_block(text, work_job)
            with self.subTest(workflow=filename, job=work_job):
                self.assertIn("needs: impact", block)
                self.assertIn("needs.impact.outputs.selected == 'true'", block)
        for gate_job, check_name, work_job in gates:
            block = self._job_block(text, gate_job)
            with self.subTest(workflow=filename, job=gate_job):
                # ADR-2169: the gate also needs the tier, and a tier that does not own
                # the gate leaves it skipped (a skipped required context is accepted
                # by the aggregator only for a tier that does not owe it).
                self.assertIn(f"needs: [tier, impact, {work_job}]", block)
                self.assertRegex(
                    block, r"if: always\(\) && needs\.tier\.outputs\.(light|full) == 'true'"
                )
                self.assertIn(f"name: {check_name}", block)
                if "gate_leg_result.py" in block:
                    self.assertIn(f'gate_leg_result.py --job "{check_name} work"', block)
                else:
                    self.assertIn('if [ "$PLAN_RESULT" != success ]', block)
                    self.assertIn("true:success|false:skipped", block)
        return {check_name for _, check_name, _ in gates}

    def test_required_consumers_use_fail_closed_planner_work_gate_contract(self) -> None:
        expected_strict = set()
        for filename, contract in REQUIRED_CONSUMER_CONTRACTS.items():
            expected_strict.update(self._assert_consumer_contract(filename, *contract))
        aggregator = REQUIRED_AGGREGATOR.read_text(encoding="utf-8")
        match = re.search(r"const strictMustReport = \[(.*?)\];", aggregator, re.DOTALL)
        self.assertIsNotNone(match)
        strict_names = set(re.findall(r"'([^']+)'", match.group(1) if match else ""))
        self.assertLessEqual(expected_strict, strict_names)

    def _assert_gate_leg_result_states(self, filename: str, check_name: str) -> None:
        for plan_result in ("success", "failure", "cancelled", "skipped"):
            for selected in ("true", "false", ""):
                for work_result in ("success", "skipped", "failure", "cancelled"):
                    expected = plan_result == "success" and (
                        selected,
                        work_result,
                    ) in {("true", "success"), ("false", "skipped")}
                    jobs = [{"name": f"{check_name} work", "conclusion": work_result}]
                    reason = glr.judge(plan_result, selected, f"{check_name} work", jobs)
                    with self.subTest(
                        workflow=filename,
                        gate=check_name,
                        plan=plan_result,
                        selected=selected,
                        work=work_result,
                    ):
                        self.assertEqual(
                            reason is None,
                            expected,
                            f"plan={plan_result} selected={selected} "
                            f"work={work_result} produced reason={reason!r}",
                        )

    def test_gate_scripts_accept_only_the_two_valid_state_tuples(self) -> None:
        workflow_root = REPO_ROOT / ".github" / "workflows"
        for filename, (_selector, _work_jobs, gates) in REQUIRED_CONSUMER_CONTRACTS.items():
            text = (workflow_root / filename).read_text(encoding="utf-8")
            for gate_job, check_name, _work_job in gates:
                block = self._job_block(text, gate_job)
                if "gate_leg_result.py" in block:
                    self._assert_gate_leg_result_states(filename, check_name)
                    continue
                script = self._literal_run(block)
                for plan_result in ("success", "failure", "cancelled", "skipped"):
                    for selected in ("true", "false", ""):
                        for work_result in ("success", "skipped", "failure", "cancelled"):
                            expected = plan_result == "success" and (
                                selected,
                                work_result,
                            ) in {("true", "success"), ("false", "skipped")}
                            result = run_command(
                                [BASH, "-eu", "-o", "pipefail", "-c", script],
                                allowed_executables=(BASH,),
                                capture_output=True,
                                text=True,
                                env={
                                    **os.environ,
                                    "PLAN_RESULT": plan_result,
                                    "SELECTED": selected,
                                    "WORK_RESULT": work_result,
                                },
                                timeout_seconds=60,
                            )
                            with self.subTest(
                                workflow=filename,
                                gate=check_name,
                                plan=plan_result,
                                selected=selected,
                                work=work_result,
                            ):
                                self.assertEqual(
                                    result.returncode == 0,
                                    expected,
                                    f"stdout={result.stdout!r}; stderr={result.stderr!r}",
                                )


if __name__ == "__main__":
    unittest.main()
