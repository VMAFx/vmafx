#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Reviewed Renovate rules carry hold unless a later rule can automerge them.

The catch-all minor/patch rule is the one exception: its mergeable addLabels
would also hold updates that later rules explicitly automerge.
"""

from __future__ import annotations

import copy
import json
import re
import sys
import unittest
from pathlib import Path
from typing import Any, ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.renovate_regex import to_python

ROOT = Path(__file__).resolve().parents[3]
HOLD = "hold"
DIMENSIONS = (
    "matchManagers",
    "matchUpdateTypes",
    "matchPackageNames",
    "matchDepNames",
    "matchDatasources",
    "matchFileNames",
    "matchDepTypes",
    "matchCurrentValue",
)
Rule = dict[str, Any]


def values(value: str | list[str]) -> list[str]:
    """Normalize the scalar matchCurrentValue form to a value list."""
    return [value] if isinstance(value, str) else value


def regex(value: str) -> re.Pattern[str] | None:
    """Compile Renovate's slash-delimited regex, including its optional i flag."""
    if not value.startswith("/"):
        return None
    if value.endswith("/i"):
        return re.compile(to_python(value[1:-2]), re.IGNORECASE)
    if value.endswith("/"):
        return re.compile(to_python(value[1:-1]))
    return None


def concrete(value: str) -> bool:
    """Only exact positive strings can prove coverage by a later rule."""
    return not value.startswith("!") and regex(value) is None and "*" not in value


def matches(pattern: str, value: str) -> bool:
    """Prove a concrete value belongs to an exact string or a regex."""
    if pattern.startswith("!"):
        return False
    expression = regex(pattern)
    if expression is not None:
        return expression.search(value) is not None
    return "*" not in pattern and pattern == value


def pair_overlap(left: str, right: str) -> bool:
    """Unknown pattern intersections conservatively count as overlapping."""
    if concrete(left) and concrete(right):
        return left == right
    if concrete(right) and regex(left) is not None:
        return matches(left, right)
    if concrete(left) and regex(right) is not None:
        return matches(right, left)
    return True


def may_overlap(a: list[str], b: list[str]) -> bool:
    """At least one pair can match within this dimension."""
    return any(pair_overlap(left, right) for left in a for right in b)


def co_match(r1: Rule, r2: Rule) -> bool:
    """Rules can co-match unless a shared dimension proves them disjoint."""
    return all(
        may_overlap(values(r1[key]), values(r2[key]))
        for key in DIMENSIONS
        if key in r1 and key in r2
    )


def covers(k: Rule, r: Rule) -> bool:
    """Prove k is a superset of r across all dimensions constrained by k."""
    for key in DIMENSIONS:
        if key not in k:
            continue
        if key not in r:
            return False
        for value in values(r[key]):
            if not concrete(value) or not any(matches(p, value) for p in values(k[key])):
                return False
    return True


def settled(rules: list[Rule], j: int, reviewed: Rule) -> bool:
    """A still later enabled review rule covers the whole reviewed set."""
    return any(
        rule.get("enabled") is not False
        and rule.get("automerge") is False
        and covers(rule, reviewed)
        for rule in rules[j + 1 :]
    )


def leaks(rules: list[Rule], i: int) -> bool:
    """A later automerge rule can win without a covering review rule after it."""
    return any(
        rule.get("automerge") is True
        and rule.get("enabled") is not False
        and co_match(rules[i], rule)
        and not settled(rules, j, rules[i])
        for j, rule in enumerate(rules[i + 1 :], start=i + 1)
    )


def name(rule: Rule, index: int) -> str:
    """Use the human-readable group, description, or position in diagnostics."""
    return str(rule.get("groupName", rule.get("description", index)))


def findings(config: dict[str, Any]) -> list[str]:
    """Reject missing review holds, holds outside review rules, and leaking holds."""
    errors = []
    rules = config["packageRules"]
    for i, rule in enumerate(rules):
        if rule.get("enabled") is False:
            continue
        held = HOLD in rule.get("addLabels", [])
        reviewed = rule.get("automerge") is False
        leaking = leaks(rules, i)
        label = name(rule, i)
        if reviewed and not leaking and not held:
            errors.append(f"reviewed rule {label} lacks addLabels hold")
        if held and not reviewed:
            errors.append(f"hold outside a reviewed rule: {label}")
        if held and leaking:
            errors.append(f"hold on {label} reaches an automerged update")
    return errors


def leak_exempt(config: dict[str, Any]) -> list[str]:
    """Name enabled reviewed rules where adding hold would reach automerge."""
    rules = config["packageRules"]
    return [
        name(rule, i)
        for i, rule in enumerate(rules)
        if rule.get("enabled") is not False and rule.get("automerge") is False and leaks(rules, i)
    ]


class ReviewedRulesCarryHold(unittest.TestCase):
    config: ClassVar[dict[str, Any]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.config = json.loads((ROOT / "renovate.json").read_text(encoding="utf-8"))

    def test_checked_in_rules(self) -> None:
        self.assertEqual(findings(self.config), [])

    def test_only_catch_all_is_exempt(self) -> None:
        self.assertEqual(leak_exempt(self.config), ["Other dependencies (minor + patch)"])

    def test_ci_node_carries_hold(self) -> None:
        rule = next(r for r in self.config["packageRules"] if r.get("groupName") == "CI Node.js")
        self.assertIn(HOLD, rule.get("addLabels", []))

    def test_missing_ci_node_hold_fails(self) -> None:
        config = copy.deepcopy(self.config)
        rule = next(r for r in config["packageRules"] if r.get("groupName") == "CI Node.js")
        rule.pop("addLabels", None)
        self.assertIn("reviewed rule CI Node.js lacks addLabels hold", findings(config))

    def test_catch_all_hold_leaks(self) -> None:
        config = copy.deepcopy(self.config)
        config["packageRules"][0]["addLabels"] = [HOLD]
        self.assertIn(
            "hold on Other dependencies (minor + patch) reaches an automerged update",
            findings(config),
        )

    def test_separate_label_only_rule_fails(self) -> None:
        config = copy.deepcopy(self.config)
        config["packageRules"].append({"matchPackageNames": ["numpy"], "addLabels": [HOLD]})
        index = len(config["packageRules"]) - 1
        self.assertIn(f"hold outside a reviewed rule: {index}", findings(config))

    def test_automerge_rule_with_hold_fails(self) -> None:
        config = copy.deepcopy(self.config)
        rule = next(
            r
            for r in config["packageRules"]
            if r.get("groupName") == "GitHub Actions (minor + patch)"
        )
        rule["addLabels"] = [HOLD]
        self.assertIn(
            "hold outside a reviewed rule: GitHub Actions (minor + patch)", findings(config)
        )

    def test_review_after_automerge_wins(self) -> None:
        rules = [
            {"matchPackageNames": ["foo"], "automerge": True},
            {"matchPackageNames": ["foo"], "automerge": False, "addLabels": [HOLD]},
        ]
        self.assertFalse(leaks(rules, 1))
        self.assertEqual(findings({"packageRules": rules}), [])

    def test_later_reviewed_superset_settles_overlap(self) -> None:
        rules = [
            {"matchPackageNames": ["foo"], "automerge": False, "addLabels": [HOLD]},
            {"matchManagers": ["custom.regex"], "automerge": True},
            {"matchPackageNames": ["foo", "bar"], "automerge": False, "addLabels": [HOLD]},
        ]
        self.assertFalse(leaks(rules, 0))
        self.assertEqual(findings({"packageRules": rules}), [])

    def test_disjoint_update_types(self) -> None:
        self.assertFalse(co_match({"matchUpdateTypes": ["major"]}, {"matchUpdateTypes": ["minor"]}))

    def test_regex_against_concrete(self) -> None:
        for pattern, value, expected in (
            ("/^foo/", "foobar", True),
            ("/^foo/", "barfoo", False),
            ("/^foo/i", "FOObar", True),
            ("/(?<prefix>foo)/", "foobar", True),
        ):
            with self.subTest(pattern=pattern, value=value):
                left = {"matchPackageNames": [pattern]}
                right = {"matchPackageNames": [value]}
                self.assertEqual(co_match(left, right), expected)
                self.assertEqual(co_match(right, left), expected)

    def test_current_value_accepts_a_string(self) -> None:
        rule = {"matchCurrentValue": "24.04"}
        self.assertTrue(co_match(rule, {"matchCurrentValue": ["24.04"]}))
        self.assertFalse(co_match(rule, {"matchCurrentValue": "26.04"}))
        self.assertTrue(covers({"matchCurrentValue": "/^24/"}, rule))

    def test_unknown_pattern_overlap_is_conservative(self) -> None:
        for left, right in (("foo*", "bar"), ("!foo", "foo"), ("/^foo/", "/^bar/")):
            with self.subTest(left=left, right=right):
                self.assertTrue(may_overlap([left], [right]))
                self.assertTrue(may_overlap([right], [left]))

    def test_coverage_requires_proof_in_every_dimension(self) -> None:
        reviewed = {"matchPackageNames": ["foo", "bar"]}
        self.assertFalse(covers({"matchPackageNames": ["foo"]}, reviewed))
        self.assertFalse(covers({"matchManagers": ["npm"]}, reviewed))
        self.assertFalse(covers({"matchPackageNames": ["*"]}, reviewed))
        self.assertFalse(covers({"matchPackageNames": ["!baz"]}, reviewed))
        self.assertTrue(covers({}, {"matchPackageNames": ["/^foo/"]}))
        self.assertFalse(covers(reviewed, {"matchPackageNames": ["/^foo/"]}))
        self.assertTrue(covers({"matchPackageNames": ["/^(foo|bar)$/"]}, reviewed))

    def test_disabled_rules_do_not_affect_findings_or_settle_leaks(self) -> None:
        rules = [
            {"matchPackageNames": ["foo"], "automerge": False, "addLabels": [HOLD]},
            {"matchPackageNames": ["foo"], "automerge": True, "enabled": False},
        ]
        self.assertEqual(findings({"packageRules": rules}), [])
        rules[1].pop("enabled")
        rules.append({"matchPackageNames": ["foo"], "automerge": False, "enabled": False})
        self.assertTrue(leaks(rules, 0))
        self.assertEqual(
            findings({"packageRules": rules}), ["hold on 0 reaches an automerged update"]
        )


if __name__ == "__main__":
    unittest.main()
