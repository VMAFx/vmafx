#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Validated, bounded process execution for repository automation.

The standard :mod:`subprocess` API deliberately accepts arbitrary argument
vectors.  Repository automation has a narrower contract: the executable is
explicitly allowlisted, every argument is length-bounded and NUL-free, output
captured in memory has a hard combined ceiling, and a timeout terminates the
whole process group rather than abandoning descendants.

Callers that redirect output to a file opt out of the in-memory ceiling for
that stream.  This is intentional: the file is then the bounded CI artifact or
operator-owned log, not an allocation controlled by child output.
"""

from __future__ import annotations

import asyncio
import locale
import os
import shutil
import signal
from collections.abc import Collection, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any

MAX_ARG_COUNT = 4096
MAX_ARG_BYTES = 65_536
MAX_ARGV_BYTES = 1_048_576
DEFAULT_TIMEOUT_SECONDS = 120.0
DEFAULT_MAX_OUTPUT_BYTES = 4 * 1_048_576
TERMINATION_GRACE_SECONDS = 2.0
READ_CHUNK_BYTES = 65_536
_WINDOWS_CREATE_NEW_PROCESS_GROUP = 0x00000200

Arg = str | os.PathLike[str]


class CommandValidationError(ValueError):
    """The requested command does not satisfy the execution contract."""


@dataclass(frozen=True)
class CommandResult:
    """Small immutable result returned by :func:`run`."""

    argv: tuple[str, ...]
    returncode: int
    stdout: str | bytes | None
    stderr: str | bytes | None

    def check_returncode(self) -> None:
        """Raise :class:`CommandFailed` when the child did not succeed."""
        if self.returncode != 0:
            raise CommandFailed(self)


class CommandFailed(RuntimeError):
    """A validated command ran and returned a non-zero status."""

    def __init__(self, result: CommandResult):
        self.result = result
        super().__init__(f"command exited {result.returncode}: {result.argv[0]}")


class CommandTimedOut(TimeoutError):
    """A command exceeded its wall-clock deadline and was terminated."""

    def __init__(
        self,
        argv: tuple[str, ...],
        timeout_seconds: float,
        stdout: str | bytes | None,
        stderr: str | bytes | None,
    ):
        self.argv = argv
        self.timeout_seconds = timeout_seconds
        self.stdout = stdout
        self.stderr = stderr
        super().__init__(f"command timed out after {timeout_seconds:g}s: {argv[0]}")


class CommandOutputLimitExceeded(RuntimeError):
    """Captured output reached the configured in-memory ceiling."""

    def __init__(
        self,
        argv: tuple[str, ...],
        max_output_bytes: int,
        stdout: str | bytes | None,
        stderr: str | bytes | None,
    ):
        self.argv = argv
        self.max_output_bytes = max_output_bytes
        self.stdout = stdout
        self.stderr = stderr
        super().__init__(f"command exceeded {max_output_bytes} captured bytes: {argv[0]}")


class _CaptureOverflow(RuntimeError):
    """Internal signal from a stream reader to the process supervisor."""


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


@dataclass(frozen=True)
class _AllowedExecutables:
    names: frozenset[str]
    paths: frozenset[Path]


@dataclass(frozen=True)
class _RunConfiguration:
    environment: dict[str, str]
    working_directory: str | None
    encoding: str
    payload: bytes | None
    capture_stdout: bool
    capture_stderr: bool
    process_kwargs: dict[str, Any]


def _stringify(value: Arg, *, label: str) -> str:
    try:
        text = os.fspath(value)
    except TypeError as exc:
        raise CommandValidationError(f"{label} must be a string or path") from exc
    if not isinstance(text, str):
        raise CommandValidationError(f"{label} must resolve to text, not bytes")
    if "\0" in text:
        raise CommandValidationError(f"{label} contains a NUL byte")
    encoded = os.fsencode(text)
    if len(encoded) > MAX_ARG_BYTES:
        raise CommandValidationError(f"{label} exceeds {MAX_ARG_BYTES} encoded bytes")
    return text


def _parse_allowed_executables(allowed: Collection[Arg]) -> _AllowedExecutables:
    if not allowed:
        raise CommandValidationError("allowed_executables must not be empty")
    allowed_names: set[str] = set()
    allowed_paths: set[Path] = set()
    for index, value in enumerate(allowed):
        token = _stringify(value, label=f"allowed executable {index}")
        candidate = Path(token).expanduser()
        if candidate.is_absolute():
            try:
                allowed_paths.add(candidate.resolve(strict=True))
            except OSError as exc:
                raise CommandValidationError(
                    f"allowlisted executable is unavailable: {token}"
                ) from exc
        elif len(candidate.parts) == 1:
            allowed_names.add(token)
        else:
            raise CommandValidationError(
                f"allowlisted executable must be bare or absolute: {token}"
            )
    return _AllowedExecutables(frozenset(allowed_names), frozenset(allowed_paths))


def _resolve_absolute_executable(
    executable: str,
    requested: Path,
    allowed: _AllowedExecutables,
    search_path: str,
) -> Path:
    try:
        resolved = requested.resolve(strict=True)
    except OSError as exc:
        raise CommandValidationError(f"executable is unavailable: {executable}") from exc
    name_matches: set[Path] = set()
    for name in allowed.names:
        found = shutil.which(name, path=search_path)
        if found is not None:
            name_matches.add(Path(found).resolve(strict=True))
    if resolved not in allowed.paths and resolved not in name_matches:
        raise CommandValidationError(f"executable is not allowlisted: {resolved}")
    return resolved


def _resolve_allowed_executable(
    executable: str, allowed_values: Collection[Arg], search_path: str
) -> str:
    allowed = _parse_allowed_executables(allowed_values)

    requested = Path(executable).expanduser()
    if requested.is_absolute():
        resolved = _resolve_absolute_executable(executable, requested, allowed, search_path)
    elif len(requested.parts) == 1:
        if executable not in allowed.names:
            raise CommandValidationError(f"executable name is not allowlisted: {executable}")
        found = shutil.which(executable, path=search_path)
        if found is None:
            raise CommandValidationError(f"executable is unavailable: {executable}")
        resolved = Path(found).resolve(strict=True)
    else:
        raise CommandValidationError(f"executable path must be absolute or bare: {executable}")

    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise CommandValidationError(f"executable is not a runnable file: {resolved}")
    return str(resolved)


def _validated_argv(
    argv: Sequence[Arg], allowed_executables: Collection[Arg], search_path: str
) -> tuple[str, ...]:
    if not argv:
        raise CommandValidationError("argv must contain an executable")
    if len(argv) > MAX_ARG_COUNT:
        raise CommandValidationError(f"argv exceeds {MAX_ARG_COUNT} entries")
    values = tuple(_stringify(value, label=f"argv[{index}]") for index, value in enumerate(argv))
    if sum(len(os.fsencode(value)) + 1 for value in values) > MAX_ARGV_BYTES:
        raise CommandValidationError(f"argv exceeds {MAX_ARGV_BYTES} encoded bytes")
    executable = _resolve_allowed_executable(values[0], allowed_executables, search_path)
    return (executable, *values[1:])


def _validated_environment(environment: Mapping[str, str] | None) -> dict[str, str]:
    source = os.environ if environment is None else environment
    result: dict[str, str] = {}
    for key, value in source.items():
        if not isinstance(key, str) or not isinstance(value, str):
            raise CommandValidationError("environment keys and values must be strings")
        if not key or "=" in key or "\0" in key or "\0" in value:
            raise CommandValidationError(f"invalid environment entry: {key!r}")
        result[key] = value
    return result


def _decode(data: bytes | None, *, text: bool, encoding: str, errors: str) -> str | bytes | None:
    if data is None or not text:
        return data
    return data.decode(encoding, errors)


def _working_directory(cwd: str | os.PathLike[str] | None) -> str | None:
    if cwd is None:
        return None
    try:
        directory = Path(cwd).expanduser().resolve(strict=True)
    except OSError as exc:
        raise CommandValidationError(f"working directory is unavailable: {cwd}") from exc
    if not directory.is_dir():
        raise CommandValidationError(f"working directory is not a directory: {directory}")
    return str(directory)


def _validate_run_options(
    *,
    input_data: str | bytes | None,
    capture_output: bool,
    stdout: IO[Any] | None,
    stderr: IO[Any] | None,
    stderr_to_stdout: bool,
    text: bool,
    timeout_seconds: float,
    max_output_bytes: int,
) -> None:
    if timeout_seconds <= 0:
        raise CommandValidationError("timeout_seconds must be positive")
    if max_output_bytes <= 0:
        raise CommandValidationError("max_output_bytes must be positive")
    if capture_output and (stdout is not None or stderr is not None):
        raise CommandValidationError("capture_output cannot be combined with stdout or stderr")
    if stderr_to_stdout and stderr is not None:
        raise CommandValidationError("stderr_to_stdout cannot be combined with stderr")
    if isinstance(input_data, str) and not text:
        raise CommandValidationError("string input_data requires text=True")
    if isinstance(input_data, bytes) and text:
        raise CommandValidationError("bytes input_data requires text=False")


def _prepare_run_configuration(
    *,
    cwd: str | os.PathLike[str] | None,
    environment: dict[str, str],
    input_data: str | bytes | None,
    capture_output: bool,
    stdout: IO[Any] | None,
    stderr: IO[Any] | None,
    stderr_to_stdout: bool,
    text: bool,
    encoding: str | None,
    errors: str,
) -> _RunConfiguration:
    selected_encoding = encoding or locale.getpreferredencoding(False)
    capture_stdout = capture_output
    capture_stderr = capture_output and not stderr_to_stdout
    stdout_target: Any = asyncio.subprocess.PIPE if capture_stdout else stdout
    if stderr_to_stdout:
        stderr_target: Any = asyncio.subprocess.STDOUT
    else:
        stderr_target = asyncio.subprocess.PIPE if capture_stderr else stderr
    payload = (
        input_data.encode(selected_encoding, errors) if isinstance(input_data, str) else input_data
    )
    process_kwargs: dict[str, Any] = {
        "cwd": _working_directory(cwd),
        "env": environment,
        "stdin": asyncio.subprocess.PIPE if payload is not None else asyncio.subprocess.DEVNULL,
        "stdout": stdout_target,
        "stderr": stderr_target,
    }
    if os.name == "posix":
        process_kwargs["start_new_session"] = True
    elif os.name == "nt":
        process_kwargs["creationflags"] = _WINDOWS_CREATE_NEW_PROCESS_GROUP
    return _RunConfiguration(
        environment=process_kwargs["env"],
        working_directory=process_kwargs["cwd"],
        encoding=selected_encoding,
        payload=payload,
        capture_stdout=capture_stdout,
        capture_stderr=capture_stderr,
        process_kwargs=process_kwargs,
    )


async def _read_bounded(
    reader: asyncio.StreamReader,
    destination: bytearray,
    budget: _CaptureBudget,
) -> None:
    while chunk := await reader.read(READ_CHUNK_BYTES):
        budget.retain(chunk, destination)


async def _write_input(writer: asyncio.StreamWriter, data: bytes) -> None:
    try:
        writer.write(data)
        await writer.drain()
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        writer.close()


async def _terminate_process_group(process: asyncio.subprocess.Process) -> None:
    if os.name == "posix":
        await _terminate_posix_process_group(process)
        return
    if process.returncode is not None:
        return
    try:
        process.terminate()
    except ProcessLookupError:
        return
    try:
        await asyncio.wait_for(process.wait(), timeout=TERMINATION_GRACE_SECONDS)
        return
    except TimeoutError:
        pass
    try:
        process.kill()
    except ProcessLookupError:
        return
    try:
        await asyncio.wait_for(process.wait(), timeout=TERMINATION_GRACE_SECONDS)
    except TimeoutError as exc:
        raise RuntimeError(f"unable to reap timed-out process {process.pid}") from exc


def _posix_process_group_exists(group_id: int) -> bool:
    try:
        os.killpg(group_id, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _signal_posix_process_group(group_id: int, selected_signal: signal.Signals) -> bool:
    try:
        os.killpg(group_id, selected_signal)
    except ProcessLookupError:
        return False
    return True


async def _terminate_posix_process_group(process: asyncio.subprocess.Process) -> None:
    """Terminate the whole session even when its original leader already exited."""
    if not _signal_posix_process_group(process.pid, signal.SIGTERM):
        return

    loop = asyncio.get_running_loop()
    deadline = loop.time() + TERMINATION_GRACE_SECONDS
    while _posix_process_group_exists(process.pid) and loop.time() < deadline:
        await asyncio.sleep(min(0.05, max(0.0, deadline - loop.time())))
    if _posix_process_group_exists(process.pid):
        _signal_posix_process_group(process.pid, signal.SIGKILL)
    if process.returncode is None:
        try:
            await asyncio.wait_for(process.wait(), timeout=TERMINATION_GRACE_SECONDS)
        except TimeoutError as exc:
            raise RuntimeError(f"unable to reap timed-out process {process.pid}") from exc


async def _supervise(
    process: asyncio.subprocess.Process,
    configuration: _RunConfiguration,
    *,
    timeout_seconds: float,
    max_output_bytes: int,
) -> tuple[bytes | None, bytes | None, bool, bool]:
    stdout_bytes = bytearray()
    stderr_bytes = bytearray()
    budget = _CaptureBudget(max_output_bytes)
    tasks: list[asyncio.Task[Any]] = [asyncio.create_task(process.wait())]
    if configuration.capture_stdout:
        assert process.stdout is not None
        tasks.append(asyncio.create_task(_read_bounded(process.stdout, stdout_bytes, budget)))
    if configuration.capture_stderr:
        assert process.stderr is not None
        tasks.append(asyncio.create_task(_read_bounded(process.stderr, stderr_bytes, budget)))
    if configuration.payload is not None:
        assert process.stdin is not None
        tasks.append(asyncio.create_task(_write_input(process.stdin, configuration.payload)))

    timed_out = False
    overflowed = False
    try:
        await asyncio.wait_for(asyncio.gather(*tasks), timeout=timeout_seconds)
    except TimeoutError:
        timed_out = True
    except _CaptureOverflow:
        overflowed = True
    finally:
        if timed_out or overflowed:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await _terminate_process_group(process)
    stdout = bytes(stdout_bytes) if configuration.capture_stdout else None
    stderr = bytes(stderr_bytes) if configuration.capture_stderr else None
    return stdout, stderr, timed_out, overflowed


def _finalize_result(
    command: tuple[str, ...],
    process: asyncio.subprocess.Process,
    configuration: _RunConfiguration,
    stdout: bytes | None,
    stderr: bytes | None,
    *,
    text: bool,
    errors: str,
    timed_out: bool,
    overflowed: bool,
    timeout_seconds: float,
    max_output_bytes: int,
    check: bool,
) -> CommandResult:
    decoded_stdout = _decode(
        stdout,
        text=text,
        encoding=configuration.encoding,
        errors=errors,
    )
    decoded_stderr = _decode(
        stderr,
        text=text,
        encoding=configuration.encoding,
        errors=errors,
    )
    if timed_out:
        raise CommandTimedOut(command, timeout_seconds, decoded_stdout, decoded_stderr)
    if overflowed:
        raise CommandOutputLimitExceeded(command, max_output_bytes, decoded_stdout, decoded_stderr)
    assert process.returncode is not None
    result = CommandResult(command, process.returncode, decoded_stdout, decoded_stderr)
    if check:
        result.check_returncode()
    return result


async def run_async(
    argv: Sequence[Arg],
    *,
    allowed_executables: Collection[Arg],
    cwd: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
    input_data: str | bytes | None = None,
    capture_output: bool = False,
    stdout: IO[Any] | None = None,
    stderr: IO[Any] | None = None,
    stderr_to_stdout: bool = False,
    text: bool = False,
    encoding: str | None = None,
    errors: str = "strict",
    check: bool = False,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES,
) -> CommandResult:
    """Run one validated command without a shell.

    ``env=None`` copies the current environment; a supplied mapping replaces it
    exactly.  Standard input is closed unless ``input_data`` is provided.
    Captured stdout and stderr share ``max_output_bytes`` so a child cannot
    double the allocation by flooding both streams.
    """
    environment = _validated_environment(env)
    command = _validated_argv(argv, allowed_executables, environment.get("PATH", os.defpath))
    _validate_run_options(
        input_data=input_data,
        capture_output=capture_output,
        stdout=stdout,
        stderr=stderr,
        stderr_to_stdout=stderr_to_stdout,
        text=text,
        timeout_seconds=timeout_seconds,
        max_output_bytes=max_output_bytes,
    )
    configuration = _prepare_run_configuration(
        cwd=cwd,
        environment=environment,
        input_data=input_data,
        capture_output=capture_output,
        stdout=stdout,
        stderr=stderr,
        stderr_to_stdout=stderr_to_stdout,
        text=text,
        encoding=encoding,
        errors=errors,
    )
    process = await asyncio.create_subprocess_exec(*command, **configuration.process_kwargs)
    captured_stdout, captured_stderr, timed_out, overflowed = await _supervise(
        process,
        configuration,
        timeout_seconds=timeout_seconds,
        max_output_bytes=max_output_bytes,
    )
    return _finalize_result(
        command,
        process,
        configuration,
        captured_stdout,
        captured_stderr,
        text=text,
        errors=errors,
        timed_out=timed_out,
        overflowed=overflowed,
        timeout_seconds=timeout_seconds,
        max_output_bytes=max_output_bytes,
        check=check,
    )


def run(
    argv: Sequence[Arg],
    *,
    allowed_executables: Collection[Arg],
    cwd: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
    input_data: str | bytes | None = None,
    capture_output: bool = False,
    stdout: IO[Any] | None = None,
    stderr: IO[Any] | None = None,
    stderr_to_stdout: bool = False,
    text: bool = False,
    encoding: str | None = None,
    errors: str = "strict",
    check: bool = False,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES,
) -> CommandResult:
    """Synchronous entry point; async callers should await :func:`run_async`."""
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        pass
    else:
        raise RuntimeError(
            "safe_subprocess.run() cannot block an active event loop; await run_async()"
        )
    return asyncio.run(
        run_async(
            argv,
            allowed_executables=allowed_executables,
            cwd=cwd,
            env=env,
            input_data=input_data,
            capture_output=capture_output,
            stdout=stdout,
            stderr=stderr,
            stderr_to_stdout=stderr_to_stdout,
            text=text,
            encoding=encoding,
            errors=errors,
            check=check,
            timeout_seconds=timeout_seconds,
            max_output_bytes=max_output_bytes,
        )
    )
