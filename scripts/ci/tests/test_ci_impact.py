#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Contract tests for scripts/ci/plan-ci-impact.py and .github/ci-impact.json.

Run with:  python3 -m unittest scripts/ci/tests/test_ci_impact.py
No third-party dependencies (the CI runners have only the stdlib).
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PLANNER = REPO_ROOT / "scripts" / "ci" / "plan-ci-impact.py"
CONFIG = REPO_ROOT / ".github" / "ci-impact.json"
REQUIRED_AGGREGATOR = REPO_ROOT / ".github" / "workflows" / "required-aggregator.yml"
GIT = shutil.which("git") or "/usr/bin/git"


def _load_planner():
    spec = importlib.util.spec_from_file_location("plan_ci_impact", PLANNER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    # dataclasses resolve `cls.__module__` through sys.modules when the module
    # uses `from __future__ import annotations`; register before executing.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


planner = _load_planner()


def _plan_for(paths: list[str], statuses: list[str] | None = None):
    config = planner.load_config(CONFIG)
    statuses = statuses or ["M"] * len(paths)
    changes = tuple(
        planner.Change(status=s, paths=(p,)) for s, p in zip(statuses, paths, strict=True)
    )
    return planner.build_plan(config, changes, None, "b" * 40, "h" * 40)


class ConfigContract(unittest.TestCase):
    def test_config_is_canonical_json(self):
        raw = CONFIG.read_text(encoding="utf-8")
        parsed = json.loads(raw)
        self.assertEqual(raw, json.dumps(parsed, indent=2, ensure_ascii=False) + "\n")

    def test_config_loads_and_every_selector_has_patterns_or_inherits(self):
        config = planner.load_config(CONFIG)
        for name, selector in config["selectors"].items():
            self.assertTrue(
                selector.get("patterns") or selector.get("inherits"),
                f"selector {name} selects nothing",
            )

    def test_every_top_level_repo_entry_is_known(self):
        """A path the planner cannot classify forces mode=full (fail-closed).
        Keep the map in step with the tree so routing actually happens."""
        config = planner.load_config(CONFIG)
        tracked = subprocess.run(  # noqa: S603 -- fixed argv, absolute git
            [GIT, "-C", str(REPO_ROOT), "ls-tree", "--name-only", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.split()
        known_prefixes = {p.rstrip("/") for p in config["known_prefixes"]}
        known_files = set(config["known_files"])
        unknown = [e for e in tracked if e not in known_prefixes and e not in known_files]
        self.assertEqual(unknown, [], f"top-level entries missing from ci-impact.json: {unknown}")

    def test_ci_authority_files_are_full_patterns(self):
        config = planner.load_config(CONFIG)
        for path in (
            ".github/ci-impact.json",
            ".github/workflows/required-aggregator.yml",
            ".pre-commit-config.yaml",
            "Makefile",
            "scripts/ci/plan-ci-impact.py",
        ):
            self.assertTrue(planner._matches(path, tuple(config["full_patterns"])), path)


class RoutingContract(unittest.TestCase):
    def test_docs_only_change_is_impact_mode_with_no_c_lane(self):
        plan = _plan_for(["docs/usage/cli.md", "changelog.d/fixed/x.md"])
        self.assertEqual(plan.mode, "impact")
        self.assertTrue(plan.selectors["docs"])
        for lane in ("c_core", "python", "go", "rust", "golden_harness", "tiny_ai"):
            self.assertFalse(plan.selectors[lane], lane)

    def test_c_change_selects_core_and_its_dependents(self):
        plan = _plan_for(["core/src/feature/adm_tools.c"])
        self.assertEqual(plan.mode, "impact")
        self.assertTrue(plan.selectors["c_core"])
        self.assertTrue(plan.selectors["golden_harness"])
        self.assertTrue(plan.selectors["tiny_ai"])
        self.assertFalse(plan.selectors["go"])
        self.assertFalse(plan.selectors["docs"])

    def test_model_json_change_runs_goldens(self):
        plan = _plan_for(["model/vmaf_v0.6.1.json"])
        self.assertTrue(plan.selectors["c_core"])
        self.assertTrue(plan.selectors["golden_harness"])

    def test_golden_fixture_change_runs_goldens(self):
        plan = _plan_for(["python/test/resource/yuv/src01_hrc00_576x324.yuv"])
        self.assertTrue(plan.selectors["golden_harness"])

    def test_go_change_selects_only_go(self):
        plan = _plan_for(["pkg/predictor/predictor.go", "go.mod"])
        self.assertTrue(plan.selectors["go"])
        self.assertFalse(plan.selectors["c_core"])
        self.assertFalse(plan.selectors["python"])

    def test_python_harness_change_runs_goldens_but_not_c_builds(self):
        plan = _plan_for(["python/vmaf/core/result.py"])
        self.assertTrue(plan.selectors["python"])
        self.assertTrue(plan.selectors["golden_harness"])
        self.assertFalse(plan.selectors["c_core"])

    def test_shell_change_selects_shell_lane_only(self):
        plan = _plan_for(["dev/scripts/probe.sh"])
        self.assertTrue(plan.selectors["shell"])
        self.assertTrue(plan.selectors["container"])
        self.assertFalse(plan.selectors["c_core"])

    def test_workflow_hosting_required_context_forces_full(self):
        plan = _plan_for([".github/workflows/lint-and-format.yml"])
        self.assertEqual(plan.mode, "full")
        self.assertTrue(plan.reason.startswith("global-ci-input:"))
        self.assertTrue(all(plan.selectors.values()))

    def test_ci_script_change_forces_full(self):
        plan = _plan_for(["scripts/ci/assertion-density.sh"])
        self.assertEqual(plan.mode, "full")

    def test_unknown_root_forces_full(self):
        plan = _plan_for(["brand-new-top-level/thing.c"])
        self.assertEqual(plan.mode, "full")
        self.assertEqual(plan.reason, "unknown-path:brand-new-top-level/thing.c")

    def test_every_non_additive_status_forces_full(self):
        for status in ("D", "R100", "C75", "T", "U"):
            plan = _plan_for(["docs/x.md"], [status])
            self.assertEqual(plan.mode, "full", status)
            self.assertTrue(plan.reason.startswith("non-additive-change:"), status)

    def test_empty_diff_and_missing_enumeration_force_full(self):
        config = planner.load_config(CONFIG)
        self.assertEqual(planner.build_plan(config, (), None, "b", "h").mode, "full")
        self.assertEqual(
            planner.build_plan(config, None, "no-merge-base", "b", "h").reason, "no-merge-base"
        )

    def test_mixed_paths_are_sorted_and_deduplicated(self):
        plan = _plan_for(["docs/b.md", "docs/a.md", "docs/b.md"])
        self.assertEqual(plan.changed_paths, ("docs/a.md", "docs/b.md"))


class ParserContract(unittest.TestCase):
    def test_name_status_parser_is_nul_safe_and_preserves_rename_pairs(self):
        raw = b"M\0core/src/a.c\0R090\0old name.c\0new name.c\0A\0docs/x.md\0"
        changes = planner.parse_name_status(raw, max_paths=10)
        self.assertEqual([c.status for c in changes], ["M", "R090", "A"])
        self.assertEqual(changes[1].paths, ("old name.c", "new name.c"))

    def test_name_status_parser_rejects_missing_delimiter_and_bounds(self):
        with self.assertRaises(planner.PlanError):
            planner.parse_name_status(b"M\0core/src/a.c", max_paths=10)
        with self.assertRaises(planner.PlanError):
            planner.parse_name_status(b"M\0a\0M\0b\0", max_paths=1)

    def test_unsafe_paths_are_refused(self):
        for bad in (b"../x", b"/abs", b"a/../b"):
            with self.assertRaises(planner.PlanError):
                planner.parse_name_status(b"M\0" + bad + b"\0", max_paths=10)


class OutputContract(unittest.TestCase):
    def test_github_output_is_single_line_exact_booleans(self):
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

    def _repo(self):
        tmp = tempfile.mkdtemp()
        env = {
            **os.environ,
            "GIT_AUTHOR_NAME": "t",
            "GIT_AUTHOR_EMAIL": "t@t",
            "GIT_COMMITTER_NAME": "t",
            "GIT_COMMITTER_EMAIL": "t@t",
        }

        def git(*a):
            return subprocess.run(  # noqa: S603 -- fixed argv, absolute git
                [GIT, "-C", tmp, *a], capture_output=True, text=True, check=True, env=env
            ).stdout.strip()

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

    def _run(self, tmp, event, base, head):
        with tempfile.TemporaryDirectory() as t:
            out = Path(t) / "gh"
            proc = subprocess.run(  # noqa: S603 -- fixed argv: our own planner
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
                capture_output=True,
                text=True,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            return dict(line.split("=", 1) for line in out.read_text().splitlines())

    def test_pull_request_uses_merge_base_aware_diff(self):
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

    def test_linear_push_uses_exact_before_and_head(self):
        tmp, git = self._repo()
        before = git("rev-parse", "HEAD")
        (Path(tmp) / "core" / "src" / "a.c").write_text("int a = 2;\n")
        git("commit", "-qam", "c")
        head = git("rev-parse", "HEAD")
        kv = self._run(tmp, "push", before, head)
        self.assertEqual(kv["mode"], "impact")
        self.assertEqual(kv["c_core"], "true")
        self.assertEqual(kv["docs"], "false")

    def test_zero_before_and_non_linear_push_fall_back_to_full(self):
        tmp, git = self._repo()
        head = git("rev-parse", "HEAD")
        self.assertEqual(self._run(tmp, "push", "0" * 40, head)["mode"], "full")
        git("checkout", "-q", "-b", "other")
        (Path(tmp) / "docs" / "b.md").write_text("b\n")
        git("add", "-A")
        git("commit", "-qm", "other")
        other = git("rev-parse", "HEAD")
        self.assertEqual(self._run(tmp, "push", other, head)["mode"], "full")

    def test_unrouted_event_is_full(self):
        tmp, git = self._repo()
        head = git("rev-parse", "HEAD")
        kv = self._run(tmp, "workflow_dispatch", head, head)
        self.assertEqual(kv["mode"], "full")
        self.assertTrue(kv["reason"].startswith("event-not-routed:"))


class WorkflowContract(unittest.TestCase):
    def test_required_contexts_workflows_have_no_path_filters(self):
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
            self.assertNotRegex(
                head, r"^\s+paths(-ignore)?:", f"{wf.name} still uses a workflow-level path filter"
            )


if __name__ == "__main__":
    unittest.main()
