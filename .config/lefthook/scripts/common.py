"""Bounded process execution and isolated Git snapshots for local checks."""

import contextlib
import os
from pathlib import Path
import subprocess
import signal
import selectors
import tempfile
import time


class HookError(Exception):
    """An actionable local gate failure."""


def _stop_bounded(process):
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass  # The group already exited.
    process.wait(timeout=5)


def _bounded_output(process, timeout, maximum):
    deadline = time.monotonic() + timeout
    output = {"stdout": bytearray(), "stderr": bytearray()}
    total = 0
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ, "stdout")
        selector.register(process.stderr, selectors.EVENT_READ, "stderr")
        # Every iteration either consumes a byte, observes EOF, or waits to deadline.
        for _ in range(maximum + 3):
            if not selector.get_map():
                process.wait(timeout=max(0.001, deadline - time.monotonic()))
                return bytes(output["stdout"])
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise HookError("checkpoint command timed out")
            events = selector.select(remaining)
            if not events:
                raise HookError("checkpoint command timed out")
            for key, _mask in events:
                chunk = os.read(key.fileobj.fileno(), min(65536, maximum - total + 1))
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                total += len(chunk)
                if total > maximum:
                    raise HookError("checkpoint command output exceeded its byte limit")
                output[key.data].extend(chunk)
    raise HookError("checkpoint command exceeded its read bound")


def run_bounded(args, cwd=None, *, timeout=10, max_output=1024 * 1024,
                env=None, allowed=(0,)):
    """Bound both streams during capture; never copy credential-bearing diagnostics."""
    if not 0 < timeout <= 60 or not 0 < max_output <= 1024 * 1024:
        raise HookError("invalid checkpoint process bounds")
    try:
        with subprocess.Popen(args, cwd=cwd, env=env, start_new_session=True,
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE) as process:
            try:
                stdout = _bounded_output(process, timeout, max_output)
            except BaseException:
                _stop_bounded(process)
                raise
            if process.returncode not in allowed:
                raise HookError(f"{args[0]} exited {process.returncode}; checkpoint unverified")
            return stdout
    except (OSError, subprocess.TimeoutExpired) as error:
        raise HookError(f"{args[0]} checkpoint process failed ({type(error).__name__})") from error


def run(args, cwd=None, *, data=None, timeout=180, capture=True, env=None, allowed=(0,)):
    """Execute argv without a shell; preserve failures and bound every process."""
    settings = dict(os.environ if env is None else env)
    settings["PYTHONDONTWRITEBYTECODE"] = "1"
    try:
        with subprocess.Popen(args, cwd=cwd, env=settings, start_new_session=True,
                              stdin=subprocess.PIPE if data is not None else None,
                              stdout=subprocess.PIPE if capture else None,
                              stderr=subprocess.PIPE if capture else None) as process:
            try:
                stdout, stderr = process.communicate(input=data, timeout=timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass  # The group exited between timeout detection and cleanup.
                process.communicate(timeout=5)
                raise
    except (OSError, subprocess.TimeoutExpired) as error:
        raise HookError(f"{args[0]}: {error}") from error
    if process.returncode not in allowed:
        output = (stdout or b"") + (stderr or b"")
        raise HookError(f"{' '.join(map(str, args))} exited {process.returncode}\n"
                        + output.decode(errors="replace"))
    return stdout or b""


def git(*args, cwd=None):
    return run(["git", *args], cwd=cwd)


def paths(raw):
    return [os.fsdecode(item) for item in raw.split(b"\0") if item]


def changed(base, head="HEAD"):
    return paths(git("diff", "--name-only", "-z", "--no-renames", base, head, "--"))


def clean_env():
    env = dict(os.environ)
    for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
                "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES"):
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
            origin = run(["git", "config", "--get", "remote.origin.url"], allowed=(0, 1)).decode().strip()
            refs = git("for-each-ref", "--format=%(objectname) %(refname)", "refs/remotes/origin/")
            run(["git", "clone", "--quiet", "--no-hardlinks", "--no-checkout",
                 "--origin", "praetor-snapshot", source, str(dest)], env=env)
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
