# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Self-contained bounded subprocess execution for the external tester."""

from __future__ import annotations

import asyncio
import os
import shutil
import signal
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

MAX_ARG_COUNT = 4096
MAX_ARG_BYTES = 65_536
MAX_ARGV_BYTES = 1_048_576
READ_CHUNK_BYTES = 65_536
TERMINATION_GRACE_SECONDS = 2.0
WINDOWS_CREATE_NEW_PROCESS_GROUP = 0x00000200


class CommandValidationError(ValueError):
    """The direct argv command does not satisfy the bounded contract."""


class CommandTimedOut(TimeoutError):
    """A command exceeded its deadline and its process group was stopped."""

    def __init__(self, timeout_seconds: float, stdout: str, stderr: str) -> None:
        self.stdout = stdout
        self.stderr = stderr
        super().__init__(f"command timed out after {timeout_seconds:g}s")


class CommandOutputLimitExceeded(RuntimeError):
    """A command exceeded the shared stdout/stderr capture budget."""

    def __init__(self, max_output_bytes: int, stdout: str, stderr: str) -> None:
        self.stdout = stdout
        self.stderr = stderr
        super().__init__(f"command exceeded {max_output_bytes} captured bytes")


class _CaptureOverflow(RuntimeError):
    pass


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


@dataclass
class _CaptureBudget:
    limit: int
    used: int = 0

    def retain(self, chunk: bytes, destination: bytearray) -> None:
        remaining = self.limit - self.used
        if remaining > 0:
            kept = chunk[:remaining]
            destination.extend(kept)
            self.used += len(kept)
        if len(chunk) > remaining:
            raise _CaptureOverflow


def _validated_argv(argv: Sequence[str]) -> tuple[str, ...]:
    if not argv or len(argv) > MAX_ARG_COUNT:
        raise CommandValidationError("argv must contain at most 4096 entries")
    values: list[str] = []
    for index, value in enumerate(argv):
        if not isinstance(value, str) or "\0" in value:
            raise CommandValidationError(f"argv[{index}] must be NUL-free text")
        if len(os.fsencode(value)) > MAX_ARG_BYTES:
            raise CommandValidationError(f"argv[{index}] exceeds {MAX_ARG_BYTES} bytes")
        values.append(value)
    if sum(len(os.fsencode(value)) + 1 for value in values) > MAX_ARGV_BYTES:
        raise CommandValidationError(f"argv exceeds {MAX_ARGV_BYTES} encoded bytes")
    executable = _resolve_executable(values[0])
    return (executable, *values[1:])


def _resolve_executable(value: str) -> str:
    candidate = Path(value).expanduser()
    if candidate.is_absolute():
        try:
            resolved = candidate.resolve(strict=True)
        except OSError as exc:
            raise CommandValidationError(f"executable is unavailable: {value}") from exc
    elif len(candidate.parts) == 1:
        found = shutil.which(value)
        if found is None:
            raise CommandValidationError(f"executable is unavailable: {value}")
        resolved = Path(found).resolve(strict=True)
    else:
        raise CommandValidationError("executable path must be absolute or bare")
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise CommandValidationError(f"executable is not runnable: {resolved}")
    return str(resolved)


def _validated_environment(environment: Mapping[str, str] | None) -> dict[str, str]:
    source = os.environ if environment is None else environment
    result: dict[str, str] = {}
    for key, value in source.items():
        if not isinstance(key, str) or not isinstance(value, str):
            raise CommandValidationError("environment entries must be text")
        if not key or "=" in key or "\0" in key or "\0" in value:
            raise CommandValidationError(f"invalid environment entry: {key!r}")
        result[key] = value
    return result


async def _read_bounded(
    reader: asyncio.StreamReader,
    destination: bytearray,
    budget: _CaptureBudget,
) -> None:
    while chunk := await reader.read(READ_CHUNK_BYTES):
        budget.retain(chunk, destination)


def _signal_group(process_id: int, selected_signal: signal.Signals) -> bool:
    try:
        os.killpg(process_id, selected_signal)
    except ProcessLookupError:
        return False
    return True


def _process_group_exists(process_id: int) -> bool:
    try:
        os.killpg(process_id, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


async def _terminate_process_group(process: asyncio.subprocess.Process) -> None:
    if os.name == "posix":
        if not _signal_group(process.pid, signal.SIGTERM):
            return
        loop = asyncio.get_running_loop()
        deadline = loop.time() + TERMINATION_GRACE_SECONDS
        while _process_group_exists(process.pid) and loop.time() < deadline:
            await asyncio.sleep(min(0.05, max(0.0, deadline - loop.time())))
        if _process_group_exists(process.pid):
            _signal_group(process.pid, signal.SIGKILL)
        if process.returncode is None:
            await asyncio.wait_for(process.wait(), timeout=TERMINATION_GRACE_SECONDS)
        return
    if process.returncode is not None:
        return
    try:
        process.terminate()
    except ProcessLookupError:
        return
    try:
        await asyncio.wait_for(process.wait(), timeout=TERMINATION_GRACE_SECONDS)
    except TimeoutError:
        try:
            process.kill()
        except ProcessLookupError:
            return
        await asyncio.wait_for(process.wait(), timeout=TERMINATION_GRACE_SECONDS)


async def _capture(
    process: asyncio.subprocess.Process,
    timeout_seconds: float,
    max_output_bytes: int,
) -> tuple[bytes, bytes, bool, bool]:
    assert process.stdout is not None
    assert process.stderr is not None
    stdout = bytearray()
    stderr = bytearray()
    budget = _CaptureBudget(max_output_bytes)
    tasks = [
        asyncio.create_task(process.wait()),
        asyncio.create_task(_read_bounded(process.stdout, stdout, budget)),
        asyncio.create_task(_read_bounded(process.stderr, stderr, budget)),
    ]
    timed_out = False
    overflowed = False
    cancellation: asyncio.CancelledError | None = None
    try:
        await asyncio.wait_for(asyncio.gather(*tasks), timeout=timeout_seconds)
    except TimeoutError:
        timed_out = True
    except _CaptureOverflow:
        overflowed = True
    except asyncio.CancelledError as exc:
        cancellation = exc
    finally:
        if timed_out or overflowed or cancellation is not None:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await _terminate_process_group(process)
    if cancellation is not None:
        raise cancellation
    return bytes(stdout), bytes(stderr), timed_out, overflowed


async def _run_async(
    argv: tuple[str, ...],
    environment: dict[str, str],
    timeout_seconds: float,
    max_output_bytes: int,
) -> CommandResult:
    process_options: dict[str, object] = {
        "stdin": asyncio.subprocess.DEVNULL,
        "stdout": asyncio.subprocess.PIPE,
        "stderr": asyncio.subprocess.PIPE,
        "env": environment,
    }
    if os.name == "posix":
        process_options["start_new_session"] = True
    elif os.name == "nt":
        process_options["creationflags"] = WINDOWS_CREATE_NEW_PROCESS_GROUP
    process = await asyncio.create_subprocess_exec(*argv, **process_options)
    stdout, stderr, timed_out, overflowed = await _capture(
        process, timeout_seconds, max_output_bytes
    )
    decoded_stdout = stdout.decode("utf-8", "replace")
    decoded_stderr = stderr.decode("utf-8", "replace")
    if timed_out:
        raise CommandTimedOut(timeout_seconds, decoded_stdout, decoded_stderr)
    if overflowed:
        raise CommandOutputLimitExceeded(max_output_bytes, decoded_stdout, decoded_stderr)
    assert process.returncode is not None
    return CommandResult(process.returncode, decoded_stdout, decoded_stderr)


def run_bounded(
    argv: Sequence[str],
    *,
    environment: Mapping[str, str] | None = None,
    timeout_seconds: float,
    max_output_bytes: int,
) -> CommandResult:
    """Run one direct argv command with process-group and output bounds."""
    if timeout_seconds <= 0 or max_output_bytes <= 0:
        raise CommandValidationError("timeout and output bounds must be positive")
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        pass
    else:
        raise RuntimeError("run_bounded cannot block an active event loop")
    command = _validated_argv(argv)
    selected_environment = _validated_environment(environment)
    return asyncio.run(_run_async(command, selected_environment, timeout_seconds, max_output_bytes))
