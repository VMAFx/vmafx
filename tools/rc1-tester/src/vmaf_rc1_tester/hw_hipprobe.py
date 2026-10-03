# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""AMD GPUs as the ROCm HSA runtime reports them (the AMD GPU tester image).

Runs as its own process, `python3 -m vmaf_rc1_tester.hw_hipprobe`, under the
caller's time limit: a broken driver can hang `hsa_init`. It loads the HSA
runtime libvmaf's HIP runtime loads (`libhsa-runtime64.so.1`, shipped in the
image) and prints one JSON document with, per GPU agent, its gfx target and
family, product name, compute units, maximum clock and PCI device ID, plus the
runtime's version. It reads no UUID and no PCI bus address: `HSA_AMD_AGENT_INFO_UUID`
is never asked for.

The order is the runtime's order of GPU agents, the order `ROCR_VISIBLE_DEVICES=<n>`
selects. The attribute numbers are those of `hsa/hsa.h` and `hsa/hsa_ext_amd.h`;
`hsa_agent_t` is a struct of one `uint64_t`, passed and received as that integer
(the same registers under the x86-64 SysV ABI).
"""

from __future__ import annotations

import ctypes
import json
import re
import sys
from typing import Any

RUNTIME = "libhsa-runtime64.so.1"
SYSTEM_INFO_VERSION_MAJOR = 0
SYSTEM_INFO_VERSION_MINOR = 1
AGENT_INFO_NAME = 0
AGENT_INFO_DEVICE = 17
DEVICE_TYPE_GPU = 1
AMD_AGENT_INFO_CHIP_ID = 0xA000
AMD_AGENT_INFO_COMPUTE_UNIT_COUNT = 0xA002
AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY = 0xA003
AMD_AGENT_INFO_PRODUCT_NAME = 0xA009
NAME_BYTES = 64
MAX_AGENTS = 32
GFX = re.compile(r"^gfx([0-9a-f]+)$")
# gfx target prefix -> family (AMD's architecture names). gfx115x is RDNA 3.5.
FAMILIES = (
    ("gfx908", "cdna1"),
    ("gfx90a", "cdna2"),
    ("gfx942", "cdna3"),
    ("gfx950", "cdna4"),
    ("gfx101", "rdna1"),
    ("gfx103", "rdna2"),
    ("gfx110", "rdna3"),
    ("gfx115", "rdna3.5"),
    ("gfx120", "rdna4"),
)
FAMILY_NAMES = tuple(name for _, name in FAMILIES)
AGENT_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p)


def family_of(target: str) -> str:
    """The architecture family of a gfx target, or `unknown`."""
    if GFX.match(target) is None:
        return "unknown"
    return next((name for prefix, name in FAMILIES if target.startswith(prefix)), "unknown")


def status_name(lib: Any, status: int) -> str:
    """The runtime's own text for a status code."""
    text = ctypes.c_char_p()
    if lib.hsa_status_string(status, ctypes.byref(text)) != 0 or not text.value:
        return f"status 0x{status:x}"
    return text.value.decode("utf-8", errors="replace").split("\n")[0][:120]


def agent_string(lib: Any, agent: int, attribute: int) -> str:
    buffer = ctypes.create_string_buffer(NAME_BYTES)
    if lib.hsa_agent_get_info(agent, attribute, buffer) != 0:
        return "unknown"
    return buffer.value.decode("utf-8", errors="replace").strip() or "unknown"


def agent_uint(lib: Any, agent: int, attribute: int) -> int:
    value = ctypes.c_uint32(0)
    if lib.hsa_agent_get_info(agent, attribute, ctypes.byref(value)) != 0:
        return -1
    return int(value.value)


def describe_agent(lib: Any, agent: int) -> dict[str, Any]:
    """The report's facts about one GPU agent; no UUID, no bus address."""
    target = agent_string(lib, agent, AGENT_INFO_NAME)
    return {
        "name": agent_string(lib, agent, AMD_AGENT_INFO_PRODUCT_NAME),
        "gfx_target": target,
        "family": family_of(target),
        "device_id": f"0x{max(agent_uint(lib, agent, AMD_AGENT_INFO_CHIP_ID), 0):04x}",
        "compute_units": agent_uint(lib, agent, AMD_AGENT_INFO_COMPUTE_UNIT_COUNT),
        "max_clock_mhz": agent_uint(lib, agent, AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY),
    }


def gpu_agents(lib: Any) -> list[int]:
    """Handles of the GPU agents in the runtime's order (at most MAX_AGENTS)."""
    found: list[int] = []

    def visit(agent: int, _data: Any) -> int:
        if len(found) < MAX_AGENTS and agent_uint(lib, agent, AGENT_INFO_DEVICE) == DEVICE_TYPE_GPU:
            found.append(int(agent))
        return 0

    callback = AGENT_CALLBACK(visit)
    lib.hsa_iterate_agents(callback, None)
    return found


def runtime_version(lib: Any) -> str:
    major, minor = ctypes.c_uint16(0), ctypes.c_uint16(0)
    if (lib.hsa_system_get_info(SYSTEM_INFO_VERSION_MAJOR, ctypes.byref(major)) != 0
            or lib.hsa_system_get_info(SYSTEM_INFO_VERSION_MINOR, ctypes.byref(minor)) != 0):  # fmt: skip
        return "unknown"
    return f"{major.value}.{minor.value}"


def load(runtime: str) -> Any:
    lib = ctypes.CDLL(runtime)
    lib.hsa_agent_get_info.argtypes = [ctypes.c_uint64, ctypes.c_int, ctypes.c_void_p]
    lib.hsa_iterate_agents.argtypes = [AGENT_CALLBACK, ctypes.c_void_p]
    return lib


def probe(runtime: str = RUNTIME) -> dict[str, Any]:
    """Every GPU agent in the runtime's order: the order `ROCR_VISIBLE_DEVICES` uses."""
    try:
        lib = load(runtime)
    except OSError as error:
        return {"status": "no_runtime_library", "error": str(error)[:200], "devices": []}
    status = lib.hsa_init()
    if status != 0:
        return {"status": "init_failed", "error": f"hsa_init: {status_name(lib, status)}",
                "devices": []}  # fmt: skip
    devices = []
    for index, agent in enumerate(gpu_agents(lib)):
        facts = describe_agent(lib, agent)
        facts["index"] = index
        devices.append(facts)
    version = runtime_version(lib)
    lib.hsa_shut_down()
    return {"status": "ok" if devices else "no_device", "hsa_runtime_version": version,
            "devices": devices}  # fmt: skip


def main() -> int:
    print(json.dumps(probe(), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
