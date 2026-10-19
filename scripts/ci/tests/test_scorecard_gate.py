#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Failure controls for ADR-1247, with actual disposable Git source bindings."""

from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
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

from scripts.lib.safe_subprocess import CommandFailed, CommandResult
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

    def test_previous_reviewed_tool_is_refused(self) -> None:
        # The identity reviewed before the v5.6.0 move; a report from it must
        # not pass once the pins have moved on.
        data = report()
        data["scorecard"] = {
            "version": "v5.5.0",
            "commit": "c395761df6afe1a69e476bc60a013a94bcbc153f",
        }
        with self.assertRaisesRegex(gate.InvalidReport, "differs from reviewed tool"):
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

            # No gh on PATH: a moved master cannot have its ancestry read, so it fails
            # closed without a network request (ADR-1686).
            env = dict(os.environ, PATH=str(root))

            def run(args: list[str]) -> int:
                return run_command(
                    args,
                    allowed_executables=(sys.executable,),
                    env=env,
                    capture_output=True,
                    timeout_seconds=SUBPROCESS_TIMEOUT_S,
                ).returncode

            self.assertNotEqual(run(command), 0)
            reference.write_text(
                json.dumps(
                    {"ref": "refs/heads/master", "object": {"type": "commit", "sha": "b" * 40}}
                )
            )
            self.assertEqual(run([*command, "--master-ref", str(reference)]), 1)
            self.assertFalse((root / "gate.json").exists())
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


# Run 37276297218 (ADR-1686): the gate of master b0d991df7 read master at 53e582831,
# one commit ahead. These are that run's final ref and GitHub's comparison of the two.
BASE = "b0d991df7f269c4a4aad0a4333ee1b3f4bd870ff"
NEWER = "53e58283139c2eb8b6290ae12a33d7d12da4c51c"
MOVED_REF: dict[str, Any] = {
    "ref": "refs/heads/master",
    "node_id": "REF_kwDOSqOYRrFyZWZzL2hlYWRzL21hc3Rlcg",
    "object": {"sha": NEWER, "type": "commit"},
}
DESCENDANT: dict[str, Any] = {
    "ahead_by": 1,
    "base_commit": BASE,
    "behind_by": 0,
    "head_commit": NEWER,
    "merge_base_commit": BASE,
    "status": "ahead",
}


def never_compare(newer: str) -> dict[str, object]:
    raise AssertionError(f"no comparison may be read here, asked for {newer}")


class MasterSupersessionTests(unittest.TestCase):
    """Invalid ref fails, descendant supersedes, non-descendant fails, same SHA binds."""

    def test_invalid_final_ref_fails_without_reading_a_comparison(self) -> None:
        target = MOVED_REF["object"]
        for bad in [
            {},
            {"ref": "refs/heads/master"},
            {"ref": "refs/heads/master", "object": "commit"},
            {"ref": "refs/heads/other", "object": target},
            {"ref": "refs/heads/master", "object": {**target, "type": "tag"}},
            {"ref": "refs/heads/master", "object": {**target, "sha": NEWER.upper()}},
            {"ref": "refs/heads/master", "object": {**target, "sha": NEWER[:39]}},
            {"ref": "refs/heads/master", "object": {**target, "sha": None}},
        ]:
            with self.subTest(bad=bad), self.assertRaises(gate.InvalidReport):
                gate.master_identity(bad, BASE, never_compare)
        with self.assertRaises(gate.InvalidReport):
            gate.master_identity(MOVED_REF, BASE[:39], never_compare)

    def test_same_commit_binds_without_reading_a_comparison(self) -> None:
        same = copy.deepcopy(MOVED_REF)
        same["object"]["sha"] = BASE
        self.assertEqual(
            gate.master_identity(same, BASE, never_compare),
            {"ref": "refs/heads/master", "commit": BASE},
        )

    def test_descendant_master_supersedes_the_run(self) -> None:
        asked: list[str] = []

        def compare(newer: str) -> dict[str, object]:
            asked.append(newer)
            return copy.deepcopy(DESCENDANT)

        with self.assertRaises(gate.Superseded) as raised:
            gate.master_identity(MOVED_REF, BASE, compare)
        self.assertEqual(asked, [NEWER])
        self.assertEqual((raised.exception.newer, raised.exception.ahead_by), (NEWER, 1))
        self.assertNotIsInstance(raised.exception, ValueError)

    def test_non_descendant_master_is_a_failure(self) -> None:
        for key, value in [
            ("status", "behind"),
            ("status", "diverged"),
            ("status", "identical"),
            ("behind_by", 1),
            ("behind_by", False),
            ("ahead_by", 0),
            ("ahead_by", True),
            ("ahead_by", "1"),
            ("base_commit", NEWER),
            ("merge_base_commit", "e882ea175f0ab8a3608db85c525eaa5c4484fa13"),
            ("head_commit", BASE),
            ("head_commit", None),
        ]:
            comparison = dict(DESCENDANT, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(gate.InvalidReport):
                gate.master_identity(MOVED_REF, BASE, lambda _newer, c=comparison: c)
        for key in DESCENDANT:
            partial = {k: v for k, v in DESCENDANT.items() if k != key}
            with self.subTest(missing=key), self.assertRaises(gate.InvalidReport):
                gate.master_identity(MOVED_REF, BASE, lambda _newer, c=partial: c)

    def test_moved_master_without_a_comparison_fails_closed(self) -> None:
        with self.assertRaises(gate.InvalidReport):
            gate.master_identity(MOVED_REF, BASE)


class GhStubCase(unittest.TestCase):
    """Plays `gh api` for the comparison read; holds no test of its own."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.calls: list[tuple[list[str], dict[str, Any]]] = []
        self.answer = json.dumps(DESCENDANT).encode()

    def fake_run(self, argv: list[str], **kwargs: Any) -> CommandResult:
        self.calls.append((list(argv), kwargs))
        return CommandResult(tuple(argv), 0, self.answer, b"")

    def patched(self) -> Any:
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.object(gate.shutil, "which", return_value="/bin/gh"))
        stack.enter_context(mock.patch.object(gate, "run_command", self.fake_run))
        return stack


class ComparisonReadTests(GhStubCase):
    """The comparison names both exact commits and is kept as evidence."""

    def test_reads_the_exact_commits_through_gh_and_keeps_the_answer(self) -> None:
        evidence = self.root / "master-compare.json"
        with self.patched():
            self.assertEqual(gate.fetch_comparison(REPO, BASE, NEWER, evidence), DESCENDANT)
        [(argv, kwargs)] = self.calls
        route = f"repos/{REPO}/compare/{BASE}...{NEWER}"
        self.assertEqual(
            argv, ["/bin/gh", "api", "--hostname", "github.com", route, "--jq", gate.COMPARE_FIELDS]
        )
        self.assertEqual(kwargs["allowed_executables"], ("/bin/gh",))
        self.assertTrue(kwargs["check"])
        self.assertLessEqual(kwargs["timeout_seconds"], 120)
        self.assertEqual(evidence.read_bytes(), self.answer)

    def test_refuses_bad_input_before_any_request(self) -> None:
        with self.patched():
            for repository, newer in [("VMAFx/vmafx/x", NEWER), ("VMAFx", NEWER), (REPO, "HEAD")]:
                with self.subTest(repository=repository, newer=newer):
                    with self.assertRaises(gate.InvalidReport):
                        gate.fetch_comparison(repository, BASE, newer, None)
        self.assertEqual(self.calls, [])
        with mock.patch.object(gate.shutil, "which", return_value=None):
            with self.assertRaises(gate.InvalidReport):
                gate.fetch_comparison(REPO, BASE, NEWER, None)

    def test_malformed_answer_fails(self) -> None:
        for answer in [b"[]", b"{", b'{"status": "ahead", "status": "ahead"}']:
            self.answer = answer
            with self.subTest(answer=answer), self.patched(), self.assertRaises(ValueError):
                gate.fetch_comparison(REPO, BASE, NEWER, None)


class MasterCliOutcomeTests(GhStubCase):
    """The master CLI: exit 0 verdict, 1 failure, SUPERSEDED_EXIT superseded."""

    def run_master(self, final_sha: str) -> tuple[int, str]:
        source = self.root / "report.json"
        source.write_text(
            json.dumps(dict(report(), repo={"name": f"github.com/{REPO}", "commit": BASE}))
        )
        reference = self.root / "ref.json"
        reference.write_text(
            json.dumps(dict(MOVED_REF, object={"sha": final_sha, "type": "commit"}))
        )
        argv = ["scorecard_gate.py", "master", "--sha", BASE, "--repository", REPO]
        argv += ["--report", str(source), "--master-ref", str(reference)]
        argv += ["--master-compare", str(self.root / "master-compare.json")]
        argv += [
            "--receipt",
            str(self.root / "gate.json"),
            "--summary",
            str(self.root / "summary.md"),
        ]
        output = io.StringIO()
        with self.patched(), mock.patch.object(sys, "argv", argv):
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
                code = gate.main()
        return code, output.getvalue()

    def test_descendant_exits_superseded_with_receipt_summary_and_notice(self) -> None:
        code, stdout = self.run_master(NEWER)
        self.assertEqual(code, gate.SUPERSEDED_EXIT)
        self.assertNotIn(gate.SUPERSEDED_EXIT, (0, 1, 2))
        receipt = json.loads((self.root / "gate.json").read_text())
        self.assertEqual(receipt["outcome"], "superseded")
        self.assertEqual((receipt["commit"], receipt["master"]["commit"]), (BASE, NEWER))
        self.assertEqual(receipt["ahead_by"], 1)
        self.assertNotIn("aggregate", receipt)
        summary = (self.root / "summary.md").read_text()
        self.assertIn("superseded", summary)
        self.assertIn("no verdict", summary)
        self.assertIn(NEWER, summary)
        self.assertNotIn("Result: PASS", summary)
        self.assertRegex(stdout, rf"(?m)^::notice title=Scorecard superseded::.*{NEWER}")
        self.assertEqual((self.root / "master-compare.json").read_bytes(), self.answer)

    def test_non_descendant_and_unreadable_comparisons_fail(self) -> None:
        self.answer = json.dumps(dict(DESCENDANT, status="diverged", behind_by=2)).encode()
        self.assertEqual(self.run_master(NEWER)[0], 1)
        self.assertFalse((self.root / "gate.json").exists())

        def refused(argv: list[str], **_kwargs: Any) -> CommandResult:
            raise CommandFailed(CommandResult(tuple(argv), 1, b"", b"HTTP 404"))

        self.fake_run = refused  # type: ignore[method-assign]
        self.assertEqual(self.run_master(NEWER)[0], 1)

    def test_unmoved_master_keeps_the_verdict_path(self) -> None:
        code, stdout = self.run_master(BASE)
        self.assertEqual(code, 0)
        self.assertEqual(self.calls, [])
        self.assertIn("Result: PASS", stdout)
        self.assertNotIn("superseded", stdout)


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
