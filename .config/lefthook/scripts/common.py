"""Bounded process execution and isolated Git snapshots for local checks."""

import asyncio
import contextlib
import os
import shutil
import signal
import tempfile
import time
from pathlib import Path

MAX_TIMEOUT_SECONDS = 60
PROCESS_STOP_TIMEOUT_SECONDS = 5
READ_CHUNK_BYTES = 64 * 1024


class HookError(Exception):
    """An actionable local gate failure."""


def _resolve_argv(args, cwd, env):
    """Resolve argv[0] once so hook execution cannot be redirected through PATH races."""
    if not args:
        raise HookError("checkpoint command argv is empty")
    argv = [os.fspath(argument) for argument in args]
    program = Path(argv[0])
    if program.is_absolute():
        executable = program
    elif program.parent != Path():
        base = Path.cwd() if cwd is None else Path(cwd)
        executable = (base / program).resolve()
    else:
        search_path = env.get("PATH") if env is not None else None
        found = shutil.which(argv[0], path=search_path)
        if found is None:
            raise HookError(f"checkpoint executable not found: {argv[0]}")
        executable = Path(found).resolve()
    return [str(executable), *argv[1:]]


def _kill_bounded(process):
    """Kill a bounded child and everything it started.

    ``run_bounded`` starts children with ``start_new_session=True``, so on POSIX the whole
    process group is killed -- a bounded command that forks must not leave orphans behind. That
    call is POSIX-only: on Windows ``os.killpg`` does not exist, and the timeout path raised
    ``AttributeError`` instead of reporting that a process had exceeded its bound. The failure
    therefore appeared only when a gate was already failing, which is the worst time to lose the
    reason.

    Windows gets ``Popen.kill``, which terminates the child itself. Grandchildren are not reaped,
    and that is a real difference rather than a hidden one: a full equivalent needs
    ``CREATE_NEW_PROCESS_GROUP`` at spawn plus ``CTRL_BREAK_EVENT`` here, which is worth doing
    when a bounded command on Windows is observed to fork.
    """
    try:
        if hasattr(os, "killpg"):
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
    except (ProcessLookupError, PermissionError):
        pass  # The process or group already exited.


async def _stop_bounded(process):
    _kill_bounded(process)
    try:
        await asyncio.wait_for(process.wait(), PROCESS_STOP_TIMEOUT_SECONDS)
    except TimeoutError as error:
        raise HookError("checkpoint command could not be stopped") from error


async def _read_bounded(stream, output, state, maximum):
    while True:
        chunk = await stream.read(READ_CHUNK_BYTES)
        if not chunk:
            return
        state["total"] += len(chunk)
        if state["total"] > maximum:
            raise HookError("checkpoint command output exceeded its byte limit")
        output.extend(chunk)


async def _bounded_output(process, timeout, maximum):
    """Drain both asyncio pipes concurrently under one deadline and byte cap."""
    deadline = time.monotonic() + timeout
    stdout = bytearray()
    stderr = bytearray()
    state = {"total": 0}
    try:
        await asyncio.wait_for(
            asyncio.gather(
                _read_bounded(process.stdout, stdout, state, maximum),
                _read_bounded(process.stderr, stderr, state, maximum),
            ),
            timeout,
        )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise HookError("checkpoint command timed out")
        await asyncio.wait_for(process.wait(), remaining)
    except TimeoutError as error:
        raise HookError("checkpoint command timed out") from error
    return bytes(stdout)


async def _run_bounded_async(args, cwd, timeout, max_output, env, allowed):
    argv = _resolve_argv(args, cwd, env)
    try:
        process = await asyncio.create_subprocess_exec(
            *argv,
            cwd=cwd,
            env=env,
            start_new_session=True,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        try:
            stdout = await _bounded_output(process, timeout, max_output)
        except BaseException:
            await _stop_bounded(process)
            raise
    except OSError as error:
        raise HookError(f"{args[0]} checkpoint process failed ({type(error).__name__})") from error
    if process.returncode not in allowed:
        raise HookError(f"{args[0]} exited {process.returncode}; checkpoint unverified")
    return stdout


def run_bounded(args, cwd=None, *, timeout=10, max_output=1024 * 1024, env=None, allowed=(0,)):
    """Bound both streams during capture; never copy credential-bearing diagnostics."""
    if not 0 < timeout <= MAX_TIMEOUT_SECONDS or not 0 < max_output <= 1024 * 1024:
        raise HookError("invalid checkpoint process bounds")
    return asyncio.run(_run_bounded_async(args, cwd, timeout, max_output, env, allowed))


async def _run_async(args, cwd, data, timeout, capture, env, allowed):
    argv = _resolve_argv(args, cwd, env)
    try:
        process = await asyncio.create_subprocess_exec(
            *argv,
            cwd=cwd,
            env=env,
            start_new_session=True,
            stdin=asyncio.subprocess.PIPE if data is not None else None,
            stdout=asyncio.subprocess.PIPE if capture else None,
            stderr=asyncio.subprocess.PIPE if capture else None,
        )
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(input=data), timeout)
        except (TimeoutError, KeyboardInterrupt):
            await _stop_bounded(process)
            raise
    except (OSError, TimeoutError) as error:
        raise HookError(f"{args[0]}: {error}") from error
    if process.returncode not in allowed:
        output = (stdout or b"") + (stderr or b"")
        raise HookError(
            f"{' '.join(map(str, args))} exited {process.returncode}\n"
            + output.decode(errors="replace")
        )
    return stdout or b""


def run(args, cwd=None, *, data=None, timeout=180, capture=True, env=None, allowed=(0,)):
    """Execute argv without a shell; preserve failures and bound every process."""
    settings = dict(os.environ if env is None else env)
    settings["PYTHONDONTWRITEBYTECODE"] = "1"
    return asyncio.run(_run_async(args, cwd, data, timeout, capture, settings, allowed))


def git(*args, cwd=None):
    return run(["git", *args], cwd=cwd)


def paths(raw):
    return [os.fsdecode(item) for item in raw.split(b"\0") if item]


def changed(base, head="HEAD"):
    return paths(git("diff", "--name-only", "-z", "--no-renames", base, head, "--"))


def clean_env():
    env = dict(os.environ)
    for key in (
        "GIT_DIR",
        "GIT_WORK_TREE",
        "GIT_INDEX_FILE",
        "GIT_COMMON_DIR",
        "GIT_OBJECT_DIRECTORY",
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    ):
        env.pop(key, None)
    env.update(CI="true", GOWORK="off", GOFLAGS="-mod=readonly")
    return env


@contextlib.contextmanager
def snapshot(ref=None):
    """Export the exact index or commit; never stash, stage, or edit the source."""
    with tempfile.TemporaryDirectory(prefix="praetor-hook-") as directory:
        dest = Path(directory)
        if ref is None:
            git("checkout-index", "--all", "--force", f"--prefix={dest}/")
            # Lefthook's validator requires a repository even though it only
            # validates configuration. This metadata belongs solely to the export.
            run(["git", "init", "--quiet", str(dest)], env=clean_env())
        else:
            source = git("rev-parse", "--show-toplevel").decode().strip()
            env = clean_env()
            origin = (
                run(["git", "config", "--get", "remote.origin.url"], allowed=(0, 1))
                .decode()
                .strip()
            )
            refs = git("for-each-ref", "--format=%(objectname) %(refname)", "refs/remotes/origin/")
            run(
                [
                    "git",
                    "clone",
                    "--quiet",
                    "--no-hardlinks",
                    "--no-checkout",
                    "--origin",
                    "praetor-snapshot",
                    source,
                    str(dest),
                ],
                env=env,
            )
            if origin:
                run(["git", "remote", "add", "origin", origin], cwd=dest, env=env)
            for line in refs.decode().splitlines():
                oid, name = line.split()
                run(["git", "update-ref", name, oid], cwd=dest, env=env)
            run(["git", "checkout", "--quiet", "--detach", ref], cwd=dest, env=env)
        yield dest


def present_files(directory, names):
    files = []
    for name in names:
        path = directory / name
        if path.is_symlink():
            raise HookError(f"{name}: changed symlinks require explicit review")
        if path.is_file():
            files.append(name)
    return files
