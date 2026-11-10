#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Verify default cross-backend parity gate run completes for CPU vs CUDA.

Checks that scripts/ci/cross_backend_parity_gate.py runs without error when
comparing default features on CPU vs CUDA (e.g. on an RTX 4090). A binary
built without CUDA, or a host without a CUDA device, has nothing to compare:
the test is skipped (exit 77), not failed.

A build directory configured without CUDA is skipped before the device lock is
taken: waiting for a device this build cannot use ran into the test's timeout
whenever another job held the lock.
"""

from __future__ import annotations

import json
import signal
import subprocess  # nosec B404 -- fixed repository gate requires SIGINT supervision.
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.lib import vmaftest  # noqa: E402 - needs the repository root on sys.path

SCRIPT = ROOT / "scripts" / "ci" / "cross_backend_parity_gate.py"
# Not measured: the parity gate needs a free GPU, which the measuring host did not have.
# Set under the 120 s Meson timeout of this test (core/test/meson.build), so this
# message, not Meson's kill, names the stuck run; it covers the lock wait too.
SCRIPT_TIMEOUT_S = 105
INTERRUPT_GRACE_S = 10
# After SIGKILL the gate dies at once; this only waits for the pipes to close, which a
# descendant that kept them open would otherwise hold forever.
KILL_GRACE_S = 2
REF = ROOT / "python" / "test" / "resource" / "yuv" / "src01_hrc00_576x324.yuv"
DIS = ROOT / "python" / "test" / "resource" / "yuv" / "src01_hrc01_576x324.yuv"


# What the CLI prints when --backend cuda cannot run at all: no device, or a
# libvmaf built without CUDA (ADR-0498 refuses the silent CPU fallback).
NO_CUDA_MARKERS = (
    "No CUDA device",
    "cudaErrorNoDevice",
    "built without cuda support",
)


def cuda_unavailable(output: str) -> bool:
    """True when `output` says the run could not use CUDA at all."""
    return any(marker in output for marker in NO_CUDA_MARKERS)


def build_has_cuda(build_dir: Path) -> bool | None:
    """Whether Meson configured `build_dir` with CUDA; None when it does not say."""
    options = build_dir / "meson-info" / "intro-buildoptions.json"
    try:
        entries = json.loads(options.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(entries, list):
        return None
    for entry in entries:
        if isinstance(entry, dict) and entry.get("name") == "enable_cuda":
            value = entry.get("value")
            return value if isinstance(value, bool) else None
    return None


def find_binary() -> Path | None:
    """The vmaf CLI under test, from the one resolver every Python test uses.

    scripts/lib/vmaftest.py reads VMAF_BIN, VMAF_BIN_FOR_TESTS, then the repository's
    build directories. Those directories are usually CPU builds, which skip_reason() skips:
    name a CUDA build with VMAF_BIN. A set variable that names no executable raises
    vmaftest.InvalidVmafBinary.
    """
    return vmaftest.find(root=ROOT)


def skip_reason(binary: Path | None) -> str | None:
    """Why there is nothing to compare, or None. Decided without the device."""
    if binary is None:
        return vmaftest.MISSING_MESSAGE
    if build_has_cuda(binary.parent.parent) is False:
        return "This build was configured without CUDA"
    if not REF.is_file() or not DIS.is_file():
        return "Reference / distorted YUV fixtures not found"
    return None


def gate_command(binary: Path, lock_path: Path | None = None) -> list[str]:
    """The parity gate command for `binary`, run under the GPU lock at `lock_path`.

    --no-fork: flock execs the gate, so the timeout path signals the gate itself and the
    lock frees only when the gate exits; a forked gate would outlive a kill and keep the
    lock. `lock_path` defaults to the shared 4090 lock; tests pass a temporary one.
    """
    if lock_path is None:
        lock_path = Path.home() / ".cache" / "vmafx-locks" / "cuda-4090.lock"
    return [
        "flock",
        "--no-fork",
        str(lock_path),
        sys.executable,
        str(SCRIPT),
        "--vmaf-binary",
        str(binary),
        "--reference",
        str(REF),
        "--distorted",
        str(DIS),
        "--width",
        "576",
        "--height",
        "324",
        "--backends",
        "cpu",
        "cuda",
    ]


def as_text(output: str | bytes | None) -> str:
    """Partial output of a TimeoutExpired, which communicate() leaves undecoded."""
    if isinstance(output, bytes):
        return output.decode("utf-8", errors="replace")
    return output or ""


def stop_gate(proc: subprocess.Popen[str]) -> tuple[str, str]:
    """SIGINT, then SIGKILL, each wait bounded; return the output collected so far."""
    proc.send_signal(signal.SIGINT)
    try:
        return proc.communicate(timeout=INTERRUPT_GRACE_S)
    except subprocess.TimeoutExpired:
        proc.kill()
    try:
        return proc.communicate(timeout=KILL_GRACE_S)
    except subprocess.TimeoutExpired as exc:
        sys.stderr.write(
            f"A gate descendant still held the output pipes {KILL_GRACE_S}s after the kill"
            " (KILL_GRACE_S)\n"
        )
        return as_text(exc.stdout), as_text(exc.stderr)


def run_gate(cmd: list[str]) -> subprocess.CompletedProcess[str] | None:
    """Interrupt a timed-out gate so its asyncio supervisor reaps vmaf before unlocking."""
    with subprocess.Popen(  # noqa: S603  # nosec B603 -- fixed argv uses sys.executable and a repository script.
        cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    ) as proc:
        try:
            stdout, stderr = proc.communicate(timeout=SCRIPT_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            stdout, stderr = stop_gate(proc)
            sys.stdout.write(stdout or "")
            sys.stderr.write(stderr or "")
            sys.stderr.write(
                f"Default parity gate timed out after {SCRIPT_TIMEOUT_S}s (SCRIPT_TIMEOUT_S)\n"
            )
            return None
        return subprocess.CompletedProcess(cmd, proc.returncode, stdout, stderr)


def gate_verdict(proc: subprocess.CompletedProcess[str] | None) -> int:
    """The exit code for a finished gate run: 0, 77 to skip, 124 on timeout, else a failure."""
    if proc is None:
        return 124
    if proc.returncode != 0:
        if cuda_unavailable(proc.stdout + proc.stderr):
            sys.stderr.write("CUDA is not available to this binary; skipping (77)\n")
            return 77
        sys.stderr.write(
            f"Default parity gate failed:\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}\n"
        )
        return proc.returncode

    if (
        "motion         cpu    ↔ cuda" not in proc.stdout
        or "cambi          cpu    ↔ cuda" not in proc.stdout
    ):
        sys.stderr.write(f"Missing expected cells in output:\n{proc.stdout}\n")
        return 1

    return 0


def main() -> int:
    """Run the CUDA parity gate under the GPU lock and return its exit code, or 77 to skip."""
    try:
        binary = find_binary()
    except vmaftest.InvalidVmafBinary as exc:
        sys.stderr.write(f"{exc}\n")
        return 1
    reason = skip_reason(binary)
    if reason is not None or binary is None:
        sys.stderr.write(f"{reason}; skipping (77)\n")
        return 77

    return gate_verdict(run_gate(gate_command(binary)))


if __name__ == "__main__":
    sys.exit(main())
