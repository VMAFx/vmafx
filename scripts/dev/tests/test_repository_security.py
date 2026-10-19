# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Adversarial readback controls for the repository's live security policy."""

from __future__ import annotations

import copy
import datetime as dt
import importlib.util
import json
import unittest
from pathlib import Path
from typing import Any, cast
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "repository_security", ROOT / "scripts/dev/check_repository_security.py"
)
assert SPEC is not None and SPEC.loader is not None
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class RepositorySecurityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.policy = json.loads((ROOT / ".github/repository-security-policy.json").read_text())
        self.rule = copy.deepcopy(self.policy["ruleset"])
        self.rule.update(id=8123, source_type="Repository", source="VMAFx/vmafx")
        self.responses = {
            "/repos/VMAFx/vmafx": {"full_name": "VMAFx/vmafx", "default_branch": "master"},
            "/repos/VMAFx/vmafx/rulesets?includes_parents=false&per_page=100": [
                {"id": 8123, "name": self.rule["name"]},
                {"id": 8199, "name": "unrelated release policy"},
            ],
            "/repos/VMAFx/vmafx/private-vulnerability-reporting": {"enabled": True},
            "/repos/VMAFx/vmafx/rulesets/8123": self.rule,
            "/repos/VMAFx/vmafx/rules/branches/master?per_page=100": [
                {**copy.deepcopy(rule), "ruleset_id": 8123} for rule in self.rule["rules"]
            ],
        }

    def inspect(self) -> list[str]:
        with patch.object(CHECK, "get_json", side_effect=self.responses.__getitem__):
            errors, _ = CHECK.inspect(self.policy)
        return cast(list[str], errors)

    def test_no_stricter_ruleset_template_is_declared(self) -> None:
        """Only the policy file declares the ruleset; praetor's stricter template is declined."""
        standards = (ROOT / ".standards.yaml").read_text()
        self.assertRegex(
            standards, r"(?m)^adoption:\n  decline:\n(?:    - .+\n)*    - branch-ruleset$"
        )
        self.assertFalse((ROOT / ".github/rulesets/main.json").exists())

    def test_declared_review_requirements_are_the_live_ones(self) -> None:
        """No approval, no last-push approval, no code-owner review, no signatures (ADR-3313)."""
        rules = self.policy["ruleset"]["rules"]
        review = next(r for r in rules if r["type"] == "pull_request")["parameters"]
        self.assertEqual(review["required_approving_review_count"], 0)
        self.assertIs(review["require_last_push_approval"], False)
        self.assertIs(review["require_code_owner_review"], False)
        self.assertNotIn("required_signatures", {r["type"] for r in rules})

    def test_a_declared_value_stricter_than_live_is_detected(self) -> None:
        self.rule["rules"] = copy.deepcopy(self.policy["ruleset"]["rules"])
        review = next(r for r in self.rule["rules"] if r["type"] == "pull_request")
        review["parameters"]["required_approving_review_count"] = 2
        review["parameters"]["require_code_owner_review"] = True
        errors = CHECK.differences(self.policy["ruleset"], self.rule)
        self.assertEqual(len(errors), 2)

    def test_matching_policy_allows_unrelated_ruleset_and_metadata(self) -> None:
        self.rule["rules"].reverse()
        self.assertEqual(self.inspect(), [])

    def test_no_managed_ruleset_fails(self) -> None:
        self.responses["/repos/VMAFx/vmafx/rulesets?includes_parents=false&per_page=100"] = []
        self.assertIn("exactly one", " ".join(self.inspect()))

    def test_duplicate_managed_rule_fails(self) -> None:
        key = "/repos/VMAFx/vmafx/rulesets?includes_parents=false&per_page=100"
        self.responses[key].append({"id": 8999, "name": self.rule["name"]})
        self.assertIn("exactly one", " ".join(self.inspect()))

    def test_declared_bypass_actor_is_accepted(self) -> None:
        # ADR-1252 declares exactly one User bypass actor. The live list must be
        # that actor and nothing else; the fixture inherits it from the policy.
        self.assertEqual(self.policy["ruleset"]["bypass_actors"], self.rule["bypass_actors"])
        self.assertEqual(len(self.rule["bypass_actors"]), 1)
        self.assertEqual(self.inspect(), [])

    def test_undeclared_bypass_actor_is_detected(self) -> None:
        self.rule["bypass_actors"] = [{"actor_type": "OrganizationAdmin", "bypass_mode": "always"}]
        self.assertIn("bypass_actors", " ".join(self.inspect()))

    def test_extra_bypass_actor_alongside_the_declared_one_is_detected(self) -> None:
        self.rule["bypass_actors"] = [
            *self.policy["ruleset"]["bypass_actors"],
            {"actor_id": 1, "actor_type": "OrganizationAdmin", "bypass_mode": "always"},
        ]
        self.assertIn("bypass_actors", " ".join(self.inspect()))

    def test_bypass_mode_widening_is_detected(self) -> None:
        widened = [dict(actor, bypass_mode="exempt") for actor in self.rule["bypass_actors"]]
        self.rule["bypass_actors"] = widened
        self.assertIn("bypass_mode", " ".join(self.inspect()))

    def test_hidden_bypass_actors_require_a_count_matching_the_policy(self) -> None:
        declared = len(self.policy["ruleset"]["bypass_actors"])
        del self.rule["bypass_actors"]
        with patch.object(CHECK, "bypass_count", return_value=declared) as query:
            self.assertEqual(self.inspect(), [])
            query.assert_called_once_with(8123)
        for wrong in (declared - 1, declared + 1):
            with patch.object(CHECK, "bypass_count", return_value=wrong):
                self.assertIn("bypass_actors", " ".join(self.inspect()))
        with patch.object(CHECK, "bypass_count", side_effect=ValueError("unavailable")):
            with self.assertRaisesRegex(ValueError, "unavailable"):
                self.inspect()

    def test_graphql_errors_and_truncation_never_count_as_zero(self) -> None:
        with (
            patch.object(CHECK.shutil, "which", return_value="/fixture/bin/gh"),
            patch.object(CHECK, "run_command") as run,
        ):
            run.return_value.stdout = json.dumps({"errors": [{"message": "denied"}]})
            with self.assertRaisesRegex(ValueError, "rejected"):
                CHECK.bypass_count(8123)
            run.return_value.stdout = json.dumps(
                {
                    "data": {
                        "repository": {"rulesets": {"pageInfo": {"hasNextPage": True}, "nodes": []}}
                    }
                }
            )
            with self.assertRaisesRegex(ValueError, "truncated"):
                CHECK.bypass_count(8123)

    def test_disabled_reporting_is_detected(self) -> None:
        self.responses["/repos/VMAFx/vmafx/private-vulnerability-reporting"] = {"enabled": False}
        self.assertIn("private_vulnerability_reporting", " ".join(self.inspect()))

    def test_boolean_as_integer_is_not_accepted(self) -> None:
        self.responses["/repos/VMAFx/vmafx/private-vulnerability-reporting"] = {"enabled": 1}
        self.assertIn("private_vulnerability_reporting", " ".join(self.inspect()))

    def test_review_freshness_or_check_origin_changes_are_detected(self) -> None:
        for rule_type, key, value in [
            ("pull_request", "required_approving_review_count", 1),
            ("pull_request", "require_last_push_approval", True),
            ("pull_request", "dismiss_stale_reviews_on_push", False),
            ("required_status_checks", "strict_required_status_checks_policy", False),
            (
                "required_status_checks",
                "required_status_checks",
                [{"context": "Required Checks Aggregator"}],
            ),
        ]:
            with self.subTest(rule_type=rule_type, key=key):
                self.setUp()
                rule = next(item for item in self.rule["rules"] if item["type"] == rule_type)
                rule["parameters"][key] = value
                self.assertIn(key, " ".join(self.inspect()))

    def test_inactive_or_mis_scoped_ruleset_is_detected(self) -> None:
        self.rule["enforcement"] = "disabled"
        self.rule["conditions"]["ref_name"]["include"] = ["refs/heads/another-branch"]
        errors = " ".join(self.inspect())
        self.assertIn("enforcement", errors)
        self.assertIn("conditions", errors)

    def test_missing_effective_rule_is_detected(self) -> None:
        self.responses["/repos/VMAFx/vmafx/rules/branches/master?per_page=100"].pop()
        self.assertIn("effective_rules", " ".join(self.inspect()))

    def test_truncation_and_malformed_ids_fail_closed(self) -> None:
        key = "/repos/VMAFx/vmafx/rulesets?includes_parents=false&per_page=100"
        self.responses[key] *= 50
        with self.assertRaisesRegex(ValueError, "truncated"):
            self.inspect()
        self.setUp()
        self.responses[key][0]["id"] = "https://unexpected.example"
        with self.assertRaisesRegex(ValueError, "ID"):
            self.inspect()

    def test_wrong_target_is_rejected_before_network_access(self) -> None:
        self.policy["repository"] = "Netflix/vmaf"
        with patch.object(CHECK, "get_json") as fetch:
            with self.assertRaisesRegex(ValueError, "VMAFx/vmafx master"):
                CHECK.inspect(self.policy)
            fetch.assert_not_called()


class BypassModeExceptionTests(unittest.TestCase):
    """A bypass mode other than pull-request mode holds only under a dated exception."""

    PATH = ".github/repository-security-policy.json"
    DAY = dt.date(2026, 10, 12)

    def setUp(self) -> None:
        self.policy = json.loads((ROOT / self.PATH).read_text())
        self.listed = CHECK.lint_exceptions.Entry

    def entry(self, expires: dt.date, rule: str = "", path: str = "") -> Any:
        return self.listed(rule or CHECK.BYPASS_MODE_RULE, path or self.PATH, "reason", expires)

    def findings(self, entries: list[Any], day: dt.date | None = None) -> list[str]:
        return cast(list[str], CHECK.bypass_mode_findings(self.policy, entries, day or self.DAY))

    def set_mode(self, mode: str) -> None:
        for actor in self.policy["ruleset"]["bypass_actors"]:
            actor["bypass_mode"] = mode

    def tracked(self) -> list[Any]:
        entries, malformed = CHECK.lint_exceptions.load(ROOT)
        self.assertEqual(malformed, [])
        return [entry for entry in entries if entry.rule == CHECK.BYPASS_MODE_RULE]

    def test_the_tracked_policy_is_pull_request_mode_without_an_entry(self) -> None:
        """ADR-3313: the bypass is at pull-request mode and the list holds no entry."""
        modes = {actor["bypass_mode"] for actor in self.policy["ruleset"]["bypass_actors"]}
        self.assertEqual(modes, {"pull_request"})
        own = self.tracked()
        self.assertEqual(own, [])
        self.assertEqual(self.findings(own, CHECK.lint_exceptions.today()), [])

    def test_a_mode_other_than_pull_request_without_an_entry_is_a_finding(self) -> None:
        self.set_mode("always")
        found = " ".join(self.findings([]))
        self.assertIn("bypass_mode always", found)
        self.assertIn("solo-repository-bypass-mode.toml", found)

    def test_an_expired_entry_does_not_hold(self) -> None:
        self.set_mode("always")
        yesterday = self.DAY - dt.timedelta(1)
        self.assertIn("unexpired", " ".join(self.findings([self.entry(yesterday)])))
        self.assertEqual(self.findings([self.entry(self.DAY)]), [])

    def test_an_entry_of_another_rule_or_file_does_not_hold(self) -> None:
        self.set_mode("always")
        later = self.DAY + dt.timedelta(30)
        for other in (
            self.entry(later, rule="scorecard-branch-protection"),
            self.entry(later, path=".github/CODEOWNERS"),
        ):
            with self.subTest(other=other):
                self.assertIn("unexpired", " ".join(self.findings([other])))

    def test_pull_request_mode_needs_no_entry_and_refuses_a_stale_one(self) -> None:
        self.set_mode("pull_request")
        self.assertEqual(self.findings([]), [])
        stale = self.findings([self.entry(self.DAY + dt.timedelta(30))])
        self.assertIn("stale entry", " ".join(stale))

    def test_the_checker_fails_on_a_stale_entry(self) -> None:
        stale = self.entry(self.DAY + dt.timedelta(days=30))
        with (
            patch.object(CHECK, "inspect", return_value=([], {})),
            patch.object(CHECK.sys, "argv", ["check_repository_security.py"]),
        ):
            self.assertEqual(CHECK.main(), 0)
            with patch.object(CHECK.lint_exceptions, "load", return_value=([stale], [])):
                self.assertEqual(CHECK.main(), 1)


if __name__ == "__main__":
    unittest.main()
