#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Failure controls for ADR-1247, with actual disposable Git source bindings."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import CommandResult
from scripts.lib.safe_subprocess import run as run_command

# A hang detector, not a timing assertion: these subprocesses finish in tens of
# milliseconds locally, but a loaded CI runner has blown a 10-second cap and the
# TimeoutExpired then reads as a real test failure (bug ledger L-76). 120s still
# catches a genuine hang long before the job's own timeout.
SUBPROCESS_TIMEOUT_S = 120

SCRIPT = Path(__file__).resolve().parents[1] / "scorecard_gate.py"
spec = importlib.util.spec_from_file_location("scorecard_gate", SCRIPT)
assert spec and spec.loader
gate = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gate
spec.loader.exec_module(gate)
REPO = "VMAFx/vmafx"
SHA = "a" * 40


def report(scope: str = "master", score: int = 10) -> dict[str, Any]:
    names = sorted(gate.LOCAL_CHECKS if scope == "local" else gate.WEIGHTS)
    return {
        "date": "2026-09-08T16:15:55Z",
        "repo": {
            "name": "file://." if scope == "local" else f"github.com/{REPO}",
            "commit": "unknown" if scope == "local" else SHA,
        },
        "scorecard": {"version": gate.VERSION, "commit": gate.TOOL_COMMIT},
        "score": float(score),
        "checks": [{"name": name, "score": score, "reason": "fixture scored"} for name in names],
    }


def rescore(data: dict[str, Any]) -> None:
    values = [(gate.WEIGHTS[c["name"]], c["score"]) for c in data["checks"] if c["score"] >= 0]
    data["score"] = round(sum(w * s for w, s in values) / sum(w for w, _ in values), 1)


class ScorecardReportTests(unittest.TestCase):
    def test_complete_reports_have_distinct_scopes(self) -> None:
        for scope, count in [("master", 18), ("local", 11)]:
            result = gate.assess(report(scope), scope, REPO, SHA)
            self.assertEqual(len(result.checks), count)
            self.assertEqual(result.failures, [])
        self.assertIn(
            "not the full repository score",
            gate.markdown(gate.assess(report("local"), "local", REPO, SHA)),
        )

    def test_stale_repo_commit_and_tool_fail_closed(self) -> None:
        for section, key, value in [
            ("repo", "commit", "b" * 40),
            ("repo", "name", "github.com/other/repo"),
            ("scorecard", "version", "v5.4.0"),
            ("scorecard", "commit", "b" * 40),
        ]:
            with self.subTest(section=section, key=key):
                data = report()
                data[section][key] = value
                with self.assertRaises(gate.InvalidReport):
                    gate.assess(data, "master", REPO, SHA)

    def test_local_unknown_commit_never_satisfies_master(self) -> None:
        with self.assertRaises(gate.InvalidReport):
            gate.assess(report("local"), "master", REPO, SHA)
        with self.assertRaises(gate.InvalidReport):
            gate.assess(report(), "local", REPO, SHA)

    def test_partial_duplicate_extra_empty_and_wrong_sets_rejected(self) -> None:
        original = report()
        for checks in [
            [],
            original["checks"][:-1],
            original["checks"] + [original["checks"][0]],
            original["checks"] + [{"name": "NewCheck", "score": 10, "reason": "new"}],
            [c for c in original["checks"] if c["name"] in gate.LOCAL_CHECKS],
        ]:
            data = copy.deepcopy(original)
            data["checks"] = checks
            with self.assertRaises(gate.InvalidReport):
                gate.assess(data, "master", REPO, SHA)

    def test_malformed_values_never_satisfy(self) -> None:
        for bad in [True, -2, 11, 8.5, "10", None, float("nan")]:
            data = report()
            data["checks"][0]["score"] = bad
            with self.subTest(bad=bad), self.assertRaises(gate.InvalidReport):
                gate.assess(data, "master", REPO, SHA)
        for bad in [True, "10", float("inf"), float("nan"), 100, 9.99]:
            data = report()
            data["score"] = bad
            with self.subTest(aggregate=bad), self.assertRaises(gate.InvalidReport):
                gate.assess(data, "master", REPO, SHA)

    def test_malformed_date_and_reason_rejected(self) -> None:
        data = report()
        data["date"] = "not a date"
        with self.assertRaises(gate.InvalidReport):
            gate.assess(data, "master", REPO, SHA)
        data = report()
        data["checks"][0]["reason"] = ""
        with self.assertRaises(gate.InvalidReport):
            gate.assess(data, "master", REPO, SHA)

    def test_scanner_errors_cannot_improve_aggregate_to_pass(self) -> None:
        for name in gate.WEIGHTS:
            data = report()
            next(c for c in data["checks"] if c["name"] == name).update(
                score=-1, reason="internal error: unavailable"
            )
            rescore(data)
            result = gate.assess(data, "master", REPO, SHA)
            self.assertEqual(result.aggregate, 10)
            self.assertTrue(result.failures, name)
            self.assertIn("inconclusive", gate.markdown(result))

    def test_exact_no_release_reason_is_unassessed_not_signed(self) -> None:
        data = report()
        signed = next(c for c in data["checks"] if c["name"] == "Signed-Releases")
        signed.update(score=-1, reason="no releases found")
        rescore(data)
        result = gate.assess(data, "master", REPO, SHA)
        self.assertEqual(result.failures, [])
        self.assertIn("unassessed: no releases (not signed)", gate.markdown(result))
        signed["reason"] += ": internal error"
        self.assertTrue(gate.assess(data, "master", REPO, SHA).failures)

    def test_badge_and_review_zero_remain_zero_in_weighted_score(self) -> None:
        data = report()
        for check in data["checks"]:
            if check["name"] in {"CII-Best-Practices", "Code-Review"}:
                check.update(score=0, reason="no approvals/badge")
        rescore(data)
        result = gate.assess(data, "master", REPO, SHA)
        self.assertLess(result.aggregate, 10)
        self.assertEqual(
            [(c.name, c.state) for c in result.checks if c.score == 0],
            [("CII-Best-Practices", "zero"), ("Code-Review", "zero")],
        )

    def test_floor_uses_unrounded_weighted_score(self) -> None:
        data = report(score=9)
        # Total weight 44; lose 23 points => 373/44=8.47727, displayed 8.5.
        remaining = 23
        for check in data["checks"]:
            weight = gate.WEIGHTS[check["name"]]
            decrease = min(9, remaining // weight)
            check["score"] -= decrease
            remaining -= decrease * weight
        self.assertEqual(remaining, 0)
        rescore(data)
        self.assertEqual(data["score"], 8.5)
        self.assertTrue(gate.assess(data, "master", REPO, SHA).failures)
        self.assertTrue(gate.assess(report(score=8), "master", REPO, SHA).failures)

    def test_forged_rounded_aggregate_rejected(self) -> None:
        data = report(score=1)
        data["score"] = 10
        with self.assertRaises(gate.InvalidReport):
            gate.assess(data, "master", REPO, SHA)

    def test_final_master_ref_rejects_graphql_archive_head_race(self) -> None:
        good: dict[str, Any] = {
            "ref": "refs/heads/master",
            "object": {"type": "commit", "sha": SHA},
        }
        self.assertEqual(gate.master_identity(good, SHA)["commit"], SHA)
        for key, value in [("sha", "b" * 40), ("type", "tag")]:
            bad = copy.deepcopy(good)
            bad["object"][key] = value
            with self.assertRaises(gate.InvalidReport):
                gate.master_identity(bad, SHA)
        for bad in [{}, {"ref": "refs/heads/other", "object": good["object"]}]:
            with self.assertRaises(gate.InvalidReport):
                gate.master_identity(bad, SHA)

    def test_summary_escapes_untrusted_report_text(self) -> None:
        data = report()
        data["checks"][0]["reason"] = "<script>|x\n::error::spoof"
        text = gate.markdown(gate.assess(data, "master", REPO, SHA))
        self.assertNotIn("<script>", text)
        self.assertNotIn("\n::error::", text)
        self.assertIn("&#124;", text)

    def test_master_cli_requires_final_matching_ref_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "report.json"
            reference = root / "ref.json"
            source.write_text(json.dumps(report()))
            command = [
                sys.executable,
                "-B",
                str(SCRIPT),
                "master",
                "--sha",
                SHA,
                "--repository",
                REPO,
                "--report",
                str(source),
                "--receipt",
                str(root / "gate.json"),
            ]

            def run(args: list[str]) -> int:
                return run_command(
                    args,
                    allowed_executables=(sys.executable,),
                    capture_output=True,
                    timeout_seconds=SUBPROCESS_TIMEOUT_S,
                ).returncode

            self.assertNotEqual(run(command), 0)
            reference.write_text(
                json.dumps(
                    {"ref": "refs/heads/master", "object": {"type": "commit", "sha": "b" * 40}}
                )
            )
            self.assertNotEqual(run([*command, "--master-ref", str(reference)]), 0)
            reference.write_text(
                json.dumps({"ref": "refs/heads/master", "object": {"type": "commit", "sha": SHA}})
            )
            self.assertEqual(run([*command, "--master-ref", str(reference)]), 0)

    def test_duplicate_json_keys_missing_and_invalid_files_fail(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "report.json"
            path.write_text('{"score": 1, "score": 10}')
            with self.assertRaises(gate.InvalidReport):
                gate.load_json(path)
            path.write_text("{")
            with self.assertRaises(ValueError):
                gate.load_json(path)
            path.unlink()
            with self.assertRaises(OSError):
                gate.load_json(path)


class SourceBindingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.root.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        self.env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        self.command("init", "-q")
        self.command("config", "user.name", "Fixture")
        self.command("config", "user.email", "fixture@example.invalid")
        (self.root / "input").write_text("original\n")
        (self.root / ".gitignore").write_text("ignored\n")
        self.command("add", ".")
        self.command("commit", "-qm", "fixture")
        self.sha = self.command("rev-parse", "HEAD").strip()

    def command(self, *args: str) -> str:
        executable = shutil.which("git")
        self.assertIsNotNone(executable)
        assert executable is not None
        return run_command(
            [executable, "-C", str(self.root), *args],
            allowed_executables=(executable,),
            env=self.env,
            check=True,
            capture_output=True,
            text=True,
            timeout_seconds=SUBPROCESS_TIMEOUT_S,
        ).stdout

    def test_clean_source_binding_and_only_generated_report_allowed(self) -> None:
        before = gate.source_identity(self.root, self.sha)
        (self.root / "scorecard-local.json").write_text("{}")
        self.assertEqual(before, gate.source_identity(self.root, self.sha, "scorecard-local.json"))
        with self.assertRaises(gate.InvalidReport):
            gate.source_identity(self.root, self.sha)

    def test_stale_head_and_modified_assumed_unchanged_input_rejected(self) -> None:
        with self.assertRaises(gate.InvalidReport):
            gate.source_identity(self.root, "b" * 40)
        self.command("update-index", "--assume-unchanged", "input")
        (self.root / "input").write_text("changed\n")
        self.assertEqual(self.command("status", "--porcelain"), "")
        with self.assertRaises(gate.InvalidReport):
            gate.source_identity(self.root, self.sha)

    def test_literal_symlink_target_is_not_path_normalized(self) -> None:
        (self.root / "link").symlink_to("./input")
        self.command("add", "link")
        self.command("commit", "-qm", "literal symlink")
        sha = self.command("rev-parse", "HEAD").strip()
        self.assertEqual(gate.source_identity(self.root, sha)["files"], 3)

    def test_metadata_untracked_escaping_and_cyclic_symlink_targets_rejected(self) -> None:
        for target in [
            ".git/hidden-input",
            "ignored",
            "missing",
            "../outside",
            "loop",
            "Dockerfile.hidden/suffix",
            "/etc/passwd",
        ]:
            with self.subTest(target=target):
                link = self.root / "Dockerfile.hidden"
                link.symlink_to(target)
                (self.root / "loop").symlink_to("Dockerfile.hidden")
                self.command("add", "Dockerfile.hidden", "loop")
                self.command("commit", "-qm", "unbound links")
                (self.root / ".git/hidden-input").write_text("FROM alpine:latest\n")
                sha = self.command("rev-parse", "HEAD").strip()
                with self.assertRaises(gate.InvalidReport):
                    gate.source_identity(self.root, sha)
                self.command("rm", "Dockerfile.hidden", "loop")
                self.command("commit", "-qm", "remove links")

    def test_tracked_directory_and_file_link_chains_remain_supported(self) -> None:
        (self.root / "nested").mkdir()
        (self.root / "nested/file").write_text("tracked\n")
        (self.root / "dir-link").symlink_to("nested")
        (self.root / "file-link").symlink_to("dir-link/file")
        self.command("add", ".")
        self.command("commit", "-qm", "tracked link chains")
        sha = self.command("rev-parse", "HEAD").strip()
        self.assertEqual(gate.source_identity(self.root, sha)["files"], 5)
        # Even a chain ending in committed bytes cannot transit mutable metadata.
        (self.root / "file-link").unlink()
        (self.root / "file-link").symlink_to(".git/alias")
        (self.root / ".git/alias").symlink_to("../nested/file")
        self.command("add", "file-link")
        self.command("commit", "-qm", "mutable chain")
        with self.assertRaises(gate.InvalidReport):
            gate.source_identity(self.root, self.command("rev-parse", "HEAD").strip())

    def test_ignored_input_and_type_replacement_rejected(self) -> None:
        (self.root / "ignored").write_text("unmeasured")
        with self.assertRaises(gate.InvalidReport):
            gate.source_identity(self.root, self.sha)
        (self.root / "ignored").unlink()
        (self.root / "input").unlink()
        (self.root / "input").symlink_to(".gitignore")
        with self.assertRaises(gate.InvalidReport):
            gate.source_identity(self.root, self.sha)

    def test_git_environment_cannot_redirect_source_binding(self) -> None:
        before = gate.source_identity(self.root, self.sha)
        poison = {
            name: str(Path(self.temp.name) / "absent")
            for name in [
                "GIT_DIR",
                "GIT_COMMON_DIR",
                "GIT_WORK_TREE",
                "GIT_INDEX_FILE",
                "GIT_CONFIG_PARAMETERS",
            ]
        }
        with mock.patch.dict(os.environ, poison):
            self.assertEqual(before, gate.source_identity(self.root, self.sha))

    @staticmethod
    def run_cli(command: list[str], env: dict[str, str]) -> CommandResult:
        return run_command(
            command,
            allowed_executables=(sys.executable,),
            env=env,
            capture_output=True,
            timeout_seconds=SUBPROCESS_TIMEOUT_S,
        )

    def test_actual_cli_missing_before_or_wrong_run_never_passes(self) -> None:
        output = Path(self.temp.name) / "receipt.json"
        snapshot = Path(self.temp.name) / "before.json"
        env = dict(self.env, GITHUB_RUN_ID="100", GITHUB_RUN_ATTEMPT="1")
        common = [
            sys.executable,
            "-B",
            str(SCRIPT),
            "--root",
            str(self.root),
            "--sha",
            self.sha,
            "--repository",
            REPO,
        ]
        result = self.run_cli([*common, "snapshot", "--receipt", str(snapshot)], env)
        self.assertEqual(result.returncode, 0, result.stderr)
        data = report("local")
        (self.root / "scorecard-local.json").write_text(json.dumps(data))
        command = [
            *common,
            "local",
            "--report",
            str(self.root / "scorecard-local.json"),
            "--receipt",
            str(output),
            "--snapshot",
            str(snapshot),
        ]
        result = self.run_cli(command, env)
        self.assertEqual(result.returncode, 0, result.stderr)
        env["GITHUB_RUN_ATTEMPT"] = "2"
        self.assertNotEqual(self.run_cli(command, env).returncode, 0)
        snapshot.unlink()
        self.assertNotEqual(self.run_cli(command, env).returncode, 0)


if __name__ == "__main__":
    unittest.main()
