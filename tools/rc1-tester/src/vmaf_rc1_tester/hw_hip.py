# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The HIP backend of the GPU section (the AMD GPU tester image).

How the image reaches an AMD GPU, recorded as facts and as one `path`:

- `kfd`: the ROCm compute node `/dev/kfd` and a render node (`/dev/dri/renderD*`)
  of PCI vendor 0x1002, both readable and writable by this process;
- `none`: either missing, with the reason and the `docker run` option that fixes
  it. WSL2's `/dev/dxg` is recorded, but the image's ROCm runtime drives
  `/dev/kfd` only: AMD's WSL2 support needs its own runtime, not in the image.

The devices are the GPU agents of the image's HSA runtime (hw_hipprobe.py, its
own bounded process); every run of a device is pinned with
`ROCR_VISIBLE_DEVICES=<n>` in the same order. A device whose gfx target has no
code object in the build (image/hip-targets.json, written from the build's
offload targets) is listed with the reason and not run. HIP has no audit test.
"""

from __future__ import annotations

import json
import os
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from .hw_equiv import Runner
from .hw_gpu import GpuBackend
from .hw_sycl import node_facts

AMD_VENDOR = "0x1002"
KFD = Path("/dev/kfd")
DRI = Path("/dev/dri")
DXG = Path("/dev/dxg")
SYSFS_DRM = Path("/sys/class/drm")
PROBE_TIMEOUT_SECONDS = 120.0
DEVICE_HINT = "add --device /dev/kfd --device /dev/dri"


def kfd_facts(kfd: Path = KFD) -> dict[str, Any]:
    """The compute node: present, its group, and whether this process can open it."""
    try:
        info = kfd.stat()
    except OSError:
        return {"present": False, "accessible": False, "gid": None}
    return {"present": True, "gid": info.st_gid,
            "accessible": os.access(kfd, os.R_OK | os.W_OK)}  # fmt: skip


def access_facts(
    kfd: Path = KFD, dri: Path = DRI, dxg: Path = DXG, sysfs: Path = SYSFS_DRM
) -> dict[str, Any]:
    """How the container can reach an AMD GPU, and the path a run takes."""
    nodes = [node_facts(node, sysfs) for node in sorted(dri.glob("renderD*"))]
    amd = [node for node in nodes if node["vendor"] == AMD_VENDOR]
    facts: dict[str, Any] = {
        "kfd": kfd_facts(kfd),
        "render_nodes": nodes,
        "dxg_present": dxg.exists(),
        "groups": sorted(os.getgroups()),
    }
    usable = facts["kfd"]["accessible"] and any(node["accessible"] for node in amd)
    facts["path"] = "kfd" if usable else "none"
    return facts


def group_options(gids: Sequence[Any]) -> str:
    return " ".join(f"--group-add {gid}" for gid in sorted({str(g) for g in gids if g is not None}))


def missing_access_reason(facts: Mapping[str, Any]) -> str:
    """Why no AMD GPU is reachable, with the `docker run` option that fixes it."""
    kfd = facts["kfd"]
    amd = [n for n in facts["render_nodes"] if n["vendor"] == AMD_VENDOR]
    if not kfd["present"] and facts["dxg_present"]:
        return ("/dev/dxg is present (WSL2) but /dev/kfd is not: this image's ROCm runtime "
                "runs on Linux with the amdgpu driver only")  # fmt: skip
    if not kfd["present"]:
        return f"no /dev/kfd is visible: {DEVICE_HINT} (Linux, amdgpu driver)"
    closed = [n["gid"] for n in amd if not n["accessible"]]
    if not kfd["accessible"] or (amd and len(closed) == len(amd)):
        gids = ([kfd["gid"]] if not kfd["accessible"] else []) + closed
        return ("/dev/kfd or the AMD render node is not readable by this container's user: add "
                + group_options(gids))  # fmt: skip
    return "no render node of an AMD GPU (PCI vendor 0x1002) is visible: add --device /dev/dri"


def probe_reason(probe: Mapping[str, Any]) -> str:
    """Why the runtime reports no device although the device nodes are open."""
    return (f"the ROCm runtime reports no GPU ({probe.get('status')}): "
            f"{probe.get('error', '')}").rstrip(": ")  # fmt: skip


def load_targets(path: Path) -> dict[str, Any]:
    """image/hip-targets.json: the build's gfx targets and ROCm version, {} when absent."""
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return document if isinstance(document, dict) else {}


def probe_devices(runner: Runner) -> dict[str, Any]:
    """The HSA probe in its own process; `error` when it does not finish."""
    src = str(Path(__file__).resolve().parents[1])
    env = {**os.environ, "PYTHONPATH": src}
    try:
        result = runner(
            [sys.executable, "-B", "-m", "vmaf_rc1_tester.hw_hipprobe"], environment=env,
            timeout_seconds=PROBE_TIMEOUT_SECONDS, max_output_bytes=262_144,
        )  # fmt: skip
        document = json.loads(result.stdout)
    except (TimeoutError, RuntimeError, ValueError, OSError) as error:
        return {"status": "error", "error": str(error)[:200], "devices": []}
    return document if isinstance(document, dict) else {"status": "error", "devices": []}


def split_devices(
    probed: Sequence[Mapping[str, Any]], targets: Sequence[str]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """(devices to run, devices left out with the reason)."""
    usable: list[dict[str, Any]] = []
    left_out: list[dict[str, Any]] = []
    for device in probed:
        if device.get("gfx_target") in targets:
            usable.append({"index": int(device["index"]), "facts": dict(device)})
        else:
            left_out.append({**device, "reason": f"the image has no code object for "
                             f"{device.get('gfx_target')} (it has {', '.join(targets) or 'none'})"})  # fmt: skip
    return usable, left_out


def discover(root: Path, runner: Runner) -> dict[str, Any]:
    """Access facts, runtime versions and the usable AMD GPUs of this host."""
    facts = access_facts()
    targets = load_targets(root / "image" / "hip-targets.json")
    probe = probe_devices(runner) if facts["path"] != "none" else {"status": "not_run",
                                                                   "devices": []}  # fmt: skip
    facts["hsa_runtime"] = {k: v for k, v in probe.items() if k != "devices"}
    devices, left_out = split_devices(probe.get("devices", []), targets.get("targets", []))
    if left_out:
        facts["devices_left_out"] = left_out
    runtime = {"rocm_version": str(targets.get("rocm_version", "unknown")),
               "hsa_runtime_version": str(probe.get("hsa_runtime_version", "unknown"))}  # fmt: skip
    found: dict[str, Any] = {"access": facts, "runtime": runtime, "devices": devices}
    if not devices:
        if facts["path"] == "none":
            found["reason"] = missing_access_reason(facts)
        elif left_out:
            found["reason"] = f"no AMD GPU this build runs on: {left_out[0]['reason']}"
        else:
            found["reason"] = probe_reason(probe)
    return found


def device_env(device: Mapping[str, Any]) -> dict[str, str]:
    """Pins every run to one GPU agent, in the probe's order."""
    return {"ROCR_VISIBLE_DEVICES": str(int(device["index"]))}


HIP = GpuBackend(
    name="hip",
    discover=discover,
    device_env=device_env,
    audits={},
    row_map="hip-rows.json",
)
