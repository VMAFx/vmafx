#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The vmafx-node contract of the Helm chart, checked on real `helm template`
renders: the controller address the node's controller client reads, the
NetworkPolicy rules it needs, the storage mode the node accepts, and the GPU
device-plugin resource the pods request.

Requires the `helm` binary (the helm-chart workflow installs it); a missing
binary is a failure, not a skip.
"""

from __future__ import annotations

import shutil
import subprocess
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

CHART = Path(__file__).resolve().parents[3] / "deploy" / "helm" / "vmafx"


def render(*sets: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    """Run `helm template` with the given --set values."""
    helm = shutil.which("helm")
    if helm is None:
        raise AssertionError("helm binary not found on PATH; install helm to run this test")
    argv = [helm, "template", "vmafx", str(CHART), "--namespace", "vmafx"]
    for value in sets:
        argv += ["--set", value]
    return subprocess.run(  # noqa: S603 -- resolved helm binary, fixed argv, no shell
        argv, capture_output=True, text=True, check=check
    )


def manifests(*sets: str) -> list[dict[str, Any]]:
    """Rendered manifests, empty documents dropped."""
    return [doc for doc in yaml.safe_load_all(render(*sets).stdout) if doc]


def node_container(*sets: str) -> dict[str, Any]:
    """The vmafx-node container of the node Deployment."""
    for doc in manifests("node.enabled=true", *sets):
        if doc["kind"] == "Deployment" and doc["metadata"]["name"].endswith("-node"):
            container: dict[str, Any] = doc["spec"]["template"]["spec"]["containers"][0]
            return container
    raise AssertionError("no node Deployment rendered")


def node_env(*sets: str) -> dict[str, str]:
    return {item["name"]: item.get("value", "") for item in node_container(*sets)["env"]}


def policy(name_suffix: str, *sets: str) -> dict[str, Any] | None:
    for doc in manifests("node.enabled=true", "networkPolicy.enabled=true", *sets):
        if doc["kind"] == "NetworkPolicy" and doc["metadata"]["name"].endswith(name_suffix):
            return doc
    return None


class ControllerAddress(unittest.TestCase):
    def test_unset_without_value(self) -> None:
        """No node.controllerAddr: the variable is absent, the node runs standalone."""
        self.assertNotIn("VMAFX_CONTROLLER_ADDR", node_env())

    def test_rendered_verbatim(self) -> None:
        env = node_env("node.controllerAddr=vmafx-controller.vmafx:9090")
        self.assertEqual(env["VMAFX_CONTROLLER_ADDR"], "vmafx-controller.vmafx:9090")


class ModelDirectory(unittest.TestCase):
    def test_image_models_without_a_models_volume(self) -> None:
        # persistence.models.mountPath has a default even when no volume is
        # mounted; a node pointed at it found no model and failed every job.
        env = node_env("node.enabled=true")
        self.assertEqual(env["VMAFX_MODEL_DIR"], "/usr/local/share/vmafx/model")

    def test_models_volume_when_mounted(self) -> None:
        env = node_env(
            "node.enabled=true",
            "persistence.models.enabled=true",
            "persistence.models.mountPath=/srv/models",
        )
        self.assertEqual(env["VMAFX_MODEL_DIR"], "/srv/models")


class NodeToControllerEgress(unittest.TestCase):
    def test_egress_to_controller_port(self) -> None:
        doc = policy("-allow-node-to-controller", "node.controllerAddr=ctrl:9090")
        assert doc is not None, "no node-to-controller egress policy rendered"
        self.assertEqual(doc["spec"]["policyTypes"], ["Egress"])
        ports = doc["spec"]["egress"][0]["ports"]
        self.assertEqual(ports, [{"protocol": "TCP", "port": 9090}])

    def test_port_follows_value(self) -> None:
        doc = policy(
            "-allow-node-to-controller",
            "node.controllerAddr=ctrl:7000",
            "networkPolicy.allow.nodeToController.port=7000",
        )
        assert doc is not None
        self.assertEqual(doc["spec"]["egress"][0]["ports"][0]["port"], 7000)

    def test_absent_without_controller(self) -> None:
        """Boundary: no controller address, no egress hole."""
        self.assertIsNone(policy("-allow-node-to-controller"))

    def test_schema_refuses_unknown_key(self) -> None:
        """Negative: a misspelt allow-rule key is refused by the schema."""
        result = render("networkPolicy.allow.nodeToController.prot=1", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("prot", result.stderr)


# node.fuse settings mount mode needs (ADR-1593).
FUSE = ("node.fuse.enabled=true", "node.fuse.resourceName=devic.es/fuse")


class StorageMode(unittest.TestCase):
    def test_default_is_http_serve(self) -> None:
        env = node_env()
        self.assertEqual(env["VMAFX_STORAGE_MODE"], "http-serve")
        self.assertNotIn("VMAFX_STORAGE_MOUNT_ROOT", env)

    def test_every_node_mode_renders(self) -> None:
        for mode in ("http-serve", "mount", "auto"):
            with self.subTest(mode=mode):
                self.assertEqual(
                    node_env(f"storage.mode={mode}", *FUSE)["VMAFX_STORAGE_MODE"], mode
                )

    def test_mount_needs_fuse(self) -> None:
        """Negative: mount mode without node.fuse is refused (ADR-1593)."""
        result = render("node.enabled=true", "storage.mode=mount", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("storage.mode mount needs FUSE", result.stderr)

    def test_mount_root_rendered(self) -> None:
        env = node_env("storage.mode=mount", "storage.mountRoot=/rclone-mount", *FUSE)
        self.assertEqual(env["VMAFX_STORAGE_MOUNT_ROOT"], "/rclone-mount")

    def test_unknown_mode_refused(self) -> None:
        """Negative: the old value rclone, never implemented, is refused."""
        result = render("node.enabled=true", "storage.mode=rclone", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("/storage/mode", result.stderr)

    def test_rclone_config_only_with_secret(self) -> None:
        """Boundary: the config path is set only when the Secret is mounted."""
        self.assertNotIn("VMAFX_RCLONE_CONFIG", node_env())
        env = node_env("storage.rclone.config=[s3]\ntype = s3")
        self.assertEqual(env["VMAFX_RCLONE_CONFIG"], "/etc/vmafx/rclone.conf")


def gpu_limits(name: str, *sets: str) -> dict[str, Any]:
    """Resource limits of the first container of the Deployment called name."""
    for doc in manifests("node.enabled=true", *sets):
        if doc["kind"] == "Deployment" and doc["metadata"]["name"] == name:
            limits: dict[str, Any] = doc["spec"]["template"]["spec"]["containers"][0]["resources"][
                "limits"
            ]
            return limits
    raise AssertionError(f"no Deployment called {name!r}")


class GpuResource(unittest.TestCase):
    """The device-plugin resource the server and node pods request."""

    def assert_requested(self, name: str | None, *sets: str) -> None:
        for workload in ("vmafx", "vmafx-node"):
            with self.subTest(workload=workload):
                limits = gpu_limits(workload, *sets)
                gpu = {k: v for k, v in limits.items() if "/" in k}
                self.assertEqual(list(gpu), [name] if name else [])

    def test_vendor_defaults(self) -> None:
        for vendor, name in (
            ("nvidia", "nvidia.com/gpu"),
            ("amd", "amd.com/gpu"),
            ("intel", "gpu.intel.com/i915"),
        ):
            with self.subTest(vendor=vendor):
                self.assert_requested(name, f"gpu.vendor={vendor}")

    def test_intel_xe_driver(self) -> None:
        """The xe kernel driver's resource, as advertised for Arc B-series GPUs."""
        self.assert_requested("gpu.intel.com/xe", "gpu.vendor=intel", "gpu.intelDriver=xe")

    def test_explicit_resource_name(self) -> None:
        self.assert_requested("nvidia.com/mig-1g.10gb", "gpu.resourceName=nvidia.com/mig-1g.10gb")

    def test_cpu_requests_nothing(self) -> None:
        """Boundary: no device-plugin resource for a CPU deployment."""
        self.assert_requested(None, "gpu.vendor=cpu")

    def test_unknown_driver_refused(self) -> None:
        result = render("gpu.vendor=intel", "gpu.intelDriver=xe2", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("/gpu/intelDriver", result.stderr)

    def test_resource_name_with_cpu_refused(self) -> None:
        """Negative: a resource name on a CPU deployment is a contradiction."""
        result = render("gpu.vendor=cpu", "gpu.resourceName=gpu.intel.com/xe", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("needs a GPU vendor", result.stderr)

    def test_malformed_resource_name_refused(self) -> None:
        result = render("gpu.resourceName=not a resource", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("/gpu/resourceName", result.stderr)


class ControllerToNodePort(unittest.TestCase):
    def test_follows_node_grpc_port(self) -> None:
        """The allow rule opens the port the node listens on (it opened 50051)."""
        doc = policy("-allow-controller-to-node")
        assert doc is not None
        self.assertEqual(doc["spec"]["ingress"][0]["ports"][0]["port"], 50052)
        moved = policy("-allow-controller-to-node", "node.grpcPort=7443")
        assert moved is not None
        self.assertEqual(moved["spec"]["ingress"][0]["ports"][0]["port"], 7443)

    def test_explicit_port_kept(self) -> None:
        doc = policy(
            "-allow-controller-to-node", "networkPolicy.allow.controllerToNode.nodePort=6000"
        )
        assert doc is not None
        self.assertEqual(doc["spec"]["ingress"][0]["ports"][0]["port"], 6000)


if __name__ == "__main__":
    unittest.main()
