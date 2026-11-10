#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary renders of the chart's auth settings.

ADR-1519: with a tenant registry the controller reads VmafxTenant resources,
so the chart must hand it the source and namespace (and not the global
provider, which the controller then refuses), grant its service account read
access to VmafxTenants, fill each rendered VmafxTenant from the global
defaults, keep ``enabled: false`` (``default true`` used to turn it into
true), and open the API server in the NetworkPolicy. Auth settings the
rendered workload would not apply fail the render. The helm-chart workflow
runs this file; it needs ``helm`` on PATH.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

CHART = Path(__file__).resolve().parents[3] / "deploy" / "helm" / "vmafx"
HELM_TIMEOUT_S = 60
CONTROLLER = ["--set", "controller.enabled=true"]
TENANTS = """
controller: {enabled: true}
auth:
  enabled: true
  issuer: https://idp.example.com/
  jwksEndpoint: https://idp.example.com/keys
  tenants:
    - tenantId: acme
      enabled: false
      oidc: {audience: vmafx-api}
    - tenantId: rival
      oidc: {issuer: https://rival.example.com/, jwksEndpoint: https://rival.example.com/keys}
      rbac: {allowedRoles: ["vmafx:reader"]}
networkPolicy: {enabled: true}
"""


def helm(*args: str, values: str | None = None) -> subprocess.CompletedProcess[str]:
    """Render the chart in namespace vmafx; values is an extra values file."""
    cmd = ["helm", "template", "vmafx", str(CHART), "--namespace", "vmafx", *args]
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        if values is not None:
            fh.write(values)
            fh.flush()
            cmd += ["-f", fh.name]
        return subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
            cmd, capture_output=True, text=True, check=False, timeout=HELM_TIMEOUT_S
        )


def render(*args: str, values: str | None = None) -> list[dict[str, Any]]:
    result = helm(*args, values=values)
    if result.returncode != 0:
        raise AssertionError(f"helm template failed: {result.stderr}")
    return [doc for doc in yaml.safe_load_all(result.stdout) if doc]


def kinds(docs: list[dict[str, Any]], kind: str) -> list[dict[str, Any]]:
    return [d for d in docs if d["kind"] == kind]


def deployment_env(docs: list[dict[str, Any]], name: str) -> dict[str, str]:
    deploy = next(d for d in kinds(docs, "Deployment") if d["metadata"]["name"] == name)
    container = deploy["spec"]["template"]["spec"]["containers"][0]
    return {e["name"]: e.get("value", "") for e in container.get("env", [])}


def server_env(docs: list[dict[str, Any]]) -> dict[str, str]:
    """The auth settings' workload: the controller (ADR-1589)."""
    return deployment_env(docs, "vmafx-controller")


# Render failures: name -> (helm args, extra values file, stderr needle).
RENDER_FAILURES: dict[str, tuple[list[str], str | None, str]] = {
    "auth without the controller workload": (
        ["--set", "auth.enabled=true"],
        None,
        "set controller.enabled",
    ),
    "controller without auth": ([*CONTROLLER], None, "controller.enabled needs auth.enabled"),
    "controller image on the server workload": (
        ["--set", "image.repository=ghcr.io/vmafx/vmafx-controller"],
        None,
        "no longer runs the controller",
    ),
    "auth via controller.env": (
        [
            *CONTROLLER,
            "--set",
            "auth.enabled=true",
            "--set",
            "controller.env.VMAFX_AUTH_DISABLED=true",
        ],
        None,
        "controller.env.VMAFX_AUTH_DISABLED",
    ),
    "tenants without auth.enabled": (
        [],
        "auth: {tenants: [{tenantId: acme}]}",
        "need auth.enabled",
    ),
    "tenantSource without auth.enabled": (
        ["--set", "auth.tenantSource=kubernetes"],
        None,
        "need auth.enabled",
    ),
    "disabled with tenants": (
        [
            *CONTROLLER,
            "--set",
            "auth.enabled=true",
            "--set",
            "auth.disabled=true",
            "--set",
            "auth.tenantSource=kubernetes",
        ],
        None,
        "cannot be combined",
    ),
    "tenant without issuer": (
        [*CONTROLLER, "--set", "auth.enabled=true"],
        "auth: {tenants: [{tenantId: acme, oidc: {jwksEndpoint: https://a/k}}]}",
        "oidc.issuer",
    ),
    "auth via env": (
        [
            *CONTROLLER,
            "--set",
            "auth.enabled=true",
            "--set",
            "env.VMAFX_AUTH_DISABLED=true",
        ],
        None,
        "env.VMAFX_AUTH_DISABLED",
    ),
    "JWKS via env": (
        [*CONTROLLER, "--set", "env.VMAFX_JWKS_ENDPOINT=https://x/k"],
        None,
        "env.VMAFX_JWKS_ENDPOINT",
    ),
    "empty allowedRoles": (
        [*CONTROLLER, "--set", "auth.enabled=true"],
        "auth: {issuer: https://a/, jwksEndpoint: https://a/k,"
        " tenants: [{tenantId: acme, rbac: {allowedRoles: []}}]}",
        "allowedRoles is empty",
    ),
    "unknown tenantSource": (
        [*CONTROLLER, "--set", "auth.enabled=true", "--set", "auth.tenantSource=file"],
        None,
        "tenantSource",
    ),
    "scoringRoots with a tenant registry": (
        [*CONTROLLER, "--set", "auth.enabled=true"],
        "auth: {issuer: https://a/, jwksEndpoint: https://a/k, scoringRoots: [/media],"
        " tenants: [{tenantId: acme}]}",
        "auth.scoringRoots is not used with a tenant registry",
    ),
    "scoringRoots without auth": (
        [],
        "auth: {scoringRoots: [/media]}",
        "auth.scoringRoots needs auth.enabled",
    ),
}


ROLES_GO = CHART.parents[2] / "cmd" / "vmafx-controller" / "auth" / "middleware.go"


def controller_roles() -> set[str]:
    """The role strings the controller defines (``Role* = "vmafx:..."``)."""
    text = ROLES_GO.read_text(encoding="utf-8")
    return set(re.findall(r'^\s*Role[A-Za-z]+\s*=\s*"(vmafx:[a-z]+)"', text, re.MULTILINE))


def tenant_rbac_schema() -> dict[str, Any]:
    """The rbac properties of the VmafxTenant CRD's v1 schema."""
    crd = yaml.safe_load(
        (CHART / "crds" / "vmafx.dev_vmafxtenants.yaml").read_text(encoding="utf-8")
    )
    spec = crd["spec"]["versions"][0]["schema"]["openAPIV3Schema"]["properties"]["spec"]
    rbac: dict[str, Any] = spec["properties"]["rbac"]["properties"]
    return rbac


class HelmControllerAuthTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("helm") is None:
            raise AssertionError("helm is not on PATH; this test renders the chart")

    def test_crd_role_enums_follow_the_controller(self) -> None:
        """ADR-1563: allowedRoles offers every controller role, vmafx:node
        included; defaultRole offers every role except vmafx:node."""
        roles = controller_roles()
        self.assertIn("vmafx:node", roles)
        rbac = tenant_rbac_schema()
        self.assertEqual(set(rbac["allowedRoles"]["items"]["enum"]), roles)
        self.assertEqual(set(rbac["defaultRole"]["enum"]), roles - {"vmafx:node"})
        self.assertNotIn("vmafx:node", rbac["allowedRoles"]["default"])

    def test_defaults_render_no_tenant_resources(self) -> None:
        docs = render()
        self.assertEqual(kinds(docs, "VmafxTenant"), [])
        self.assertEqual(kinds(docs, "Role"), [])
        names = {d["metadata"]["name"] for d in kinds(docs, "Deployment")}
        self.assertNotIn("vmafx-controller", names, "the controller is opt-in")
        self.assertNotIn("VMAFX_AUTH_DISABLED", deployment_env(docs, "vmafx"))

    def test_tenant_registry_wiring(self) -> None:
        docs = render(values=TENANTS)
        env = server_env(docs)
        self.assertEqual(env["VMAFX_AUTH_TENANTS_SOURCE"], "kubernetes")
        self.assertEqual(env["VMAFX_AUTH_TENANTS_NAMESPACE"], "vmafx")
        for refused in (
            "VMAFX_JWKS_ENDPOINT",
            "VMAFX_AUTH_ISSUER",
            "VMAFX_AUTH_AUDIENCE",
            "VMAFX_AUTH_TENANT_CLAIM",
            "VMAFX_AUTH_ROLES_CLAIM",
        ):
            self.assertNotIn(
                refused, env, "the controller refuses the global provider with a tenant source"
            )
        role = kinds(docs, "Role")[0]
        self.assertEqual(
            role["rules"],
            [
                {
                    "apiGroups": ["vmafx.dev"],
                    "resources": ["vmafxtenants"],
                    "verbs": ["get", "list", "watch"],
                }
            ],
        )
        binding = kinds(docs, "RoleBinding")[0]
        self.assertEqual(binding["subjects"][0]["name"], "vmafx-controller")  # ADR-1592
        self.assertEqual(binding["roleRef"]["name"], role["metadata"]["name"])
        policies = {p["metadata"]["name"] for p in kinds(docs, "NetworkPolicy")}
        self.assertIn("vmafx-allow-controller-to-apiserver", policies)

    def test_rendered_tenants_take_global_defaults_and_keep_enabled_false(self) -> None:
        tenants = {
            t["spec"]["tenantId"]: t["spec"] for t in kinds(render(values=TENANTS), "VmafxTenant")
        }
        acme, rival = tenants["acme"], tenants["rival"]
        self.assertIs(acme["enabled"], False)
        self.assertIs(rival["enabled"], True)
        self.assertEqual(acme["oidc"]["issuer"], "https://idp.example.com/")
        self.assertEqual(acme["oidc"]["jwksEndpoint"], "https://idp.example.com/keys")
        self.assertEqual(acme["oidc"]["audience"], "vmafx-api")
        self.assertEqual(rival["oidc"]["issuer"], "https://rival.example.com/")
        self.assertNotIn("audience", rival["oidc"])
        self.assertEqual(rival["rbac"]["allowedRoles"], ["vmafx:reader"])

    def test_tenant_source_without_tenants_is_explicit(self) -> None:
        docs = render(
            *CONTROLLER, "--set", "auth.enabled=true", "--set", "auth.tenantSource=kubernetes"
        )
        self.assertEqual(server_env(docs)["VMAFX_AUTH_TENANTS_SOURCE"], "kubernetes")
        self.assertEqual(kinds(docs, "VmafxTenant"), [])
        self.assertEqual(len(kinds(docs, "Role")), 1)

    def test_single_provider_mode_passes_the_global_provider(self) -> None:
        docs = render(
            *CONTROLLER,
            "--set",
            "auth.enabled=true",
            "--set",
            "auth.issuer=https://idp.example.com/",
            "--set",
            "auth.jwksEndpoint=https://idp.example.com/keys",
        )
        env = server_env(docs)
        self.assertEqual(env["VMAFX_AUTH_ISSUER"], "https://idp.example.com/")
        self.assertNotIn("VMAFX_AUTH_TENANTS_SOURCE", env)
        self.assertEqual(kinds(docs, "Role"), [])

    def test_scoring_roots_reach_the_controller(self) -> None:
        """ADR-1577: auth.scoringRoots becomes VMAFX_SCORING_ROOTS without a
        registry; a tenant's scoring.roots reach its VmafxTenant; none
        renders no roots (the controller then refuses every input)."""
        docs = render(
            *CONTROLLER,
            values="auth: {enabled: true, issuer: https://a/, jwksEndpoint: https://a/k,"
            " scoringRoots: ['/media/{tenant}', 's3:media/{tenant}']}",
        )
        self.assertEqual(
            server_env(docs)["VMAFX_SCORING_ROOTS"], "/media/{tenant},s3:media/{tenant}"
        )
        docs = render(
            values=TENANTS.replace(
                "oidc: {audience: vmafx-api}",
                "oidc: {audience: vmafx-api}\n      scoring: {roots: [/media/acme]}",
            )
        )
        tenants = {t["spec"]["tenantId"]: t["spec"] for t in kinds(docs, "VmafxTenant")}
        self.assertEqual(tenants["acme"]["scoring"], {"roots": ["/media/acme"]})
        self.assertNotIn("scoring", tenants["rival"])
        self.assertNotIn("VMAFX_SCORING_ROOTS", server_env(docs))

    def test_auth_settings_the_workload_would_ignore_fail_the_render(self) -> None:
        for name, (args, values, needle) in RENDER_FAILURES.items():
            with self.subTest(name):
                result = helm(*args, values=values)
                self.assertNotEqual(result.returncode, 0, f"{name}: rendered")
                self.assertIn(needle, result.stderr, f"{name}: {result.stderr}")


if __name__ == "__main__":
    unittest.main()
