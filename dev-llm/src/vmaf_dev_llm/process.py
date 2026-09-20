# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Shell-free, bounded process execution for developer CLI helpers."""

from __future__ import annotations

import asyncio
import locale
import math
import os
import shutil
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Literal, overload

DEFAULT_TIMEOUT_SECONDS = 30


def _output(data: bytes, *, text: bool) -> bytes | str:
    if not text:
        return data
    return data.decode(locale.getpreferredencoding(False))


def _resolved_argv(
    command: Sequence[str | os.PathLike[str]],
    *,
    cwd: str | os.PathLike[str] | None,
    env: Mapping[str, str] | None,
) -> list[str]:
    if not command:
        raise ValueError("command must not be empty")
    argv = [os.fspath(argument) for argument in command]
    program = Path(argv[0])
    if program.is_absolute():
        executable = program
    elif program.parent != Path():
        base = Path.cwd() if cwd is None else Path(cwd)
        executable = (base / program).resolve()
    else:
        found = shutil.which(argv[0], path=env.get("PATH") if env is not None else None)
        if found is None:
            raise FileNotFoundError(f"executable not found: {argv[0]}")
        executable = Path(found).resolve()
    return [str(executable), *argv[1:]]


async def _checked_output_async(
    command: Sequence[str | os.PathLike[str]],
    *,
    text: bool,
    timeout: float,
    cwd: str | os.PathLike[str] | None,
    env: Mapping[str, str] | None,
) -> bytes | str:
    argv = _resolved_argv(command, cwd=cwd, env=env)
    process = await asyncio.create_subprocess_exec(
        *argv,
        cwd=cwd,
        env=None if env is None else dict(env),
        stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    communication = asyncio.create_task(process.communicate())
    try:
        stdout, stderr = await asyncio.wait_for(asyncio.shield(communication), timeout)
    except TimeoutError:
        if not communication.done() and process.returncode is None:
            process.kill()
        stdout, stderr = await communication
        raise subprocess.TimeoutExpired(
            argv,
            timeout,
            output=_output(stdout, text=text),
            stderr=_output(stderr, text=text),
        ) from None
    if process.returncode:
        raise subprocess.CalledProcessError(
            process.returncode,
            argv,
            output=_output(stdout, text=text),
            stderr=_output(stderr, text=text),
        )
    return _output(stdout, text=text)


@overload
def checked_output(
    command: Sequence[str | os.PathLike[str]],
    *,
    text: Literal[True],
    timeout: float = DEFAULT_TIMEOUT_SECONDS,
    cwd: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
) -> str: ...


@overload
def checked_output(
    command: Sequence[str | os.PathLike[str]],
    *,
    text: Literal[False] = False,
    timeout: float = DEFAULT_TIMEOUT_SECONDS,
    cwd: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
) -> bytes: ...


def checked_output(
    command: Sequence[str | os.PathLike[str]],
    *,
    text: bool = False,
    timeout: float = DEFAULT_TIMEOUT_SECONDS,
    cwd: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
) -> bytes | str:
    """Run a resolved argv without a shell and return checked stdout."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be positive and finite")
    return asyncio.run(_checked_output_async(command, text=text, timeout=timeout, cwd=cwd, env=env))
