#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary renders of the chart's MCP component.

mcp.enabled (default false) renders a vmafx-mcp Deployment and Service on the
vmafx-node image with the command vmafx-mcp, the streamable HTTP transport and
the controller address; templates/mcp-validate.yaml refuses it without a token
Secret or an explicit mcp.auth.noAuth. The Intel xe render requests the device
plugin resource and names no device. The helm-chart workflow runs this file; it
needs ``helm`` on PATH.
"""

from __future__ import annotations

import shutil
import subprocess
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

CHART = Path(__file__).resolve().parents[3] / "deploy" / "helm" / "vmafx"
STACK = [
    "controller.enabled=true",
    "auth.enabled=true",
    "auth.disabled=true",
    "node.enabled=true",
    "networkPolicy.enabled=true",
]
MCP = ["mcp.enabled=true", "mcp.auth.token.secretName=mcp-token"]


def helm(*sets: str) -> subprocess.CompletedProcess[str]:
    """Render the chart in namespace vmafx with --set for each entry."""
    cmd = ["helm", "template", "vmafx", str(CHART), "--namespace", "vmafx"]
    for entry in sets:
        cmd += ["--set", entry]
    return subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
        cmd, capture_output=True, text=True, check=False
    )


def render(*sets: str) -> list[dict[str, Any]]:
    result = helm(*sets)
    if result.returncode != 0:
        raise AssertionError(f"helm template failed: {result.stderr}")
    return [doc for doc in yaml.safe_load_all(result.stdout) if doc]


def named(docs: list[dict[str, Any]], kind: str, name: str) -> dict[str, Any] | None:
    return next((d for d in docs if d["kind"] == kind and d["metadata"]["name"] == name), None)


def mcp_container(docs: list[dict[str, Any]]) -> dict[str, Any]:
    deploy = named(docs, "Deployment", "vmafx-mcp")
    if deploy is None:
        raise AssertionError("no vmafx-mcp Deployment rendered")
    container: dict[str, Any] = deploy["spec"]["template"]["spec"]["containers"][0]
    return container


def env_of(container: dict[str, Any]) -> dict[str, Any]:
    return {e["name"]: e.get("value", e.get("valueFrom")) for e in container["env"]}


@unittest.skipUnless(shutil.which("helm"), "helm not on PATH")
class McpComponent(unittest.TestCase):
    def test_default_renders_no_mcp(self) -> None:
        docs = render(*STACK)
        self.assertFalse([d for d in docs if "mcp" in d["metadata"]["name"]], "mcp is opt-in")

    def test_controller_node_and_mcp_render(self) -> None:
        docs = render(*STACK, *MCP)
        components = {
            d["metadata"]["labels"].get("app.kubernetes.io/component")
            for d in docs
            if d["kind"] in ("Deployment", "DaemonSet")
        }
        self.assertLessEqual({"controller", "node", "mcp"}, components)
        self.assertIsNotNone(named(docs, "Service", "vmafx-mcp"))

    def test_runs_node_image_with_mcp_command(self) -> None:
        container = mcp_container(render(*STACK, *MCP))
        self.assertRegex(container["image"], r"/vmafx-node:")
        self.assertEqual(container["command"], ["vmafx-mcp"])

    def test_http_transport_token_and_controller(self) -> None:
        env = env_of(mcp_container(render(*STACK, *MCP)))
        self.assertEqual(env["VMAFX_MCP_TRANSPORT"], "http")
        self.assertEqual(env["VMAFX_MCP_HTTP_ADDR"], ":3000")
        self.assertEqual(
            env["VMAFX_MCP_HTTP_TOKEN"]["secretKeyRef"],
            {"name": "mcp-token", "key": "token"},
        )
        self.assertEqual(env["VMAFX_CONTROLLER_ADDR"], "vmafx-controller.vmafx.svc:9090")
        self.assertNotIn("VMAFX_MCP_HTTP_NO_AUTH", env)

    def test_enabled_without_auth_refused(self) -> None:
        """Planted negative: an MCP server without a token or explicit noAuth."""
        result = helm(*STACK, "mcp.enabled=true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("mcp.enabled needs auth", result.stderr)

    def test_no_auth_is_explicit(self) -> None:
        env = env_of(mcp_container(render(*STACK, "mcp.enabled=true", "mcp.auth.noAuth=true")))
        self.assertEqual(env["VMAFX_MCP_HTTP_NO_AUTH"], "1")
        self.assertNotIn("VMAFX_MCP_HTTP_TOKEN", env)

    def test_own_image_boundary(self) -> None:
        container = mcp_container(
            render(*STACK, *MCP, "mcp.image.repository=registry.example/mcp", "mcp.image.tag=t1")
        )
        self.assertEqual(container["image"], "registry.example/mcp:t1")

    def test_network_policies(self) -> None:
        docs = render(*STACK, *MCP)
        for name in (
            "vmafx-allow-mcp-ingress",
            "vmafx-allow-mcp-to-controller",
            "vmafx-allow-controller-from-mcp",
        ):
            self.assertIsNotNone(named(docs, "NetworkPolicy", name), name)
        ingress = named(docs, "NetworkPolicy", "vmafx-allow-mcp-ingress")
        assert ingress is not None
        self.assertEqual(ingress["spec"]["ingress"][0]["ports"][0]["port"], 3000)
        off = render(*STACK, *MCP, "networkPolicy.allow.mcpIngress.enabled=false")
        self.assertIsNone(named(off, "NetworkPolicy", "vmafx-allow-mcp-ingress"))


@unittest.skipUnless(shutil.which("helm"), "helm not on PATH")
class IntelXeSelection(unittest.TestCase):
    def test_xe_requests_resource_and_names_no_device(self) -> None:
        result = helm("node.enabled=true", "gpu.vendor=intel", "gpu.intelDriver=xe")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gpu.intel.com/xe", result.stdout)
        for needle in ("/dev/dri", "renderD", "hostPath", "nodeName:"):
            self.assertNotIn(needle, result.stdout, needle)


if __name__ == "__main__":
    unittest.main()
