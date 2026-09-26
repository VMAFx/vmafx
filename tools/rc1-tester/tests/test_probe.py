# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for bounded environment and binary discovery."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from unittest import mock

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester.probe import (
    MAX_CAPTURE_BYTES,
    DiagnosticReport,
    _parse_rocminfo_devices,
    _run_cmd,
    find_vmaf_binary,
    parse_vmaf_backends,
    probe_accelerators,
    probe_cpu,
    probe_os,
    probe_toolchain,
    probe_vmaf_binary,
    run_full_probe,
)
from vmaf_rc1_tester.safe_process import CommandOutputLimitExceeded

COMMAND_TIMEOUT_EXIT = 124
COMMAND_OUTPUT_LIMIT_EXIT = 125
SHA256_HEX_LENGTH = 64


class FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _program(command: list[str]) -> str:
    return Path(command[0]).name


def test_parse_vmaf_backends_is_selector_vocabulary() -> None:
    help_text = "--backend $name: auto|cpu|cuda|sycl|hip|metal.\n"
    assert parse_vmaf_backends(help_text) == ["cpu", "cuda", "sycl", "hip", "metal"]
    assert parse_vmaf_backends("Usage: vmaf [options]") == []


def test_run_cmd_surfaces_timeout() -> None:
    def timeout_runner(*_args: object, **_kwargs: object) -> FakeCompleted:
        raise subprocess.TimeoutExpired(["tool"], 1)

    rc, _out, err = _run_cmd(["tool"], runner=timeout_runner, timeout=1)
    assert rc == COMMAND_TIMEOUT_EXIT
    assert "timed out" in err


def test_run_cmd_enforces_combined_output_limit() -> None:
    overflow = CommandOutputLimitExceeded(MAX_CAPTURE_BYTES, "bounded stdout", "bounded stderr")
    with mock.patch("vmaf_rc1_tester.probe.run_bounded", side_effect=overflow) as run:
        rc, out, err = _run_cmd(["tool"])
    assert rc == COMMAND_OUTPUT_LIMIT_EXIT
    assert out == "bounded stdout"
    assert "captured bytes" in err
    assert run.call_args.kwargs["max_output_bytes"] == MAX_CAPTURE_BYTES


def test_probe_os_and_cpu_return_bounded_types() -> None:
    info = probe_os(runner=lambda *_args, **_kwargs: FakeCompleted(1))
    cpu = probe_cpu(runner=lambda *_args, **_kwargs: FakeCompleted(1))
    assert info.system in ("Linux", "Darwin", "Windows")
    assert isinstance(info.is_container, bool)
    assert cpu.logical_cores >= 1
    assert isinstance(cpu.has_avx2, bool)


def test_probe_toolchain_reads_version_output() -> None:
    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        name = _program(command)
        if name in ("clang", "clang++"):
            return FakeCompleted(0, "clang version 18.1.0\n")
        if name == "meson":
            return FakeCompleted(0, "1.4.0\n")
        if name == "ninja":
            return FakeCompleted(0, "1.11.1\n")
        return FakeCompleted(1)

    with mock.patch(
        "vmaf_rc1_tester.probe.shutil.which", side_effect=lambda name: f"/usr/bin/{name}"
    ):
        toolchain = probe_toolchain(runner=fake_runner)
    assert toolchain.c_compiler == "clang version 18.1.0"
    assert toolchain.meson == "1.4.0"
    assert toolchain.ninja == "1.11.1"


def test_probe_accelerators_cuda_avoids_uuid_fallback() -> None:
    commands: list[list[str]] = []

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        commands.append(command)
        return FakeCompleted(1, "", "query failed")

    with mock.patch(
        "vmaf_rc1_tester.probe.shutil.which",
        side_effect=lambda name: "/usr/bin/nvidia-smi" if name == "nvidia-smi" else None,
    ):
        probes = probe_accelerators(runner=fake_runner)
    assert probes.cuda_devices == []
    assert all("-L" not in command for command in commands)


def test_probe_accelerators_cuda_sycl_hip() -> None:
    def which(name: str) -> str | None:
        return f"/usr/bin/{name}" if name in ("nvidia-smi", "sycl-ls", "rocm-smi") else None

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        name = _program(command)
        if name == "nvidia-smi":
            return FakeCompleted(0, "NVIDIA RTX 4090, 550.54.14\n")
        if name == "sycl-ls":
            return FakeCompleted(0, "[ext_oneapi_level_zero:gpu:0] Intel Arc A770\n")
        if name == "rocm-smi":
            return FakeCompleted(
                0,
                "GPU[0] : Card Series: AMD Radeon RX 7900 XTX\n"
                "GPU[0] : GFX Version: gfx1100\n"
                "GPU[1] : Card Series: AMD Radeon Pro W6800\n"
                "GPU[1] : GFX Version: gfx1030\n",
            )
        return FakeCompleted(1)

    with mock.patch("vmaf_rc1_tester.probe.shutil.which", side_effect=which):
        probes = probe_accelerators(runner=fake_runner)
    assert probes.cuda_devices[0].driver_version == "550.54.14"
    assert probes.sycl_devices[0].driver_version == "unknown/not reported"
    assert probes.hip_devices[0].driver_version == "unknown/not reported"
    assert "Arc A770" in probes.sycl_devices[0].name
    assert "gfx1100" in probes.hip_devices[0].name
    assert [device.ordinal for device in probes.hip_devices] == [0, 1]
    assert "gfx1030" in probes.hip_devices[1].name


def test_rocminfo_fallback_excludes_cpu_agent_and_preserves_gpu_runtime_ordinals() -> None:
    output = """
Agent 1
  Marketing Name: AMD Ryzen 9 7950X 16-Core Processor
  Device Type: CPU
Agent 2
  Marketing Name: AMD Radeon RX 7900 XTX
  Device Type: GPU
Agent 3
  Marketing Name: AMD Radeon Pro W6800
  Device Type: GPU
"""
    devices = _parse_rocminfo_devices(output)
    assert [device.name for device in devices] == [
        "AMD Radeon RX 7900 XTX",
        "AMD Radeon Pro W6800",
    ]
    assert [device.ordinal for device in devices] == [0, 1]
    assert all("Ryzen" not in device.name for device in devices)


def test_probe_accelerators_metal() -> None:
    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        if _program(command) == "system_profiler":
            return FakeCompleted(0, "Chipset Model: Apple M3 Max\n")
        return FakeCompleted(1)

    with mock.patch("vmaf_rc1_tester.probe.platform.system", return_value="Darwin"):
        probes = probe_accelerators(runner=fake_runner)
    assert probes.metal_devices[0].name == "Apple M3 Max"
    assert probes.metal_devices[0].driver_version == "unknown/not reported"


def test_explicit_missing_binary_does_not_fall_back_to_path() -> None:
    with mock.patch("vmaf_rc1_tester.probe.shutil.which", return_value="/usr/local/bin/vmaf"):
        assert find_vmaf_binary("/nonexistent/vmaf") is None
    binary = probe_vmaf_binary("/nonexistent/vmaf")
    assert not binary.exists
    assert binary.accepted_backend_selectors == []


def test_build_dir_discovers_windows_executable(tmp_path: Path) -> None:
    executable = tmp_path / "build" / "tools" / "vmaf.exe"
    executable.parent.mkdir(parents=True)
    executable.write_text("windows test executable", encoding="utf-8")
    executable.chmod(0o755)
    assert find_vmaf_binary(build_dir=str(tmp_path / "build")) == str(executable.resolve())


def test_probe_vmaf_reads_version_from_stderr_and_hashes_binary(tmp_path: Path) -> None:
    fake_bin = tmp_path / "vmaf"
    fake_bin.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    fake_bin.chmod(0o755)

    def fake_runner(command: list[str], **_kwargs: object) -> FakeCompleted:
        if "--version" in command:
            return FakeCompleted(0, "", "3.2.0\n")
        if "--help" in command:
            return FakeCompleted(0, "--backend auto|cpu|cuda|metal\n")
        return FakeCompleted(1)

    binary = probe_vmaf_binary(str(fake_bin), runner=fake_runner)
    assert binary.version == "3.2.0"
    assert binary.accepted_backend_selectors == ["cpu", "cuda", "metal"]
    assert len(binary.sha256) == SHA256_HEX_LENGTH


def test_run_full_probe_contains_all_sections(tmp_path: Path) -> None:
    fake_bin = tmp_path / "vmaf"
    fake_bin.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    fake_bin.chmod(0o755)
    report = run_full_probe(
        vmaf_path=str(fake_bin),
        runner=lambda *_args, **_kwargs: FakeCompleted(0, "mock output", ""),
    )
    assert isinstance(report, DiagnosticReport)
    assert set(report.to_dict()) == {"os", "cpu", "toolchain", "accelerators", "vmaf_binary"}
