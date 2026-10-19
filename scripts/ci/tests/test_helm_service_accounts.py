#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Service-account RBAC of the chart: tenants (ADR-1592) and operator events.

Every Role and ClusterRole the chart renders is resolved through its bindings
to the service accounts it grants; the accounts that reach ``vmafxtenants``
must be exactly the controller's own, which only the controller pods use.
Positive (tenant registry on), negative (no other workload's account, the
operator's included) and boundary (no registry: nobody) renders.

The operator reconciles its resources in every namespace and records events
on them, which the API server stores in the object's namespace. Its account
may therefore create and patch events in any namespace, and do nothing else
with events; pods and leader-election leases stay in the release namespace
(ADR-2647). The helm-chart workflow runs this file and needs ``helm`` on PATH.
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

CHART = Path(__file__).resolve().parents[3] / "deploy" / "helm" / "vmafx"
OPERATOR = "operator: {enabled: true}\n"
REGISTRY = """
controller: {enabled: true}
node: {enabled: true}
operator: {enabled: true}
networkPolicy: {enabled: true}
auth:
  enabled: true
  issuer: https://idp.example.com/
  jwksEndpoint: https://idp.example.com/keys
  tenants:
    - tenantId: acme
"""


def render(values: str) -> list[dict[str, Any]]:
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        fh.write(values)
        fh.flush()
        cmd = ["helm", "template", "vmafx", str(CHART), "--namespace", "vmafx", "-f", fh.name]
        result = subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
            cmd, capture_output=True, text=True, check=False
        )
    if result.returncode != 0:
        raise AssertionError(f"helm template failed: {result.stderr}")
    return [doc for doc in yaml.safe_load_all(result.stdout) if doc]


def grants_tenants(role: dict[str, Any]) -> bool:
    return any("vmafxtenants" in rule.get("resources", []) for rule in role.get("rules", []))


def tenant_readers(docs: list[dict[str, Any]]) -> set[str]:
    """Service accounts any binding grants a rule on vmafxtenants."""
    roles = {
        (d["kind"], d["metadata"]["name"]): d for d in docs if d["kind"] in ("Role", "ClusterRole")
    }
    readers: set[str] = set()
    for binding in (d for d in docs if d["kind"] in ("RoleBinding", "ClusterRoleBinding")):
        ref = binding["roleRef"]
        role = roles.get((ref["kind"], ref["name"]))
        if role is None or not grants_tenants(role):
            continue
        readers |= {s["name"] for s in binding.get("subjects", []) if s["kind"] == "ServiceAccount"}
    return readers


def rule_allows(rule: dict[str, Any], group: str, resource: str, verb: str) -> bool:
    def has(key: str, value: str) -> bool:
        return value in rule.get(key, []) or "*" in rule.get(key, [])

    return has("apiGroups", group) and has("resources", resource) and has("verbs", verb)


def can(
    docs: list[dict[str, Any]], account: str, namespace: str, want: tuple[str, str, str]
) -> bool:
    """Whether `account` may do `want` (group, resource, verb) in `namespace`:
    a ClusterRoleBinding grants in every namespace, a RoleBinding only in its own."""
    roles = {
        (d["kind"], d["metadata"].get("namespace", ""), d["metadata"]["name"]): d
        for d in docs
        if d["kind"] in ("Role", "ClusterRole")
    }
    for binding in (d for d in docs if d["kind"] in ("RoleBinding", "ClusterRoleBinding")):
        subjects = {s["name"] for s in binding.get("subjects", []) if s["kind"] == "ServiceAccount"}
        scope = binding["metadata"].get("namespace", "")
        if account not in subjects or (binding["kind"] == "RoleBinding" and scope != namespace):
            continue
        ref = binding["roleRef"]
        role = roles.get((ref["kind"], scope if ref["kind"] == "Role" else "", ref["name"]))
        if role is not None and any(rule_allows(r, *want) for r in role.get("rules", [])):
            return True
    return False


def operator_account(docs: list[dict[str, Any]]) -> str:
    for doc in docs:
        labels = doc["metadata"].get("labels", {})
        if doc["kind"] == "Deployment" and labels.get("app.kubernetes.io/component") == "operator":
            name: str = doc["spec"]["template"]["spec"]["serviceAccountName"]
            return name
    raise AssertionError("the render has no operator Deployment")


def pod_accounts(docs: list[dict[str, Any]]) -> dict[str, str]:
    return {
        d["metadata"]["name"]: d["spec"]["template"]["spec"]["serviceAccountName"]
        for d in docs
        if d["kind"] in ("Deployment", "StatefulSet", "Job")
    }


class HelmServiceAccountTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("helm") is None:
            raise AssertionError("helm is not on PATH; this test renders the chart")

    def test_only_the_controller_reads_tenants(self) -> None:
        docs = render(REGISTRY)
        accounts = pod_accounts(docs)
        self.assertEqual(accounts["vmafx-controller"], "vmafx-controller")
        self.assertEqual(tenant_readers(docs), {"vmafx-controller"})
        others = {name: sa for name, sa in accounts.items() if name != "vmafx-controller"}
        self.assertTrue(others, "the render has no other workload to compare with")
        for name, sa in others.items():
            self.assertNotEqual(sa, "vmafx-controller", f"{name} runs as the controller's account")
        account_names = {d["metadata"]["name"] for d in docs if d["kind"] == "ServiceAccount"}
        self.assertIn("vmafx-controller", account_names)

    def test_operator_cluster_role_has_no_tenant_rule(self) -> None:
        docs = render(REGISTRY)
        account = operator_account(docs)
        for verb in ("get", "list", "watch"):
            for namespace in ("vmafx", "tenant-a"):
                self.assertFalse(
                    can(docs, account, namespace, ("vmafx.dev", "vmafxtenants", verb)),
                    "the operator has no tenant reconciler",
                )

    def test_operator_records_events_in_every_namespace(self) -> None:
        docs = render(OPERATOR)
        account = operator_account(docs)
        for namespace in ("vmafx", "tenant-a", "default"):
            for verb in ("create", "patch"):
                with self.subTest(namespace=namespace, verb=verb):
                    self.assertTrue(can(docs, account, namespace, ("", "events", verb)))
            self.assertTrue(
                can(docs, account, namespace, ("vmafx.dev", "vmafxmodeltrainings", "get"))
            )

    def test_operator_event_grant_is_write_only(self) -> None:
        docs = render(OPERATOR)
        account = operator_account(docs)
        for verb in ("get", "list", "watch", "update", "delete", "deletecollection"):
            for namespace in ("vmafx", "tenant-a"):
                with self.subTest(namespace=namespace, verb=verb):
                    self.assertFalse(can(docs, account, namespace, ("", "events", verb)))

    def test_operator_pods_and_leases_stay_in_the_release_namespace(self) -> None:
        docs = render(OPERATOR)
        account = operator_account(docs)
        for want in (("", "pods", "create"), ("coordination.k8s.io", "leases", "update")):
            with self.subTest(resource=want[1]):
                self.assertTrue(can(docs, account, "vmafx", want))
                self.assertFalse(can(docs, account, "tenant-a", want))

    def test_without_the_operator_no_account_records_events(self) -> None:
        docs = render("operator: {enabled: false}\n")
        accounts = {d["metadata"]["name"] for d in docs if d["kind"] == "ServiceAccount"}
        self.assertTrue(accounts)
        for account in accounts:
            self.assertFalse(can(docs, account, "tenant-a", ("", "events", "create")))

    def test_without_a_registry_nobody_reads_tenants(self) -> None:
        docs = render(
            "controller: {enabled: true}\nnode: {enabled: true}\noperator: {enabled: true}\n"
            "auth: {enabled: true, disabled: true}\n"
        )
        self.assertEqual(tenant_readers(docs), set())


if __name__ == "__main__":
    unittest.main()
