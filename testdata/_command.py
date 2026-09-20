#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Small synchronous wrapper around asyncio's shell-free process API."""

from __future__ import annotations

import asyncio
import locale
import os
import shutil
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any, Literal, overload


def _resolved_command(
    command: Sequence[str | os.PathLike[str]],
    *,
    cwd: str | os.PathLike[str] | None,
    env: Mapping[str, str] | None,
) -> list[str]:
    if not command:
        raise ValueError("command must not be empty")

    original = [os.fspath(argument) for argument in command]
    program = Path(original[0])
    if program.is_absolute():
        executable = program
    elif program.parent != Path():
        base = Path.cwd() if cwd is None else Path(cwd)
        executable = (base / program).resolve()
    else:
        search_path = env.get("PATH") if env is not None else None
        match = shutil.which(original[0], path=search_path)
        if match is None:
            raise FileNotFoundError(f"executable not found: {original[0]}")
        executable = Path(match).resolve()
    return [str(executable), *original[1:]]


def _decoded(data: bytes | None, *, text: bool) -> bytes | str | None:
    if data is None or not text:
        return data
    return data.decode(locale.getpreferredencoding(False))


async def _run_command_async(
    command: Sequence[str | os.PathLike[str]],
    *,
    capture_output: bool,
    text: bool,
    env: Mapping[str, str] | None,
    timeout: float | None,
    cwd: str | os.PathLike[str] | None,
    check: bool,
) -> subprocess.CompletedProcess[str | bytes]:
    resolved = _resolved_command(command, cwd=cwd, env=env)
    process = await asyncio.create_subprocess_exec(
        *resolved,
        stdout=asyncio.subprocess.PIPE if capture_output else None,
        stderr=asyncio.subprocess.PIPE if capture_output else None,
        env=None if env is None else dict(env),
        cwd=cwd,
    )
    try:
        if timeout is None:
            stdout_bytes, stderr_bytes = await process.communicate()
        else:
            stdout_bytes, stderr_bytes = await asyncio.wait_for(process.communicate(), timeout)
    except TimeoutError:
        if process.returncode is None:
            process.kill()
        stdout_bytes, stderr_bytes = await process.communicate()
        if timeout is None:
            raise RuntimeError("command timed out without a timeout value") from None
        raise subprocess.TimeoutExpired(
            resolved,
            timeout,
            output=_decoded(stdout_bytes, text=text),
            stderr=_decoded(stderr_bytes, text=text),
        ) from None

    stdout = _decoded(stdout_bytes, text=text)
    stderr = _decoded(stderr_bytes, text=text)
    returncode = process.returncode
    if returncode is None:
        raise RuntimeError("command exited without a return code")
    if check and returncode:
        raise subprocess.CalledProcessError(
            returncode,
            resolved,
            output=stdout,
            stderr=stderr,
        )
    return subprocess.CompletedProcess(resolved, returncode, stdout, stderr)


@overload
def run_command(
    command: Sequence[str | os.PathLike[str]],
    *,
    capture_output: bool = False,
    text: Literal[True],
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
    cwd: str | os.PathLike[str] | None = None,
    check: bool = False,
) -> subprocess.CompletedProcess[str]: ...


@overload
def run_command(
    command: Sequence[str | os.PathLike[str]],
    *,
    capture_output: bool = False,
    text: Literal[False] = False,
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
    cwd: str | os.PathLike[str] | None = None,
    check: bool = False,
) -> subprocess.CompletedProcess[bytes]: ...


def run_command(
    command: Sequence[str | os.PathLike[str]],
    *,
    capture_output: bool = False,
    text: bool = False,
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
    cwd: str | os.PathLike[str] | None = None,
    check: bool = False,
) -> subprocess.CompletedProcess[Any]:
    """Run a validated executable without a shell, mirroring subprocess.run."""
    return asyncio.run(
        _run_command_async(
            command,
            capture_output=capture_output,
            text=text,
            env=env,
            timeout=timeout,
            cwd=cwd,
            check=check,
        )
    )
