#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""node.fuse and node.ebpf of the Helm chart, checked on real renders.

ADR-1593: node.fuse gives the node pods /dev/fuse through a device plugin's
extended resource and the capability bounding set the setuid fusermount3
mounts with; node.ebpf turns on the eBPF descriptor tracker (VMAFX_EBPF_BYPASS)
with UID 0, BPF / PERFMON / SYS_ADMIN and the host's tracefs read-only. The
chart refuses the combinations the node would stop on. Positive, negative and
boundary renders; the helm-chart workflow runs this file and needs ``helm`` on
PATH.
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
HELM_TIMEOUT_S = 60
FUSE = """
node:
  enabled: true
  fuse: {enabled: true, resourceName: devic.es/fuse}
storage:
  mode: mount
"""
EBPF = """
node:
  enabled: true
  fuse: {enabled: true, resourceName: devic.es/fuse}
  ebpf: {enabled: true}
storage:
  mode: mount
"""


def helm(values: str) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", suffix=".yaml") as fh:
        fh.write(values)
        fh.flush()
        cmd = ["helm", "template", "vmafx", str(CHART), "--namespace", "vmafx", "-f", fh.name]
        return subprocess.run(  # noqa: S603 -- fixed helm executable; argv built here
            cmd, capture_output=True, text=True, check=False, timeout=HELM_TIMEOUT_S
        )


def node(values: str) -> dict[str, Any]:
    """The rendered vmafx-node Deployment's pod spec."""
    result = helm(values)
    if result.returncode != 0:
        raise AssertionError(f"helm template failed: {result.stderr}")
    docs = [d for d in yaml.safe_load_all(result.stdout) if d]
    deploy = next(
        d for d in docs if d["kind"] == "Deployment" and d["metadata"]["name"] == "vmafx-node"
    )
    spec: dict[str, Any] = deploy["spec"]["template"]["spec"]
    return spec


def container(spec: dict[str, Any]) -> dict[str, Any]:
    c: dict[str, Any] = spec["containers"][0]
    return c


def env(spec: dict[str, Any]) -> dict[str, str]:
    return {e["name"]: e.get("value", "") for e in container(spec).get("env", [])}


def volumes(spec: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {v["name"]: v for v in spec.get("volumes", [])}


def mounts(spec: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {m["name"]: m for m in container(spec).get("volumeMounts", [])}


class HelmNodeFuseEbpfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("helm") is None:
            raise AssertionError("helm is not on PATH; this test renders the chart")

    def refuses(self, values: str, message: str) -> None:
        result = helm(values)
        self.assertNotEqual(result.returncode, 0, f"rendered although {message!r} was expected")
        self.assertIn(message, result.stderr)

    def test_default_node_keeps_the_restricted_context(self) -> None:
        spec = node("node: {enabled: true}\n")
        sc = container(spec)["securityContext"]
        self.assertEqual(sc["capabilities"], {"drop": ["ALL"]})
        self.assertFalse(sc["allowPrivilegeEscalation"])
        self.assertTrue(sc["runAsNonRoot"])
        self.assertNotIn("devic.es/fuse", container(spec)["resources"]["limits"])
        self.assertFalse([k for k in env(spec) if k.startswith("VMAFX_EBPF_")])
        self.assertNotIn("tracefs", volumes(spec))
        self.assertNotIn("mount-root", volumes(spec))

    def test_fuse_requests_the_device_and_the_helper_capabilities(self) -> None:
        spec = node(FUSE)
        c = container(spec)
        self.assertEqual(c["resources"]["limits"]["devic.es/fuse"], 1)
        sc = c["securityContext"]
        self.assertEqual(
            sc["capabilities"], {"drop": ["ALL"], "add": ["SYS_ADMIN", "DAC_READ_SEARCH"]}
        )
        self.assertTrue(sc["allowPrivilegeEscalation"])
        self.assertEqual(sc["runAsUser"], 65532)
        self.assertTrue(sc["runAsNonRoot"])
        self.assertTrue(sc["readOnlyRootFilesystem"])
        self.assertNotIn("appArmorProfile", sc)
        self.assertEqual(env(spec)["VMAFX_STORAGE_MODE"], "mount")
        self.assertNotIn("VMAFX_STORAGE_MOUNT_ROOT", env(spec))
        self.assertNotIn("VMAFX_EBPF_BYPASS", env(spec))
        self.assertNotIn("tracefs", volumes(spec))
        unconfined = "devic.es/fuse, appArmorProfile: {type: Unconfined}}"
        spec = node(FUSE.replace("devic.es/fuse}", unconfined))
        self.assertEqual(
            container(spec)["securityContext"]["appArmorProfile"], {"type": "Unconfined"}
        )

    def test_ebpf_runs_the_tracker_as_root_with_its_capabilities(self) -> None:
        spec = node(EBPF)
        e = env(spec)
        self.assertEqual(e["VMAFX_EBPF_BYPASS"], "1")
        self.assertEqual(e["VMAFX_EBPF_MOUNT_PREFIX"], "/rclone-mount/")
        self.assertEqual(e["VMAFX_STORAGE_MOUNT_ROOT"], "/rclone-mount")
        sc = container(spec)["securityContext"]
        self.assertEqual(
            sc["capabilities"], {"drop": ["ALL"], "add": ["BPF", "PERFMON", "SYS_ADMIN"]}
        )
        self.assertEqual(sc["runAsUser"], 0)
        self.assertFalse(sc["runAsNonRoot"])
        self.assertEqual(
            volumes(spec)["tracefs"]["hostPath"],
            {"path": "/sys/kernel/tracing", "type": "Directory"},
        )
        self.assertEqual(
            mounts(spec)["tracefs"],
            {"name": "tracefs", "mountPath": "/sys/kernel/tracing", "readOnly": True},
        )
        self.assertEqual(volumes(spec)["mount-root"], {"name": "mount-root", "emptyDir": {}})
        self.assertEqual(mounts(spec)["mount-root"]["mountPath"], "/rclone-mount")

    def test_mount_root_volume_only_outside_tmp(self) -> None:
        spec = node(FUSE + "  mountRoot: /tmp/jobs\n")
        pod_tmp_root = "/tmp/jobs"  # noqa: S108 -- a path inside the pod, not a host temp file
        self.assertEqual(env(spec)["VMAFX_STORAGE_MOUNT_ROOT"], pod_tmp_root)
        self.assertNotIn("mount-root", volumes(spec))
        spec = node(FUSE + "  mountRoot: /mnt/jobs\n")
        self.assertEqual(mounts(spec)["mount-root"]["mountPath"], "/mnt/jobs")
        spec = node(
            EBPF.replace("ebpf: {enabled: true}", "ebpf: {enabled: true, mountPrefix: /tmp/m/}")
        )
        pod_tmp_prefix_root = "/tmp/m"  # noqa: S108 -- a path inside the pod, not a host temp file
        self.assertEqual(env(spec)["VMAFX_STORAGE_MOUNT_ROOT"], pod_tmp_prefix_root)
        self.assertNotIn("mount-root", volumes(spec))

    def test_refused_combinations(self) -> None:
        self.refuses(
            "node: {enabled: true}\nstorage: {mode: mount}\n", "storage.mode mount needs FUSE"
        )
        self.refuses(FUSE.replace("devic.es/fuse", '""'), "node.fuse.resourceName is empty")
        self.refuses(
            EBPF.replace("mode: mount", "mode: http-serve"), "node.ebpf needs storage.mode mount"
        )
        self.refuses(
            EBPF.replace("mode: mount", "mode: auto"), "node.ebpf needs storage.mode mount"
        )
        self.refuses(
            "node: {enabled: true, ebpf: {enabled: true}}\nstorage: {mode: mount}\n",
            "storage.mode mount needs FUSE",
        )
        self.refuses(EBPF + "  mountRoot: /data/jobs\n", "is not under node.ebpf.mountPrefix")
        self.refuses(EBPF + "env: {VMAFX_EBPF_BYPASS: '0'}\n", "env.VMAFX_EBPF_BYPASS")
        self.refuses(FUSE + "env: {VMAFX_STORAGE_MODE: http-serve}\n", "env.VMAFX_STORAGE_MODE")
        self.refuses(
            "node: {fuse: {enabled: true, resourceName: x/fuse}}\n", "set node.enabled: true"
        )
        # The schema refuses a relative or over-long prefix before the template.
        self.assertNotEqual(
            helm(
                EBPF.replace("ebpf: {enabled: true}", "ebpf: {enabled: true, mountPrefix: m/}")
            ).returncode,
            0,
        )

    def test_boundaries(self) -> None:
        # auto with FUSE renders (the node picks mount when it can); env
        # VMAFX_STORAGE_MODE stays allowed without node.fuse.
        self.assertEqual(
            env(node(FUSE.replace("mode: mount", "mode: auto")))["VMAFX_STORAGE_MODE"], "auto"
        )
        node("node: {enabled: true}\nenv: {VMAFX_STORAGE_MODE: http-serve}\n")
        prefix = "/" + "p" * 253 + "/"
        self.assertEqual(len(prefix), 255)
        spec = node(
            EBPF.replace("ebpf: {enabled: true}", f"ebpf: {{enabled: true, mountPrefix: {prefix}}}")
        )
        self.assertEqual(env(spec)["VMAFX_EBPF_MOUNT_PREFIX"], prefix)
        longer = "/" + "p" * 254 + "/"
        self.assertNotEqual(
            helm(
                EBPF.replace(
                    "ebpf: {enabled: true}", f"ebpf: {{enabled: true, mountPrefix: {longer}}}"
                )
            ).returncode,
            0,
        )
        # The mount root may equal the prefix with or without its slash.
        node(EBPF + "  mountRoot: /rclone-mount/\n")
        self.refuses(EBPF + "  mountRoot: /rclone-mountx\n", "is not under node.ebpf.mountPrefix")


if __name__ == "__main__":
    unittest.main()
