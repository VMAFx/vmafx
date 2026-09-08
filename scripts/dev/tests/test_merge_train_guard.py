# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Train control regressions: real Git/Make, fixture-only GitHub (ADR-1244)."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import merge_train_guard as guard


class FixtureTrain(guard.Train):
    """Only the local fixture remote substitutes for GitHub."""

    def __init__(self, root: Path, state: Path) -> None:
        super().__init__(root, state)
        self.metadata: dict[str, Any] = {}
        self.calls: list[tuple[str, ...]] = []
        self.checks = [{"name": "Required Checks Aggregator", "bucket": "pass", "state": "SUCCESS"}]
        self.fail_push = False
        self.fail_network = False

    def pr(self, number: int) -> dict[str, Any]:
        if self.fail_network:
            raise guard.Refused("fixture API unavailable")
        return {**self.metadata, "number": number}

    def gh(self, *args: str) -> str:
        self.calls.append(args)
        if args[:2] == ("pr", "checks"):
            return json.dumps(self.checks)
        if args[:2] == ("pr", "list"):
            return json.dumps([self.metadata])
        return ""

    def git(self, *args: str, cwd: Path | None = None) -> str:
        if args[:2] == ("remote", "get-url"):
            return "https://github.com/VMAFx/vmafx.git"
        if args[0] == "push" and self.fail_push:
            raise guard.Refused("fixture push rejected")
        result: str = super().git(*args, cwd=cwd)
        if args[0] == "push" and self.metadata:
            self.metadata["headRefOid"] = self.git("rev-parse", "HEAD", cwd=cwd)
        return result


class TrainContract(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / "repo"
        self.root.mkdir()
        self.state = self.directory / "state"
        self.state.mkdir()
        (self.state / "policy.json").write_text(
            json.dumps({"schema": 1, "protected_branches": [], "protected_worktrees": []})
        )
        (self.state / "hold.txt").write_text("")
        self.train = FixtureTrain(self.root, self.state)
        self.train.git("init", "--initial-branch=master")
        self.train.git("config", "user.name", "Train fixture")
        self.train.git("config", "user.email", "fixture@example.invalid")
        self.train.git("config", "core.hooksPath", "/dev/null")
        self.train.git("config", "commit.gpgsign", "false")
        (self.root / "Makefile").write_text(".PHONY: lint test\nlint test:\n\t@echo executed-$@\n")
        (self.root / ".gitignore").write_text("GNUmakefile\ncache/\n")
        self.train.git("add", ".")
        self.train.git("commit", "-m", "fixture base")
        self.train.git("checkout", "-b", "topic")
        (self.root / "topic.txt").write_text("topic\n")
        self.train.git("add", ".")
        self.train.git("commit", "-m", "fixture topic")
        self.head = self.train.git("rev-parse", "HEAD")
        self.train.git("checkout", "master")
        (self.root / "new-base.txt").write_text("new base\n")
        self.train.git("add", ".")
        self.train.git("commit", "-m", "fixture master moves")
        remote = self.directory / "remote.git"
        self.train.git("clone", "--bare", str(self.root), str(remote))
        self.train.git("remote", "add", "origin", str(remote))
        self.train.metadata = {
            "number": 1421,
            "state": "OPEN",
            "isDraft": True,
            "baseRefName": "master",
            "headRefName": "topic",
            "headRefOid": self.head,
            "isCrossRepository": False,
            "autoMergeRequest": None,
            "mergeStateStatus": "CLEAN",
        }

    def detached(self, name: str = "validation") -> Path:
        checkout = self.directory / name
        self.train.git("worktree", "add", "--detach", str(checkout), self.head)
        return checkout

    def test_holds_and_nonmaster_apply_to_every_action(self) -> None:
        for condition in ("held", "stacked", "release", "paused", "protected"):
            for action in ("promote", "rebase", "arm", "merge"):
                with self.subTest(condition=condition, action=action):
                    (self.state / "hold.txt").write_text(
                        "1421 human hold\n" if condition == "held" else ""
                    )
                    self.train.metadata["baseRefName"] = (
                        "topic-parent" if condition == "stacked" else "master"
                    )
                    (self.state / "policy.json").write_text(
                        json.dumps(
                            {
                                "schema": 1,
                                "protected_branches": ["topic"] if condition == "protected" else [],
                                "protected_worktrees": [],
                            }
                        )
                    )
                    (self.state / "PAUSED").unlink(missing_ok=True)
                    if condition == "paused":
                        (self.state / "PAUSED").touch()
                    number = 1213 if condition == "release" else 1421
                    with self.assertRaises(guard.Refused):
                        if action in ("arm", "merge"):
                            self.train.merge(number, auto=action == "arm")
                        else:
                            getattr(self.train, action)(number)
                    self.assertFalse(self.train.calls)

    def test_active_branch_and_detached_agent_head_are_refused(self) -> None:
        owned = self.directory / "human-checkout"
        self.train.git("worktree", "add", str(owned), "topic")
        with self.assertRaisesRegex(guard.Refused, "another owner"):
            self.train.rebase(1421)
        self.train.git("worktree", "remove", str(owned))
        self.detached("agent-active")
        with self.assertRaisesRegex(guard.Refused, "another owner"):
            self.train.guard(1421)

    def test_missing_policy_hold_and_network_fail_closed(self) -> None:
        for filename in ("policy.json", "hold.txt"):
            path = self.state / filename
            original = path.read_bytes()
            path.unlink()
            with self.assertRaises(OSError):
                self.train.guard(1421)
            path.write_bytes(original)
        self.train.fail_network = True
        with self.assertRaises(guard.Refused):
            self.train.promote(1421)
        self.assertFalse(self.train.calls)

    def test_rejected_push_never_promotes_and_retains_checkout(self) -> None:
        self.train.fail_push = True
        with self.assertRaisesRegex(guard.Refused, "push rejected"):
            self.train.promote(1421)
        self.assertFalse(self.train.calls)
        self.assertEqual(len(list((self.state / "worktrees").glob("*/checkout"))), 1)
        with self.assertRaisesRegex(guard.Refused, "retained train checkout"):
            self.train.promote(1421)
        self.assertEqual(len(list((self.state / "worktrees").glob("*/checkout"))), 1)

    def test_conflict_never_promotes_and_retains_checkout(self) -> None:
        (self.root / "topic.txt").write_text("conflicting master\n")
        self.train.git("add", ".")
        self.train.git("commit", "-m", "fixture conflict")
        self.train.git("push", "origin", "master")
        self.train.metadata["headRefOid"] = self.head
        with self.assertRaises(guard.Refused):
            self.train.promote(1421)
        self.assertFalse(self.train.calls)
        self.assertEqual(len(list((self.state / "worktrees").glob("*/checkout"))), 1)

    def test_success_rebases_then_promotes_without_arming_or_foreign_cleanup(self) -> None:
        unknown = self.state / "worktrees" / "unknown"
        unknown.mkdir(parents=True)
        (unknown / "keep.txt").write_text("unknown evidence\n")
        shared_fetch_head = self.root / ".git" / "FETCH_HEAD"
        shared_fetch_head.write_text("unrelated actor's fetch metadata\n")
        self.train.promote(1421)
        self.assertNotEqual(self.train.metadata["headRefOid"], self.head)
        self.assertEqual(self.train.calls, [("pr", "ready", "1421")])
        self.assertTrue((unknown / "keep.txt").exists())
        self.assertFalse(list((self.state / "worktrees").glob("*/checkout")))
        self.assertEqual(shared_fetch_head.read_text(), "unrelated actor's fetch metadata\n")

    def test_full_make_commands_generate_receipt_and_scrub_make_injection(self) -> None:
        checkout = self.detached()
        alternate = self.directory / "override.mk"
        alternate.write_text("lint test:\n\t@false\n")
        with patch.dict(
            os.environ,
            {"MAKEFLAGS": "n", "MAKEFILES": str(alternate), "MAKEOVERRIDES": "lint=skip"},
        ):
            self.train.validate(1421, checkout)
        self.train.require_validation(self.head)
        receipt = json.loads((self.state / f"validated-{self.head}.json").read_text())
        for result in receipt["payload"]["results"]:
            self.assertIn(f"executed-{result['command'][1]}", Path(result["log"]).read_text())

    def test_failed_full_gate_and_changed_source_never_issue_receipt(self) -> None:
        checkout = self.detached()
        (checkout / "Makefile").write_text("lint:\n\t@false\ntest:\n\t@echo should-not-run\n")
        self.train.git("add", ".", cwd=checkout)
        self.train.git("commit", "-m", "fixture failed lint", cwd=checkout)
        self.train.metadata["headRefOid"] = self.train.git("rev-parse", "HEAD", cwd=checkout)
        with self.assertRaisesRegex(guard.Refused, "make lint failed"):
            self.train.validate(1421, checkout)
        self.assertFalse(list(self.state.glob("validated-*.json")))
        (checkout / "dirty.txt").write_text("unknown\n")
        with self.assertRaisesRegex(guard.Refused, "changes"):
            self.train.validate(1421, checkout)

    def test_ignored_makefile_override_cannot_fake_clean_gate(self) -> None:
        checkout = self.detached()
        (checkout / "GNUmakefile").write_text("lint test:\n\t@true\n")
        with self.assertRaises(guard.Refused):
            self.train.validate(1421, checkout)
        self.assertFalse(list(self.state.glob("validated-*.json")))

    def test_receipt_and_log_tampering_are_refused(self) -> None:
        self.train.validate(1421, self.detached())
        receipt_path = self.state / f"validated-{self.head}.json"
        original = receipt_path.read_bytes()
        receipt = json.loads(original)
        receipt["payload"]["completed_at"] = "handwritten"
        receipt_path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(guard.Refused, "not issued"):
            self.train.require_validation(self.head)
        receipt_path.write_bytes(original)
        Path(receipt["payload"]["results"][0]["log"]).write_text("replaced evidence")
        with self.assertRaisesRegex(guard.Refused, "changed"):
            self.train.require_validation(self.head)

    def test_absent_pending_failed_or_skipped_required_checks_never_arm(self) -> None:
        self.train.metadata["isDraft"] = False
        with patch.object(self.train, "require_validation"):
            for checks in (
                [],
                [{"name": "other", "bucket": "pass"}],
                *[
                    [{"name": "Required Checks Aggregator", "bucket": bucket}]
                    for bucket in ("pending", "fail", "skipping", "cancel")
                ],
            ):
                self.train.checks = checks
                with self.subTest(checks=checks), self.assertRaises(guard.Refused):
                    self.train.merge(1421, auto=True)
                self.assertFalse(any(call[:2] == ("pr", "merge") for call in self.train.calls))

    def test_clean_merge_is_bound_to_validated_head_and_keeps_branch(self) -> None:
        self.train.validate(1421, self.detached())
        self.train.metadata["isDraft"] = False
        self.train.merge(1421, auto=False)
        self.assertEqual(
            self.train.calls[-1],
            ("pr", "merge", "1421", "--squash", "--match-head-commit", self.head),
        )

    def test_head_change_or_new_hold_during_checks_prevents_merge(self) -> None:
        self.train.metadata["isDraft"] = False
        for change in ("head", "hold"):
            with self.subTest(change=change):
                (self.state / "hold.txt").write_text("")
                self.train.metadata["headRefOid"] = self.head

                def race(number: int, change: str = change) -> None:
                    if change == "head":
                        self.train.metadata["headRefOid"] = "f" * 40
                    else:
                        (self.state / "hold.txt").write_text("1421 newly held\n")

                with (
                    patch.object(self.train, "require_validation"),
                    patch.object(self.train, "require_checks", side_effect=race),
                    self.assertRaises(guard.Refused),
                ):
                    self.train.merge(1421, auto=False)
                self.assertFalse(any(call[:2] == ("pr", "merge") for call in self.train.calls))

    def test_failed_repeat_test_revokes_old_receipt_but_preserves_it(self) -> None:
        checkout = self.detached()
        (checkout / "Makefile").write_text(
            'lint:\n\t@echo lint-ran\ntest:\n\t@test -z "$$FAIL_GATE"\n'
        )
        self.train.git("add", ".", cwd=checkout)
        self.train.git("commit", "-m", "fixture test environment failure", cwd=checkout)
        head = self.train.git("rev-parse", "HEAD", cwd=checkout)
        self.train.metadata["headRefOid"] = head
        self.train.validate(1421, checkout)
        self.train.require_validation(head)
        with (
            patch.dict(os.environ, {"FAIL_GATE": "yes"}),
            self.assertRaisesRegex(guard.Refused, "make test failed"),
        ):
            self.train.validate(1421, checkout)
        self.assertFalse((self.state / f"validated-{head}.json").exists())
        self.assertEqual(len(list(self.state.glob("validation-*/previous-receipt.json"))), 1)

    def test_noncanonical_or_multiple_origin_urls_refused_before_fetch(self) -> None:
        original = self.train.git
        for urls in (
            "https://example.invalid/fork.git",
            "https://github.com/VMAFx/vmafx.git\nhttps://example.invalid/fork.git",
        ):

            def git(*args: str, cwd: Path | None = None, urls: str = urls) -> str:
                if args[:2] == ("remote", "get-url"):
                    return urls
                self.assertNotEqual(args[0], "fetch")
                return original(*args, cwd=cwd)

            with (
                self.subTest(urls=urls),
                patch.object(self.train, "git", side_effect=git),
                self.assertRaisesRegex(guard.Refused, "canonical"),
            ):
                self.train.rebase(1421)

    def test_actual_nonzero_exit_including_gh_pending_never_succeeds(self) -> None:
        with self.assertRaisesRegex(guard.Refused, "failed \\(8\\)"):
            guard.execute([sys.executable, "-c", "raise SystemExit(8)"], self.root)

    def test_concurrent_actor_is_refused(self) -> None:
        with (
            self.train.mutation_lock(),
            self.assertRaises(guard.Refused),
            self.train.mutation_lock(),
        ):
            self.fail("lock should be exclusive")

    def test_cycle_is_read_only_and_refuses_previously_armed_ineligible_pr(self) -> None:
        self.train.cycle(False, 3)
        self.assertEqual(
            self.train.calls,
            [("pr", "list", "--state", "open", "--limit", "100", "--json", guard.PR_FIELDS)],
        )
        self.train.metadata["baseRefName"] = "stacked"
        self.train.metadata["autoMergeRequest"] = {"enabledAt": "fixture"}
        with self.assertRaisesRegex(guard.Refused, "owner must disarm"):
            self.train.cycle(True, 3)

    def test_mutating_cli_requires_explicit_apply(self) -> None:
        with patch.object(guard, "Train", return_value=self.train):
            for action in ("promote", "rebase", "arm", "merge", "validate"):
                self.assertEqual(guard.main([action, "1421", "--state-dir", str(self.state)]), 1)
        self.assertFalse(self.train.calls)


if __name__ == "__main__":
    unittest.main()
