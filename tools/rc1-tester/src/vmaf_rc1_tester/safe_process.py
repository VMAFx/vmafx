# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Bounded subprocess execution for the external tester.

The tester ships as a stand-alone wheel, so it carries a byte copy of the
repository's runner, ``scripts/lib/safe_subprocess.py``, as
``_safe_subprocess.py`` (``tests/test_safe_subprocess_copy.py`` fails when the
two differ). This module keeps the tester's own public contract on top of it:
the names, exceptions and messages below are unchanged since the wheel was
first published.
"""

from __future__ import annotations

import asyncio
import errno
import os
import shutil
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from vmaf_rc1_tester import _safe_subprocess as shared

MAX_ARG_COUNT = shared.MAX_ARG_COUNT
MAX_ARG_BYTES = shared.MAX_ARG_BYTES
MAX_ARGV_BYTES = shared.MAX_ARGV_BYTES
READ_CHUNK_BYTES = shared.READ_CHUNK_BYTES
TERMINATION_GRACE_SECONDS = shared.TERMINATION_GRACE_SECONDS
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


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


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


def _check_working_directory(cwd: str | None) -> None:
    """Raise the OSError process creation raised for a missing directory."""
    if cwd is None or os.path.isdir(cwd):
        return
    code = errno.ENOTDIR if os.path.exists(cwd) else errno.ENOENT
    raise OSError(code, os.strerror(code), cwd)


def run_bounded(
    argv: Sequence[str],
    *,
    environment: Mapping[str, str] | None = None,
    timeout_seconds: float,
    max_output_bytes: int,
    cwd: str | None = None,
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
    _check_working_directory(cwd)
    try:
        result = shared.run(command, allowed_executables=[command[0]], cwd=cwd,
                            env=selected_environment, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout_seconds=timeout_seconds,
                            max_output_bytes=max_output_bytes)  # fmt: skip
    except shared.CommandValidationError as exc:
        raise CommandValidationError(str(exc)) from None
    except shared.CommandTimedOut as exc:
        raise CommandTimedOut(timeout_seconds, exc.stdout or "", exc.stderr or "") from None
    except shared.CommandOutputLimitExceeded as exc:
        raise CommandOutputLimitExceeded(
            max_output_bytes, exc.stdout or "", exc.stderr or ""
        ) from None
    return CommandResult(result.returncode, result.stdout, result.stderr)
