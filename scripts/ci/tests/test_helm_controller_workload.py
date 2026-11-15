#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The vmafx-controller workload of the Helm chart, checked on real renders.

ADR-1589: controller.enabled deploys one controller replica (Recreate, SQLite
queue on a ReadWriteOnce volume) with a Service carrying its HTTP and gRPC
ports; the nodes and the operator are pointed at it, their controller tokens
are mounted from Secrets, and the NetworkPolicies open exactly the flows
between them. Positive, negative and boundary renders; the helm-chart
workflow runs this file and needs ``helm`` on PATH.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

CHART = Path(__file__).resolve().parents[3] / "deploy" / "helm" / "vmafx"
HELM_TIMEOUT_S = 60
SCRIPT_TIMEOUT_S = 60
BASE = """
controller: {enabled: true}
auth: {enabled: true, disabled: true}
"""


def render(values: str = BASE, *args: str) -> list[dict[str, Any]]:
    """Render the chart in namespace vmafx with an extra values file."""
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        fh.write(values)
        fh.flush()
        cmd = [
            "helm",
            "template",
            "vmafx",
            str(CHART),
            "--namespace",
            "vmafx",
            "-f",
            fh.name,
            *args,
        ]
        result = subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
            cmd, capture_output=True, text=True, check=False, timeout=HELM_TIMEOUT_S
        )
    if result.returncode != 0:
        raise AssertionError(f"helm template failed: {result.stderr}")
    return [doc for doc in yaml.safe_load_all(result.stdout) if doc]


def named(docs: list[dict[str, Any]], kind: str, name: str) -> dict[str, Any]:
    return next(d for d in docs if d["kind"] == kind and d["metadata"]["name"] == name)


def names(docs: list[dict[str, Any]], kind: str) -> set[str]:
    return {d["metadata"]["name"] for d in docs if d["kind"] == kind}


def container(deploy: dict[str, Any]) -> dict[str, Any]:
    c: dict[str, Any] = deploy["spec"]["template"]["spec"]["containers"][0]
    return c


def env(deploy: dict[str, Any]) -> dict[str, str]:
    return {e["name"]: e.get("value", "") for e in container(deploy).get("env", [])}


class HelmControllerWorkloadTest(unittest.TestCase):
    release_tag: str

    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("helm") is None:
            raise AssertionError("helm is not on PATH; this test renders the chart")
        chart = yaml.safe_load((CHART / "Chart.yaml").read_text(encoding="utf-8"))
        cls.release_tag = "v" + str(chart["appVersion"]).removeprefix("v")

    def test_controller_deployment_service_and_queue_volume(self) -> None:
        docs = render()
        deploy = named(docs, "Deployment", "vmafx-controller")
        self.assertEqual(deploy["spec"]["replicas"], 1)
        self.assertEqual(deploy["spec"]["strategy"], {"type": "Recreate"})
        labels = deploy["spec"]["selector"]["matchLabels"]
        self.assertEqual(labels["app.kubernetes.io/component"], "controller")
        c = container(deploy)
        self.assertEqual(c["image"], f"ghcr.io/vmafx/vmafx-controller:{self.release_tag}")
        self.assertEqual(
            {p["name"]: p["containerPort"] for p in c["ports"]}, {"http": 8080, "grpc": 9090}
        )
        e = env(deploy)
        self.assertEqual(e["VMAFX_HTTP_ADDR"], ":8080")
        self.assertEqual(e["VMAFX_GRPC_LISTEN"], ":9090")
        self.assertEqual(e["VMAFX_DB_PATH"], "/data/vmafx-controller.db")
        self.assertEqual(e["VMAFX_AUTH_DISABLED"], "true")
        self.assertEqual(c["livenessProbe"]["httpGet"], {"path": "/healthz", "port": "http"})
        self.assertEqual(c["readinessProbe"]["httpGet"], {"path": "/readyz", "port": "http"})
        volumes = {v["name"]: v for v in deploy["spec"]["template"]["spec"]["volumes"]}
        self.assertEqual(
            volumes["data"]["persistentVolumeClaim"]["claimName"], "vmafx-controller-data"
        )
        pvc = named(docs, "PersistentVolumeClaim", "vmafx-controller-data")
        self.assertEqual(pvc["spec"]["accessModes"], ["ReadWriteOnce"])
        svc = named(docs, "Service", "vmafx-controller")
        self.assertEqual(
            {p["name"]: p["port"] for p in svc["spec"]["ports"]}, {"http": 8080, "grpc": 9090}
        )
        self.assertEqual(svc["spec"]["selector"]["app.kubernetes.io/component"], "controller")

    def test_queue_volume_choices(self) -> None:
        docs = render(BASE + "controller: {enabled: true, persistence: {enabled: false}}\n")
        volumes = named(docs, "Deployment", "vmafx-controller")["spec"]["template"]["spec"][
            "volumes"
        ]
        self.assertEqual(
            next(v for v in volumes if v["name"] == "data"), {"name": "data", "emptyDir": {}}
        )
        self.assertNotIn("vmafx-controller-data", names(docs, "PersistentVolumeClaim"))
        docs = render(BASE + "controller: {enabled: true, persistence: {existingClaim: queue}}\n")
        volumes = named(docs, "Deployment", "vmafx-controller")["spec"]["template"]["spec"][
            "volumes"
        ]
        self.assertEqual(
            next(v for v in volumes if v["name"] == "data")["persistentVolumeClaim"]["claimName"],
            "queue",
        )
        self.assertNotIn("vmafx-controller-data", names(docs, "PersistentVolumeClaim"))

    def test_server_workload_carries_no_auth_settings(self) -> None:
        docs = render(
            "controller: {enabled: true}\n"
            "auth: {enabled: true, issuer: https://idp.example.com/, jwksEndpoint: https://idp.example.com/k}\n"
        )
        self.assertFalse(
            [k for k in env(named(docs, "Deployment", "vmafx")) if "AUTH" in k or "JWKS" in k]
        )
        self.assertEqual(
            env(named(docs, "Deployment", "vmafx-controller"))["VMAFX_AUTH_ISSUER"],
            "https://idp.example.com/",
        )

    def test_nodes_and_operator_reach_the_controller(self) -> None:
        docs = render(BASE + "node: {enabled: true}\noperator: {enabled: true}\n")
        self.assertEqual(
            env(named(docs, "Deployment", "vmafx-node"))["VMAFX_CONTROLLER_ADDR"],
            "vmafx-controller.vmafx.svc:9090",
        )
        op = env(named(docs, "Deployment", "vmafx-operator"))
        self.assertEqual(op["VMAFX_CONTROLLER_GRPC_ADDR"], "vmafx-controller.vmafx.svc:9090")
        self.assertEqual(op["VMAFX_CONTROLLER_HTTP_ADDR"], "http://vmafx-controller.vmafx.svc:8080")
        docs = render(BASE + "node: {enabled: true, controllerAddr: other:9999}\n")
        self.assertEqual(
            env(named(docs, "Deployment", "vmafx-node"))["VMAFX_CONTROLLER_ADDR"], "other:9999"
        )
        docs = render("node: {enabled: true}\n")
        self.assertNotIn("VMAFX_CONTROLLER_ADDR", env(named(docs, "Deployment", "vmafx-node")))

    def test_controller_tokens_are_mounted_from_secrets(self) -> None:
        docs = render(
            BASE
            + "node: {enabled: true, controllerToken: {secretName: node-token}}\n"
            + "operator: {enabled: true, controllerToken: {secretName: op-token, key: jwt}}\n"
        )
        for name, secret, key in (
            ("vmafx-node", "node-token", "token"),
            ("vmafx-operator", "op-token", "jwt"),
        ):
            deploy = named(docs, "Deployment", name)
            self.assertEqual(
                env(deploy)["VMAFX_CONTROLLER_TOKEN_FILE"],
                "/var/run/secrets/vmafx/controller-token/token",
            )
            vol = next(
                v
                for v in deploy["spec"]["template"]["spec"]["volumes"]
                if v["name"] == "controller-token"
            )
            self.assertEqual(vol["secret"]["secretName"], secret)
            self.assertEqual(vol["secret"]["items"], [{"key": key, "path": "token"}])
            mount = next(
                m for m in container(deploy)["volumeMounts"] if m["name"] == "controller-token"
            )
            self.assertTrue(mount["readOnly"])
        docs = render(BASE + "node: {enabled: true}\noperator: {enabled: true}\n")
        for name in ("vmafx-node", "vmafx-operator"):
            self.assertNotIn("VMAFX_CONTROLLER_TOKEN_FILE", env(named(docs, "Deployment", name)))

    def test_network_policies_open_the_controller_flows(self) -> None:
        values = (
            BASE
            + "node: {enabled: true}\noperator: {enabled: true}\nnetworkPolicy: {enabled: true}\n"
        )
        docs = render(values)
        ingress = named(docs, "NetworkPolicy", "vmafx-allow-controller-ingress")
        self.assertEqual(
            sorted(p["port"] for p in ingress["spec"]["ingress"][0]["ports"]), [8080, 9090]
        )
        to_ctrl = named(docs, "NetworkPolicy", "vmafx-allow-operator-to-controller")["spec"][
            "egress"
        ][0]
        self.assertEqual(
            to_ctrl["to"][0]["podSelector"]["matchLabels"]["app.kubernetes.io/component"],
            "controller",
        )
        node = named(docs, "NetworkPolicy", "vmafx-allow-node-to-controller")["spec"]["egress"][0]
        self.assertEqual(
            node["to"][0]["podSelector"]["matchLabels"]["app.kubernetes.io/component"], "controller"
        )
        self.assertEqual(node["ports"], [{"protocol": "TCP", "port": 9090}])
        self.assertIn("vmafx-controller-default-deny", names(docs, "NetworkPolicy"))
        # auth.disabled: no identity provider to reach.
        self.assertNotIn(
            "vmafx-allow-controller-to-identity-provider", names(docs, "NetworkPolicy")
        )
        docs = render(
            "controller: {enabled: true}\nnetworkPolicy: {enabled: true}\n"
            "auth: {enabled: true, issuer: https://idp.example.com/, jwksEndpoint: https://idp.example.com/k}\n"
        )
        idp = named(docs, "NetworkPolicy", "vmafx-allow-controller-to-identity-provider")["spec"][
            "egress"
        ][0]
        self.assertEqual(idp["ports"], [{"protocol": "TCP", "port": 443}])

    def test_selectors_stay_isolated(self) -> None:
        """Rendered workload selectors must remain isolated across controller components."""
        values = (
            BASE
            + "node: {enabled: true}\noperator: {enabled: true}\npodDisruptionBudget: {enabled: true}\n"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
            yaml.safe_dump_all(render(values), fh)
            fh.flush()
            result = subprocess.run(  # noqa: S603 -- repository script; argv built here
                [
                    sys.executable,
                    str(CHART.parents[2] / "scripts" / "ci" / "check-helm-selector-isolation.py"),
                    fh.name,
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=SCRIPT_TIMEOUT_S,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
