#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The chart grants the operator every rule of its generated role (ADR-2350 D13).

config/rbac/role.yaml is what controller-gen derives from the operator's
+kubebuilder:rbac markers (scripts/codegen/crd_generate.py). Every (API group,
resource, verb) it lists must be granted to the operator's service account by
a Role or ClusterRole the chart binds to it. The chart grants events
cluster-wide (ADR-2647) and scopes leases to the release namespace with a Role
(ADR-1058), so a namespaced grant counts. Positive (the real chart), negative (a dropped
chart rule, a marker the chart lacks) and boundary (wildcards, operator off)
cases; the helm-chart workflow runs this file and needs ``helm`` on PATH.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

ROOT = Path(__file__).resolve().parents[3]
CHART = ROOT / "deploy" / "helm" / "vmafx"
ROLE = ROOT / "config" / "rbac" / "role.yaml"
OPERATOR = "operator: {enabled: true}\n"
HELM_TIMEOUT_S = 120

Grant = tuple[str, str, str]


def render(values: str) -> list[dict[str, Any]]:
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        fh.write(values)
        fh.flush()
        cmd = ["helm", "template", "vmafx", str(CHART), "--namespace", "vmafx", "-f", fh.name]
        result = subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
            cmd, capture_output=True, text=True, check=False, timeout=HELM_TIMEOUT_S
        )
    if result.returncode != 0:
        raise AssertionError(f"helm template failed: {result.stderr}")
    return [doc for doc in yaml.safe_load_all(result.stdout) if doc]


def expand(rules: list[dict[str, Any]]) -> set[Grant]:
    return {
        (group, resource, verb)
        for rule in rules
        for group in rule.get("apiGroups", [])
        for resource in rule.get("resources", [])
        for verb in rule.get("verbs", [])
    }


def generated_role(path: Path = ROLE) -> set[Grant]:
    docs = [d for d in yaml.safe_load_all(path.read_text(encoding="utf-8")) if d]
    return expand([rule for d in docs if d["kind"] == "ClusterRole" for rule in d["rules"]])


def operator_account(docs: list[dict[str, Any]]) -> str | None:
    for doc in docs:
        if doc["kind"] != "Deployment":
            continue
        if doc["metadata"].get("labels", {}).get("app.kubernetes.io/component") == "operator":
            name: str = doc["spec"]["template"]["spec"]["serviceAccountName"]
            return name
    return None


def granted(docs: list[dict[str, Any]], account: str | None) -> set[Grant]:
    """Every grant a Role or ClusterRole bound to `account` holds."""
    roles = {
        (d["kind"], d["metadata"]["name"]): d for d in docs if d["kind"] in ("Role", "ClusterRole")
    }
    out: set[Grant] = set()
    for binding in (d for d in docs if d["kind"] in ("RoleBinding", "ClusterRoleBinding")):
        subjects = {s["name"] for s in binding.get("subjects", []) if s["kind"] == "ServiceAccount"}
        role = roles.get((binding["roleRef"]["kind"], binding["roleRef"]["name"]))
        if account in subjects and role is not None:
            out |= expand(role.get("rules", []))
    return out


def covers(grants: set[Grant], want: Grant) -> bool:
    group, resource, verb = want
    return any(
        g in (group, "*") and r in (resource, "*") and v in (verb, "*") for g, r, v in grants
    )


def missing(docs: list[dict[str, Any]], want: set[Grant]) -> list[Grant]:
    grants = granted(docs, operator_account(docs))
    return sorted(w for w in want if not covers(grants, w))


@unittest.skipIf(shutil.which("helm") is None, "helm is not on PATH")
class OperatorRbacTest(unittest.TestCase):
    def test_chart_grants_the_generated_role(self) -> None:
        want = generated_role()
        self.assertIn(("coordination.k8s.io", "leases", "update"), want)
        self.assertIn(("vmafx.dev", "vmafxjobs/status", "patch"), want)
        self.assertEqual(missing(render(OPERATOR), want), [])

    def test_dropped_chart_rule_is_reported(self) -> None:
        docs = render(OPERATOR)
        for doc in docs:
            if doc["kind"] == "Role" and doc["metadata"]["name"].endswith("-operator-ns"):
                doc["rules"] = [r for r in doc["rules"] if "leases" not in r["resources"]]
        gaps = missing(docs, generated_role())
        self.assertIn(("coordination.k8s.io", "leases", "get"), gaps)

    def test_marker_the_chart_lacks_is_reported(self) -> None:
        want = generated_role() | {("vmafx.dev", "vmafxtenants", "list")}
        self.assertEqual(missing(render(OPERATOR), want), [("vmafx.dev", "vmafxtenants", "list")])

    def test_operator_off_grants_nothing(self) -> None:
        docs = render("operator: {enabled: false}\n")
        self.assertIsNone(operator_account(docs))
        self.assertEqual(len(missing(docs, generated_role())), len(generated_role()))


class CoverageRulesTest(unittest.TestCase):
    """The matching rules without helm."""

    def test_wildcards_cover(self) -> None:
        self.assertTrue(covers({("*", "*", "*")}, ("vmafx.dev", "vmafxjobs", "get")))
        self.assertTrue(covers({("vmafx.dev", "*", "get")}, ("vmafx.dev", "vmafxjobs", "get")))
        self.assertFalse(covers({("vmafx.dev", "*", "get")}, ("vmafx.dev", "vmafxjobs", "list")))
        self.assertFalse(covers(set(), ("", "events", "create")))

    def test_subresource_is_not_its_parent(self) -> None:
        grants = {("vmafx.dev", "vmafxjobs", "update")}
        self.assertFalse(covers(grants, ("vmafx.dev", "vmafxjobs/status", "update")))


if __name__ == "__main__":
    unittest.main()
