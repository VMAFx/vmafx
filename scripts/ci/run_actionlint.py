#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run actionlint with a deadline, so that it fails loudly instead of hanging.

actionlint (v1.7.12, ``process.go`` ``cmdExecution.run``) feeds the script of a
``run:`` block to shellcheck by writing it to the child's stdin pipe *before* the
child is started. That works while the pipe holds the whole script, and it
deadlocks when the script is larger than the pipe: the write blocks, the child
never starts, nothing reads. A pipe holds 64 KiB, except that the kernel shrinks
the pipes of a user who holds more than ``fs.pipe-user-pages-soft`` pages (16384
by default, 64 MiB) in total to 2 pages, so on a workstation with many builds
and agents running a script of 8 KiB or more hangs the whole hook, with no
output and no exit, for as long as the pressure lasts. Measured on 2026-10-07:
a user holding 1100 pipes made ``actionlint .github/workflows/lint-and-format.yml
.github/workflows/rule-enforcement.yml`` hang; without them it takes 0.1 s.

This wrapper runs actionlint under a deadline (``ACTIONLINT_TIMEOUT_S``, default
90 s; a healthy run takes about a second). At the deadline it sends SIGQUIT, which
makes a Go program print the stack of every goroutine to stderr, saves that dump
to a file it names (``ACTIONLINT_DUMP_DIR``, default the temporary directory:
the stack is the evidence an upstream report needs and a hook's captured output
is gone after the run; ``ACTIONLINT_DUMP_WAIT_S`` bounds the wait for it, default 10 s), terminates the whole process group, says why, and exits
124. It never reports a pass for a run that did not finish. Every other outcome
is actionlint's own: its output, its exit status. A missing actionlint is exit
127.

Usage: run_actionlint.py [actionlint arguments...]
"""

from __future__ import annotations

import contextlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from collections.abc import Sequence
from pathlib import Path

DEFAULT_TIMEOUT_S = 90.0
DUMP_WAIT_S = 10.0
KILL_WAIT_S = 5.0
MAX_DUMP_BYTES = 4 * 1024 * 1024
PIPE_SIZE_FCNTL = 1032  # F_GETPIPE_SZ on Linux
FULL_PIPE_BYTES = 65536
EXIT_TIMEOUT = 124
EXIT_MISSING = 127


def pipe_capacity() -> int | None:
    """Capacity in bytes of a pipe created now, or ``None`` where it cannot be read."""
    try:
        import fcntl  # noqa: PLC0415 - not available everywhere

        read_end, write_end = os.pipe()
    except (ImportError, OSError):
        return None
    try:
        return int(fcntl.fcntl(write_end, PIPE_SIZE_FCNTL))
    except OSError:
        return None
    finally:
        os.close(read_end)
        os.close(write_end)


def timeout_seconds(environ: dict[str, str]) -> float:
    raw = environ.get("ACTIONLINT_TIMEOUT_S", "")
    if not raw:
        return DEFAULT_TIMEOUT_S
    value = float(raw)
    if value <= 0:
        raise ValueError("ACTIONLINT_TIMEOUT_S must be positive")
    return value


def diagnosis(limit: float, capacity: int | None, dump: Path | None = None) -> str:
    lines = [
        f"run_actionlint: actionlint did not finish within {limit:g} s and was terminated; this is a FAILURE, not a pass.",
        "  actionlint writes the script of a `run:` block to shellcheck's stdin pipe before it starts shellcheck,",
        "  so it deadlocks when the script is larger than the pipe (rhysd/actionlint, process.go cmdExecution.run).",
    ]
    if capacity is None:
        lines.append("  The capacity of a new pipe could not be read on this platform.")
    elif capacity < FULL_PIPE_BYTES:
        lines.append(
            f"  A new pipe holds {capacity} bytes here (the default is {FULL_PIPE_BYTES}): this user holds more pipe"
            " pages than fs.pipe-user-pages-soft allows, so the kernel shrank new pipes. This is the cause."
        )
        lines.append(
            "  Find the holders (`ls -l /proc/*/fd | grep -c pipe`), stop the leaking process and run again."
        )
    else:
        lines.append(
            f"  A new pipe holds {capacity} bytes now, so the pressure may have passed: run again. If it repeats,"
            " attach the goroutine dump to the upstream report."
        )
    if dump is not None:
        lines.append(f"  The goroutine dump (SIGQUIT at the deadline) is saved in {dump}")
    else:
        lines.append("  No goroutine dump could be saved.")
    return "\n".join(lines)


def save_dump(text: str) -> Path | None:
    """Write the goroutine dump where a person can find it; ``None`` when it cannot be written."""
    directory = Path(os.environ.get("ACTIONLINT_DUMP_DIR") or tempfile.gettempdir())
    try:
        directory.mkdir(parents=True, exist_ok=True)
        handle, name = tempfile.mkstemp(prefix="actionlint-hang-", suffix=".txt", dir=directory)
        with os.fdopen(handle, "w", encoding="utf-8") as out:
            out.write(text[:MAX_DUMP_BYTES])
        return Path(name)
    except OSError:
        return None


def _to_text(data: str | bytes | None) -> str:
    if data is None:
        return ""
    if isinstance(data, bytes):
        return data.decode("utf-8", "replace")
    return data


def _drain(process: subprocess.Popen[str], wait_s: float) -> tuple[str, str]:
    try:
        out, err = process.communicate(timeout=wait_s)
        return out or "", err or ""
    except subprocess.TimeoutExpired as exc:
        return _to_text(exc.stdout), _to_text(exc.stderr)


def stop_hung(process: subprocess.Popen[str]) -> str:
    """SIGQUIT for the stack, then the whole process group; returns what stderr held."""
    dump = ""
    with contextlib.suppress(OSError):
        with contextlib.suppress(AttributeError):
            os.killpg(process.pid, signal.SIGQUIT)
        process.send_signal(signal.SIGQUIT)
        _out, dump = _drain(process, float(os.environ.get("ACTIONLINT_DUMP_WAIT_S") or DUMP_WAIT_S))
    with contextlib.suppress(OSError, AttributeError):
        os.killpg(process.pid, signal.SIGKILL)
    with contextlib.suppress(OSError):
        process.kill()
    try:
        _out, remaining_err = process.communicate(timeout=KILL_WAIT_S)
        if remaining_err:
            dump = (dump + remaining_err) if dump else remaining_err
    except (OSError, subprocess.SubprocessError):
        pass
    with contextlib.suppress(OSError, subprocess.SubprocessError):
        process.wait(timeout=KILL_WAIT_S)
    return dump


def run_bounded(executable: str, args: list[str], limit: float) -> tuple[int, str, str, bool]:
    """Run to completion or to the deadline: (status, stdout, stderr, timed_out)."""
    process = subprocess.Popen(  # noqa: S603 - resolved executable, arguments from the hook
        [executable, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    deadline = time.monotonic() + limit
    try:
        out, err = process.communicate(timeout=max(0.0, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        return EXIT_TIMEOUT, "", stop_hung(process), True
    return process.returncode, out, err, False


def main(argv: Sequence[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    binary = os.environ.get("ACTIONLINT_BIN") or shutil.which("actionlint")
    if not binary:
        print("run_actionlint: actionlint is not on PATH (set ACTIONLINT_BIN)", file=sys.stderr)
        return EXIT_MISSING
    try:
        limit = timeout_seconds(dict(os.environ))
    except ValueError as exc:
        print(f"run_actionlint: {exc}", file=sys.stderr)
        return 2
    executable = str(Path(binary).resolve(strict=True))
    status, out, err, timed_out = run_bounded(executable, args, limit)
    if timed_out:
        dump = save_dump(err) if err else None
        print(diagnosis(limit, pipe_capacity(), dump), file=sys.stderr)
        return EXIT_TIMEOUT
    sys.stdout.write(out)
    sys.stderr.write(err)
    return status


if __name__ == "__main__":
    sys.exit(main())
