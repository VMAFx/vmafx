# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Bounded host, toolchain, accelerator, and vmaf binary discovery."""

from __future__ import annotations

import hashlib
import os
import platform
import re
import shutil
import subprocess
from collections.abc import Callable
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from vmaf_rc1_tester.safe_process import (
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
    run_bounded,
)

SubprocessRunner = Callable[..., Any]
ALL_SUPPORTED_BACKENDS: tuple[str, ...] = ("cpu", "cuda", "sycl", "hip", "metal")
MAX_CAPTURE_BYTES = 1_048_576
COMMAND_TIMEOUT_EXIT = 124
COMMAND_OUTPUT_LIMIT_EXIT = 125
COMMAND_LAUNCH_EXIT = 127


def _is_source_root(path: Path) -> bool:
    return (
        (path / "model" / "vmaf_v0.6.1.json").is_file()
        and (path / "testdata" / "ref_576x324_48f.yuv").is_file()
        and (path / "tools" / "rc1-tester").is_dir()
    )


def _discover_repo_root() -> Path:
    """Locate the source checkout for source and installed entry points."""
    module_path = Path(__file__).resolve()
    seeds = (Path.cwd().resolve(), module_path.parent)
    for seed in seeds:
        candidates = (seed, *tuple(seed.parents)[:12])
        for candidate in candidates:
            if _is_source_root(candidate):
                return candidate
    return module_path.parents[4]


REPO_ROOT = _discover_repo_root()


def _run_cmd(
    command: list[str],
    runner: SubprocessRunner | None = None,
    timeout: float = 5.0,
    environment_overrides: dict[str, str] | None = None,
) -> tuple[int, str, str]:
    """Run argv with process-group timeout and a combined output ceiling."""
    environment = None
    if environment_overrides:
        environment = os.environ.copy()
        environment.update(environment_overrides)
    if runner is None:
        try:
            result = run_bounded(
                command,
                environment=environment,
                timeout_seconds=timeout,
                max_output_bytes=MAX_CAPTURE_BYTES,
            )
        except CommandTimedOut as exc:
            outcome = COMMAND_TIMEOUT_EXIT, str(exc.stdout or "").strip(), str(exc).strip()
        except CommandOutputLimitExceeded as exc:
            outcome = COMMAND_OUTPUT_LIMIT_EXIT, str(exc.stdout or "").strip(), str(exc).strip()
        except (CommandValidationError, OSError) as exc:
            outcome = COMMAND_LAUNCH_EXIT, "", f"command could not run: {exc}"
        else:
            outcome = result.returncode, str(result.stdout).strip(), str(result.stderr).strip()
        return outcome
    try:
        result = runner(
            command,
            capture_output=True,
            text=True,
            check=False,
            env=environment,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        return (
            COMMAND_TIMEOUT_EXIT,
            str(exc.stdout or "").strip(),
            f"command timed out after {timeout:g}s",
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return COMMAND_LAUNCH_EXIT, "", f"command could not run: {exc}"
    return (
        int(getattr(result, "returncode", 1)),
        str(getattr(result, "stdout", "") or "").strip(),
        str(getattr(result, "stderr", "") or "").strip(),
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(65536):
            digest.update(chunk)
    return digest.hexdigest()


@dataclass(frozen=True)
class OSInfo:
    system: str
    release: str
    machine: str
    distro: str
    is_container: bool


def probe_os(runner: SubprocessRunner | None = None) -> OSInfo:
    """Probe platform details without collecting host or user identifiers."""
    system = platform.system()
    distro = system
    if system == "Linux" and Path("/etc/os-release").is_file():
        try:
            for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines()[:30]:
                if line.startswith("PRETTY_NAME="):
                    distro = line.split("=", 1)[1].strip("\"'")
                    break
        except OSError:
            pass
    elif system == "Darwin":
        rc, out, _ = _run_cmd(["sw_vers", "-productVersion"], runner=runner)
        distro = f"macOS {out}" if rc == 0 and out else "macOS"
    in_container = (
        Path("/.dockerenv").exists()
        or Path("/run/.containerenv").exists()
        or bool(os.environ.get("container"))
    )
    return OSInfo(system, platform.release(), platform.machine(), distro, in_container)


@dataclass(frozen=True)
class CPUInfo:
    model: str
    logical_cores: int
    has_avx2: bool
    has_avx512: bool
    has_neon: bool


def _read_linux_cpu_flags() -> tuple[str, str]:
    model = platform.processor() or "Unknown"
    flags = ""
    try:
        lines = Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines()[:160]
    except OSError:
        return model, flags
    for line in lines:
        if line.startswith("model name") and model == "Unknown":
            model = line.split(":", 1)[1].strip()
        elif line.startswith(("flags", "Features")):
            flags = line.split(":", 1)[1].strip().lower()
    return model, flags


def probe_cpu(runner: SubprocessRunner | None = None) -> CPUInfo:
    """Probe the CPU model and instruction families relevant to VMAFx."""
    system = platform.system()
    machine = platform.machine().lower()
    if system == "Linux":
        model, flags = _read_linux_cpu_flags()
    elif system == "Darwin":
        _, model, _ = _run_cmd(["sysctl", "-n", "machdep.cpu.brand_string"], runner=runner)
        _, flags, _ = _run_cmd(["sysctl", "-n", "machdep.cpu.features"], runner=runner)
        model = model or platform.processor() or "Apple Silicon"
        flags = flags.lower()
    else:
        model, flags = platform.processor() or "Unknown", ""
    return CPUInfo(
        model=model,
        logical_cores=os.cpu_count() or 1,
        has_avx2="avx2" in flags,
        has_avx512=any(flag in flags for flag in ("avx512f", "avx512vl", "avx512bw")),
        has_neon="asimd" in flags or "neon" in flags or machine in ("arm64", "aarch64"),
    )


@dataclass(frozen=True)
class ToolchainInfo:
    c_compiler: str
    cxx_compiler: str
    meson: str
    ninja: str
    nvcc: str
    icpx: str
    hipcc: str
    metal_compiler: str


def _get_bin_version(name: str, args: list[str], runner: SubprocessRunner | None) -> str:
    path = shutil.which(name)
    if not path:
        return "not installed"
    rc, out, err = _run_cmd([path, *args], runner=runner)
    version_text = out or err
    if rc == 0 and version_text:
        return version_text.splitlines()[0].strip()
    return f"installed ({path})"


def probe_toolchain(runner: SubprocessRunner | None = None) -> ToolchainInfo:
    """Probe build utilities and vendor compiler frontends."""
    c_compiler = _get_bin_version("clang", ["--version"], runner)
    if c_compiler == "not installed":
        c_compiler = _get_bin_version("gcc", ["--version"], runner)
    cxx_compiler = _get_bin_version("clang++", ["--version"], runner)
    if cxx_compiler == "not installed":
        cxx_compiler = _get_bin_version("g++", ["--version"], runner)
    metal = _get_bin_version("xcrun", ["metal", "--version"], runner)
    return ToolchainInfo(
        c_compiler=c_compiler,
        cxx_compiler=cxx_compiler,
        meson=_get_bin_version("meson", ["--version"], runner),
        ninja=_get_bin_version("ninja", ["--version"], runner),
        nvcc=_get_bin_version("nvcc", ["--version"], runner),
        icpx=_get_bin_version("icpx", ["--version"], runner),
        hipcc=_get_bin_version("hipcc", ["--version"], runner),
        metal_compiler=metal,
    )


@dataclass(frozen=True)
class AcceleratorDevice:
    backend: str
    name: str
    driver_version: str = "unknown/not reported"
    details: str = ""
    ordinal: int | None = None


@dataclass(frozen=True)
class HardwareProbes:
    cuda_devices: list[AcceleratorDevice] = field(default_factory=list)
    sycl_devices: list[AcceleratorDevice] = field(default_factory=list)
    hip_devices: list[AcceleratorDevice] = field(default_factory=list)
    metal_devices: list[AcceleratorDevice] = field(default_factory=list)

    def all_devices(self) -> list[AcceleratorDevice]:
        return self.cuda_devices + self.sycl_devices + self.hip_devices + self.metal_devices


def _probe_cuda_devices(runner: SubprocessRunner | None) -> list[AcceleratorDevice]:
    binary = shutil.which("nvidia-smi")
    if not binary:
        return []
    rc, out, _ = _run_cmd(
        [binary, "--query-gpu=name,driver_version", "--format=csv,noheader"], runner=runner
    )
    if rc != 0 or not out:
        return []
    devices: list[AcceleratorDevice] = []
    for ordinal, line in enumerate(out.splitlines()[:8]):
        name, separator, driver = line.partition(",")
        devices.append(
            AcceleratorDevice(
                "cuda",
                name.strip(),
                driver.strip() if separator and driver.strip() else "unknown/not reported",
                ordinal=ordinal,
            )
        )
    return devices


def _probe_sycl_devices(runner: SubprocessRunner | None) -> list[AcceleratorDevice]:
    binary = shutil.which("sycl-ls")
    if not binary:
        return []
    rc, out, _ = _run_cmd([binary], runner=runner)
    if rc != 0:
        return []
    names = [line.strip() for line in out.splitlines()[:32] if ":gpu" in line.lower()]
    return [AcceleratorDevice("sycl", name, ordinal=ordinal) for ordinal, name in enumerate(names)]


def _parse_rocm_smi_devices(output: str) -> list[AcceleratorDevice]:
    fields: dict[int, dict[str, str]] = {}
    pattern = re.compile(r"GPU\[(\d+)]\s*:\s*(Card Series|GFX Version)\s*:\s*(.+?)\s*$")
    for line in output.splitlines()[:64]:
        if match := pattern.search(line):
            ordinal = int(match.group(1))
            if ordinal < 8:
                fields.setdefault(ordinal, {})[match.group(2)] = match.group(3)
    devices: list[AcceleratorDevice] = []
    for ordinal in sorted(fields):
        name = fields[ordinal].get("Card Series", f"AMD GPU {ordinal}")
        gfx = fields[ordinal].get("GFX Version", "")
        devices.append(
            AcceleratorDevice(
                "hip",
                f"{name} ({gfx})" if gfx else name,
                details=f"runtime index {ordinal}",
                ordinal=ordinal,
            )
        )
    return devices


def _parse_rocminfo_devices(output: str) -> list[AcceleratorDevice]:
    agents: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    for line in output.splitlines()[:512]:
        stripped = line.strip()
        if re.fullmatch(r"Agent\s+\d+", stripped):
            if current is not None:
                agents.append(current)
            current = {}
            continue
        if current is None or ":" not in stripped:
            continue
        key, value = (part.strip() for part in stripped.split(":", 1))
        if key in ("Marketing Name", "Device Type"):
            current[key] = value
    if current is not None:
        agents.append(current)
    gpu_names = [
        agent.get("Marketing Name", "")
        for agent in agents
        if agent.get("Device Type", "").casefold() == "gpu"
    ]
    return [
        AcceleratorDevice("hip", name, details=f"runtime index {ordinal}", ordinal=ordinal)
        for ordinal, name in enumerate(gpu_names[:8])
        if name
    ]


def _probe_hip_devices(runner: SubprocessRunner | None) -> list[AcceleratorDevice]:
    rocm_smi = shutil.which("rocm-smi")
    if rocm_smi:
        rc, out, _ = _run_cmd([rocm_smi, "--showproductname"], runner=runner)
        if rc == 0 and out and (devices := _parse_rocm_smi_devices(out)):
            return devices
    rocminfo = shutil.which("rocminfo")
    if not rocminfo:
        return []
    rc, out, _ = _run_cmd([rocminfo], runner=runner)
    if rc != 0:
        return []
    return _parse_rocminfo_devices(out)


def _probe_metal_devices(runner: SubprocessRunner | None) -> list[AcceleratorDevice]:
    if platform.system() != "Darwin":
        return []
    rc, out, _ = _run_cmd(
        ["system_profiler", "SPDisplaysDataType", "-detailLevel", "mini"],
        runner=runner,
        timeout=8.0,
    )
    names = [
        line.split(":", 1)[1].strip() for line in out.splitlines()[:100] if "Chipset Model:" in line
    ]
    if rc == 0 and names:
        return [
            AcceleratorDevice("metal", name, ordinal=ordinal)
            for ordinal, name in enumerate(names[:8])
        ]
    if platform.machine().lower() in ("arm64", "aarch64"):
        return [AcceleratorDevice("metal", "Apple Silicon GPU", ordinal=0)]
    return []


def probe_accelerators(runner: SubprocessRunner | None = None) -> HardwareProbes:
    """Probe accelerator visibility without collecting serial numbers or UUIDs."""
    return HardwareProbes(
        cuda_devices=_probe_cuda_devices(runner),
        sycl_devices=_probe_sycl_devices(runner),
        hip_devices=_probe_hip_devices(runner),
        metal_devices=_probe_metal_devices(runner),
    )


@dataclass(frozen=True)
class VmafBinaryInfo:
    path: str
    exists: bool
    version: str
    sha256: str
    accepted_backend_selectors: list[str]


def parse_vmaf_backends(help_text: str) -> list[str]:
    """Parse selector names accepted by the CLI, not compiled availability."""
    return [backend for backend in ALL_SUPPORTED_BACKENDS if backend in help_text]


def find_vmaf_binary(custom_path: str | None = None, build_dir: str | None = None) -> str | None:
    """Resolve a runnable vmaf binary from explicit, repo-local, then PATH locations."""
    if custom_path:
        candidate = Path(custom_path).expanduser()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
        return None
    candidates: list[Path] = []
    if build_dir:
        base = Path(build_dir).expanduser()
        candidates.extend(
            (
                base / "tools" / "vmaf",
                base / "tools" / "vmaf.exe",
                base / "core" / "tools" / "vmaf",
                base / "core" / "tools" / "vmaf.exe",
            )
        )
    candidates.extend(
        (
            REPO_ROOT / "build" / "tools" / "vmaf",
            REPO_ROOT / "build" / "tools" / "vmaf.exe",
            REPO_ROOT / "core" / "build" / "tools" / "vmaf",
            REPO_ROOT / "core" / "build" / "tools" / "vmaf.exe",
            REPO_ROOT / "core" / "build-cpu" / "tools" / "vmaf",
            REPO_ROOT / "core" / "build-cpu" / "tools" / "vmaf.exe",
        )
    )
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
    return shutil.which("vmaf") or shutil.which("vmaf.exe")


def probe_vmaf_binary(
    vmaf_path: str | None, runner: SubprocessRunner | None = None
) -> VmafBinaryInfo:
    """Capture binary identity plus the selector vocabulary advertised by its help."""
    if not vmaf_path:
        return VmafBinaryInfo("", False, "", "", [])
    path = Path(vmaf_path)
    if not path.is_file() or not os.access(path, os.X_OK):
        return VmafBinaryInfo(str(path), False, "", "", [])
    rc, out, err = _run_cmd([str(path), "--version"], runner=runner)
    version_text = out or err
    version = version_text.splitlines()[0].strip() if rc == 0 and version_text else "unknown"
    _, help_out, help_err = _run_cmd([str(path), "--help"], runner=runner)
    return VmafBinaryInfo(
        path=str(path.resolve()),
        exists=True,
        version=version,
        sha256=_sha256(path),
        accepted_backend_selectors=parse_vmaf_backends(f"{help_out}\n{help_err}"),
    )


@dataclass(frozen=True)
class DiagnosticReport:
    os: OSInfo
    cpu: CPUInfo
    toolchain: ToolchainInfo
    accelerators: HardwareProbes
    vmaf_binary: VmafBinaryInfo

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def run_full_probe(
    vmaf_path: str | None = None,
    build_dir: str | None = None,
    runner: SubprocessRunner | None = None,
) -> DiagnosticReport:
    """Execute the bounded RC1 environment probe."""
    resolved = find_vmaf_binary(custom_path=vmaf_path, build_dir=build_dir)
    return DiagnosticReport(
        os=probe_os(runner=runner),
        cpu=probe_cpu(runner=runner),
        toolchain=probe_toolchain(runner=runner),
        accelerators=probe_accelerators(runner=runner),
        vmaf_binary=probe_vmaf_binary(resolved, runner=runner),
    )
