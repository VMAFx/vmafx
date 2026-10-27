# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Host facts for the tester image report.

Nothing here reads a host name, user name, serial number, MAC address or
network address. `/proc/cpuinfo` is read through a key allow-list.
"""

from __future__ import annotations

import ctypes
import json
import os
import platform
from pathlib import Path
from typing import Any

from .hw_winfacts import collect_windows_facts, windows_machine
from .probe import probe_os
from .safe_process import run_bounded

AT_HWCAP = 16
AT_HWCAP2 = 26
# core/src/arm/cpu.c: NEON on every aarch64 host, SVE2 when AT_HWCAP2 bit 1 is set.
HWCAP2_SVE2 = 1 << 1
HWCAP_SVE = 1 << 22
MAX_CPUINFO_LINES = 4096
CPUINFO_KEYS = frozenset(
    {
        "model name",
        "vendor_id",
        "cpu family",
        "model",
        "stepping",
        "flags",
        "features",
        "cpu implementer",
        "cpu architecture",
        "cpu variant",
        "cpu part",
        "cpu revision",
    }
)
# macOS: the only `sysctl` keys read, and the only `system_profiler` fields kept.
DARWIN_SYSCTL_KEYS = (
    "machdep.cpu.brand_string",
    "hw.model",
    "kern.osproductversion",
    "kern.osversion",
)
GPU_FIELDS = ("sppci_model", "sppci_cores", "spdisplays_mtlgpufamilysupport", "spdisplays_vendor")
ALLOWED_CPUINFO_KEYS = CPUINFO_KEYS | frozenset(DARWIN_SYSCTL_KEYS)
X86_FLAG_SOURCES = {
    "sse2": ("sse2",),
    "ssse3": ("ssse3",),
    "sse4.1": ("sse4_1",),
    # core/src/x86/cpu.c: the AVX2 level also needs FMA and BMI1/BMI2 (ADR-2055); the
    # AVX-512 levels chain after it, and ICL adds IFMA and the ecx 0x5f42 set.
    "avx2": ("avx2", "fma", "bmi1", "bmi2"),
    "avx512": (
        "avx2",
        "fma",
        "bmi1",
        "bmi2",
        "avx512f",
        "avx512bw",
        "avx512vl",
        "avx512dq",
        "avx512cd",
    ),
    "avx512icl": (
        "avx2",
        "fma",
        "bmi1",
        "bmi2",
        "avx512f",
        "avx512bw",
        "avx512vl",
        "avx512dq",
        "avx512cd",
        "avx512ifma",
        "avx512vbmi",
        "avx512_vbmi2",
        "gfni",
        "vaes",
        "vpclmulqdq",
        "avx512_vnni",
        "avx512_bitalg",
        "avx512_vpopcntdq",
    ),
}


def read_cpuinfo(path: str = "/proc/cpuinfo") -> dict[str, str]:
    """Return the first value of each allow-listed `/proc/cpuinfo` key."""
    found: dict[str, str] = {}
    try:
        lines = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return found
    for line in lines[:MAX_CPUINFO_LINES]:
        key, sep, value = line.partition(":")
        key = key.strip().lower()
        if sep and key in CPUINFO_KEYS and key not in found:
            found[key] = value.strip()
    return found


def read_hwcaps() -> dict[str, int | None]:
    """AT_HWCAP and AT_HWCAP2 as the libc of this process reports them."""
    try:
        libc = ctypes.CDLL(None)
        getauxval = libc.getauxval
        getauxval.restype = ctypes.c_ulong
        getauxval.argtypes = [ctypes.c_ulong]
        return {"hwcap": int(getauxval(AT_HWCAP)), "hwcap2": int(getauxval(AT_HWCAP2))}
    except (OSError, AttributeError):
        return {"hwcap": None, "hwcap2": None}


def arm_dispatch_flags(machine: str, hwcap2: int | None) -> list[str]:
    """The flags `vmaf_get_cpu_flags_arm()` sets: NEON always, SVE2 on HWCAP2 bit 1."""
    if machine not in ("aarch64", "arm64"):
        return []
    flags = ["neon"]
    if hwcap2 is not None and hwcap2 & HWCAP2_SVE2:
        flags.append("sve2")
    return flags


def x86_dispatch_flags(features: set[str]) -> list[str]:
    """Approximation of `vmaf_get_cpu_flags_x86()` from the kernel's flag list."""
    return [
        name
        for name, needed in X86_FLAG_SOURCES.items()
        if all(flag in features for flag in needed)
    ]


def dispatch_flags(machine: str, cpuinfo: dict[str, str], hwcap2: int | None) -> list[str]:
    """Dispatch flags the fork's runtime selects on this host."""
    if machine in ("aarch64", "arm64"):
        return arm_dispatch_flags(machine, hwcap2)
    if machine in ("x86_64", "amd64"):
        features = set(cpuinfo.get("flags", "").lower().split())
        return x86_dispatch_flags(features)
    return []


def cpu_model_string(cpuinfo: dict[str, str]) -> str:
    """A model string; arm64 kernels give no brand, so fall back to implementer/part."""
    if cpuinfo.get("model name"):
        return cpuinfo["model name"]
    parts = [
        f"{label} {cpuinfo[key]}"
        for key, label in (
            ("cpu implementer", "implementer"),
            ("cpu part", "part"),
            ("cpu variant", "variant"),
            ("cpu revision", "revision"),
        )
        if key in cpuinfo
    ]
    return ", ".join(parts) if parts else (platform.processor() or "unknown")


def read_build_info(path: str) -> dict[str, Any]:
    """The facts baked into the image at build time; empty when run outside the image."""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def _tool(argv: list[str], runner: Any = run_bounded, timeout: float = 60.0) -> str:
    """Stdout of a bounded system tool, empty on any failure."""
    try:
        result = runner(argv, timeout_seconds=timeout, max_output_bytes=1_048_576)
    except (TimeoutError, RuntimeError, ValueError, OSError):
        return ""
    return result.stdout if result.returncode == 0 else ""


def parse_hw_optional(text: str) -> list[str]:
    """Names of the `hw.optional.*` sysctl entries that are set to 1."""
    names = []
    for line in text.splitlines():
        key, sep, value = line.partition(":")
        if sep and key.startswith("hw.optional.") and value.strip() == "1":
            names.append(key[len("hw.optional.") :])
    return sorted(names)


def parse_metal_device(text: str) -> dict[str, str] | None:
    """The first GPU of `system_profiler SPDisplaysDataType -json`, allow-listed fields only."""
    try:
        displays = json.loads(text)["SPDisplaysDataType"]
        first = displays[0]
    except (ValueError, KeyError, IndexError, TypeError):
        return None
    if not isinstance(first, dict):
        return None
    return {key: str(first[key]) for key in GPU_FIELDS if key in first}


def collect_darwin_facts(runner: Any = run_bounded) -> dict[str, Any]:
    """Host facts of a Mac from `sysctl` and `system_profiler`; no identifiers."""
    sysctl = "/usr/sbin/sysctl"
    values = {k: _tool([sysctl, "-n", k], runner).strip() for k in DARWIN_SYSCTL_KEYS}
    values = {key: value for key, value in values.items() if value}
    machine = platform.machine().lower()
    features = parse_hw_optional(_tool([sysctl, "hw.optional"], runner))
    gpu = _tool(["/usr/sbin/system_profiler", "SPDisplaysDataType", "-json"], runner)
    return {
        "platform": "darwin",
        "machine": machine,
        "kernel_release": platform.release(),
        "in_container": False,
        "cpu_model": values.get("machdep.cpu.brand_string", "unknown"),
        "cpuinfo": {
            key: value for key, value in values.items() if key.startswith(("machdep", "hw."))
        },
        "cpu_features": features,
        "hwcap": None,
        "hwcap2": None,
        "sve_in_hwcap": False,
        "dispatch_flags": arm_dispatch_flags(machine, None),
        "dispatch_flags_source": "mirror of core/src/arm/cpu.c: NEON on aarch64, SVE2 only via Linux AT_HWCAP2",
        "logical_cores": os.cpu_count(),
        "os_version": values.get("kern.osproductversion"),
        "hw_model": values.get("hw.model"),
        "metal_device": parse_metal_device(gpu),
    }


def collect_host_facts(cpuinfo_path: str = "/proc/cpuinfo", runner: Any = run_bounded) -> dict:
    """Host facts block of the report, in fixed key order."""
    if platform.system() == "Darwin":
        return collect_darwin_facts(runner)
    if platform.system() == "Windows":
        return collect_windows_facts()
    return collect_linux_facts(cpuinfo_path)


def machine_name() -> str:
    """The architecture name reports and reference files use: `platform.machine()`,
    with Windows' `AMD64` / `ARM64` given their Linux names (x86_64, aarch64)."""
    if platform.system() == "Windows":
        return windows_machine(platform.machine())
    return platform.machine().lower()


def vmaf_binary(root: Path) -> Path:
    """The package's `vmaf` program: `build/tools/vmaf.exe` in the Windows zip,
    `build/tools/vmaf` everywhere else."""
    windows = root / "build" / "tools" / "vmaf.exe"
    return windows if windows.is_file() else root / "build" / "tools" / "vmaf"


def collect_linux_facts(cpuinfo_path: str) -> dict[str, Any]:
    """Host facts block of the report, in fixed key order."""
    machine = platform.machine().lower()
    cpuinfo = read_cpuinfo(cpuinfo_path)
    caps = read_hwcaps() if machine in ("aarch64", "arm64") else {"hwcap": None, "hwcap2": None}
    os_info = probe_os()
    feature_key = "features" if "features" in cpuinfo else "flags"
    return {
        "platform": "linux",
        "machine": machine,
        "kernel_release": os_info.release,
        "in_container": os_info.is_container,
        "cpu_model": cpu_model_string(cpuinfo),
        "cpuinfo": {key: cpuinfo[key] for key in sorted(cpuinfo) if key != feature_key},
        "cpu_features": sorted(cpuinfo.get(feature_key, "").lower().split()),
        "hwcap": caps["hwcap"],
        "hwcap2": caps["hwcap2"],
        "sve_in_hwcap": bool(caps["hwcap"] is not None and caps["hwcap"] & HWCAP_SVE),
        "dispatch_flags": dispatch_flags(machine, cpuinfo, caps["hwcap2"]),
        "dispatch_flags_source": "mirror of core/src/arm/cpu.c and core/src/x86/cpu.c rules",
        "logical_cores": os.cpu_count(),
    }
