# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tester-facing inventory with honest RC1/RC2, RC3, and RC4 boundaries."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any


@dataclass(frozen=True)
class ToolEntry:
    name: str
    phase: str
    category: str
    path_or_command: str
    description: str
    hardware_scope: str
    notes: str
    is_rc1_ready: bool


REPO_TOOLS: tuple[ToolEntry, ...] = (
    ToolEntry(
        "vmaf-rc1-report",
        "RC1",
        "Diagnostics and reporting",
        "tools/rc1-tester/vmaf-rc1-report",
        "Probes the host, runs four-frame CPU-referenced backend checks, and creates one report archive.",
        "CPU, CUDA, SYCL, HIP, Metal",
        "Requires a source checkout plus an executable vmaf binary.",
        True,
    ),
    ToolEntry(
        "vmaf CLI",
        "RC1",
        "Metric execution",
        "build/tools/vmaf --backend <name> ...",
        "Runs metric scoring with an explicit backend selector.",
        "CPU, CUDA, SYCL, HIP, Metal",
        "A selector being accepted does not prove that backend was compiled; PASS requires matching backend-state JSON plus bounded CPU-referenced metrics.",
        True,
    ),
    ToolEntry(
        "Meson fast suite",
        "RC1",
        "Correctness tests",
        "make test-fast",
        "Runs the bounded fast tests compiled into the selected build.",
        "Build-dependent",
        "GPU tests may skip when their runtime or device is unavailable.",
        True,
    ),
    ToolEntry(
        "Platform setup dispatcher",
        "RC1",
        "Host setup",
        "scripts/setup/detect.sh",
        "Selects the repository setup path for the detected operating system.",
        "Linux and macOS; Windows has dedicated setup scripts",
        "Review package-manager changes before approving privilege escalation.",
        True,
    ),
    ToolEntry(
        "vmaf_bench",
        "RC3",
        "Performance benchmarking",
        "build/tools/vmaf_bench",
        "Measures feature-extractor throughput.",
        "CPU, CUDA, SYCL",
        "Current C tool does not cover HIP or Metal; --list-devices enumerates SYCL devices only.",
        False,
    ),
    ToolEntry(
        "Backend benchmark harness",
        "RC3",
        "Performance benchmarking",
        "testdata/bench_backends.py",
        "Runs backend benchmark rows over checked-in fixtures.",
        "CPU, CUDA, SYCL, HIP on Linux",
        "Uses Linux /proc load data and has no Metal lane; cross-platform cleanup belongs to RC3.",
        False,
    ),
    ToolEntry(
        "vmaf-tune",
        "RC3",
        "Encoder tuning",
        "tools/vmaf-tune/vmaf-tune",
        "Runs rate-quality sweeps, bisection, and encoder parameter search.",
        "CPU and supported hardware encoders",
        "Intentionally excluded from RC1 smoke collection.",
        False,
    ),
    ToolEntry(
        "ensemble-training-kit",
        "RC4",
        "Model training",
        "tools/ensemble-training-kit/run-full-pipeline.sh",
        "Runs LOSO validation and production model export.",
        "CPU, CUDA, SYCL",
        "Real training is deferred until RC4.",
        False,
    ),
    ToolEntry(
        "tiny-AI training",
        "RC4",
        "Model training",
        "ai/",
        "Contains the PyTorch training and ONNX export workflows.",
        "CPU, CUDA",
        "Real training is deferred until RC4.",
        False,
    ),
)


def get_tools_catalog() -> list[dict[str, Any]]:
    return [asdict(tool) for tool in REPO_TOOLS]


def _escape(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", "<br>")


def render_tools_markdown() -> str:
    """Render the phase inventory, including known coverage gaps."""
    lines = [
        "## Repository tool inventory by release phase",
        "",
        "| Phase | Tool | Command or path | Hardware scope | Readiness notes |",
        "| :--- | :--- | :--- | :--- | :--- |",
    ]
    for tool in REPO_TOOLS:
        status = "RC1 ready" if tool.is_rc1_ready else f"Deferred to {tool.phase}"
        lines.append(
            f"| **{tool.phase}** | `{tool.name}` | `{tool.path_or_command}` | "
            f"{_escape(tool.hardware_scope)} | **{status}.** {_escape(tool.notes)} |"
        )
    lines.extend(
        [
            "",
            "> RC1 and the RC2 stabilisation candidate collect build, environment, compiled-test, and bounded backend-correctness evidence. Performance benchmarking/tuning starts in RC3; real model training starts in RC4.",
        ]
    )
    return "\n".join(lines)
