"""Adversarial readback controls for the repository's live security policy."""

from __future__ import annotations

import copy
import importlib.util
import json
import unittest
from pathlib import Path
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
        return errors

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

    def test_admin_bypass_is_detected(self) -> None:
        self.rule["bypass_actors"] = [{"actor_type": "OrganizationAdmin", "bypass_mode": "always"}]
        self.assertIn("bypass_actors", " ".join(self.inspect()))

    def test_hidden_bypass_actors_require_a_verified_zero_count(self) -> None:
        del self.rule["bypass_actors"]
        with patch.object(CHECK, "bypass_count", return_value=0) as query:
            self.assertEqual(self.inspect(), [])
            query.assert_called_once_with(8123)
        with patch.object(CHECK, "bypass_count", return_value=1):
            self.assertIn("bypass_actors", " ".join(self.inspect()))
        with patch.object(CHECK, "bypass_count", side_effect=ValueError("unavailable")):
            with self.assertRaisesRegex(ValueError, "unavailable"):
                self.inspect()

    def test_graphql_errors_and_truncation_never_count_as_zero(self) -> None:
        with (
            patch.object(CHECK.shutil, "which", return_value="/fixture/bin/gh"),
            patch.object(CHECK.subprocess, "run") as run,
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
            ("pull_request", "required_approving_review_count", 0),
            ("pull_request", "require_last_push_approval", False),
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


if __name__ == "__main__":
    unittest.main()
