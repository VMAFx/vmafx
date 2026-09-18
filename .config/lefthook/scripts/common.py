"""Bounded process execution and isolated Git snapshots for local checks."""

import contextlib
import os
from pathlib import Path
import subprocess
import signal
import selectors
import tempfile
import threading
import time


class HookError(Exception):
    """An actionable local gate failure."""


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


def _stop_bounded(process):
    _kill_bounded(process)
    process.wait(timeout=5)


class _SharedBound:
    """Output two reader threads collect under one byte limit."""

    def __init__(self, maximum):
        self.maximum = maximum
        self.output = {"stdout": bytearray(), "stderr": bytearray()}
        self.total = 0
        self.exceeded = False
        self.finished = 0
        self.changed = threading.Condition()

    def add(self, key, chunk):
        """Record a chunk; False once the limit is passed and reading must stop."""
        with self.changed:
            self.total += len(chunk)
            if self.total > self.maximum:
                self.exceeded = True
                self.changed.notify_all()
                return False
            self.output[key].extend(chunk)
            return True

    def finish(self):
        with self.changed:
            self.finished += 1
            self.changed.notify_all()


def _drain(stream, key, bound):
    # Every read consumes at least one byte or observes EOF, so maximum + 2 reads is enough.
    try:
        for _ in range(bound.maximum + 2):
            chunk = os.read(stream.fileno(), min(65536, bound.maximum + 1))
            if not chunk or not bound.add(key, chunk):
                return
    except OSError:
        return  # The pipe closed because the process was stopped.
    finally:
        bound.finish()


def _bounded_output_threaded(process, timeout, maximum):
    """Collect bounded output where select() cannot wait on pipes.

    On Windows ``selectors`` accepts only sockets, so registering a child's pipes raised
    ``OSError`` and every bounded command failed before it produced a byte -- including each git
    call the checkpoint planner makes. Each pipe is drained by its own thread instead, under the
    same shared byte limit and deadline; a reader never blocks the process on a full pipe, and a
    limit or deadline breach is raised for the caller to stop the process, as on POSIX.
    """
    deadline = time.monotonic() + timeout
    bound = _SharedBound(maximum)
    for stream, key in ((process.stdout, "stdout"), (process.stderr, "stderr")):
        threading.Thread(target=_drain, args=(stream, key, bound), daemon=True).start()
    with bound.changed:
        done = bound.changed.wait_for(lambda: bound.exceeded or bound.finished == 2,
                                      timeout=max(0.0, deadline - time.monotonic()))
        if bound.exceeded:
            raise HookError("checkpoint command output exceeded its byte limit")
        if not done:
            raise HookError("checkpoint command timed out")
    process.wait(timeout=max(0.001, deadline - time.monotonic()))
    return bytes(bound.output["stdout"])


def _bounded_output(process, timeout, maximum):
    if os.name == "nt":
        return _bounded_output_threaded(process, timeout, maximum)
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
                _kill_bounded(process)
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
