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
import contextlib
import ctypes
import errno
import locale
import os
import platform
import shutil
import signal
import stat
import sys
from collections import deque
from collections.abc import Collection, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any, Literal, overload

if sys.platform.startswith("linux") or sys.platform == "darwin":
    import fcntl

MAX_ARG_COUNT = 4096
MAX_ARG_BYTES = 65_536
MAX_ARGV_BYTES = 1_048_576
MAX_EXECUTABLE_LINK_HOPS = 40
MAX_EXECUTABLE_WALK_STEPS = 4096
MAX_DARWIN_ACL_ENTRIES = 128
_DARWIN_ACL_EXTENDED_DENY = 2
_DARWIN_ACL_TYPE_EXTENDED = 0x100
# Apple xnu bsd/sys/stat.h; fallback values verified in Python's stat documentation.
_DARWIN_REPLACEMENT_PROTECTION = (
    getattr(stat, "UF_IMMUTABLE", 0x00000002)
    | getattr(stat, "UF_APPEND", 0x00000004)
    | getattr(stat, "SF_IMMUTABLE", 0x00020000)
    | getattr(stat, "SF_APPEND", 0x00040000)
    | getattr(stat, "SF_NOUNLINK", 0x00100000)
    | getattr(stat, "SF_RESTRICTED", 0x00080000)
)
# Linux include/uapi/linux/fs.h: FS_IMMUTABLE_FL | FS_APPEND_FL.
_LINUX_REPLACEMENT_PROTECTION = 0x10 | 0x20
# uname -m spellings: arch/arm/kernel/setup.c appends the endianness letter;
# arm64 compat reports armv8l (arch/arm64/include/asm/compat.h).
_LINUX_GENERIC_IOCTL_MACHINES = frozenset(
    ("x86_64", "aarch64", "riscv64", "armv6l", "armv7l", "armv8l", "i686")
)
DEFAULT_TIMEOUT_SECONDS = 120.0
DEFAULT_MAX_OUTPUT_BYTES = 4 * 1_048_576
TERMINATION_GRACE_SECONDS = 2.0
WINDOWS_TREE_KILL_TIMEOUT_SECONDS = 30.0
READ_CHUNK_BYTES = 65_536
_WINDOWS_CREATE_NEW_PROCESS_GROUP = 0x00000200

Arg = str | os.PathLike[str]


class CommandValidationError(ValueError):
    """The requested command does not satisfy the execution contract."""


@dataclass(frozen=True)
class CommandResult:
    """Small immutable result returned by :func:`run`.

    Streams that were not captured are represented by an empty value of the
    selected text or binary type.  This keeps every successful result safe to
    inspect without weakening its type to ``None``.
    """

    argv: tuple[str, ...]
    returncode: int
    stdout: str | bytes
    stderr: str | bytes

    def check_returncode(self) -> None:
        """Raise :class:`CommandFailed` when the child did not succeed."""
        if self.returncode != 0:
            raise CommandFailed(self)


class TextCommandResult(CommandResult):
    """Result whose captured streams were decoded to text."""

    stdout: str
    stderr: str


class BinaryCommandResult(CommandResult):
    """Result whose captured streams remain bytes."""

    stdout: bytes
    stderr: bytes


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
class _RunSettings:
    """The per-call options that outlive the argument validation."""

    text: bool
    errors: str
    check: bool
    timeout_seconds: float
    max_output_bytes: int


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


def _executable_links(launch: Path) -> tuple[Path, ...]:
    """Collect links in kernel traversal order, including links in target directories.

    POSIX callers preserve trusted links; platforms without getuid launch the resolved file.
    """
    pending = deque(launch.parts)
    directory = Path(launch.anchor)
    links: list[Path] = []
    for _ in range(MAX_EXECUTABLE_WALK_STEPS):
        if not pending:
            return tuple(links)
        component = pending.popleft()
        if component == launch.anchor:
            directory = Path(launch.anchor)
            continue
        if component == "..":
            directory = directory.parent
            continue
        candidate = directory / component
        try:
            metadata = candidate.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                links.append(candidate)
                if len(links) > MAX_EXECUTABLE_LINK_HOPS:
                    raise CommandValidationError(
                        f"executable exceeds MAX_EXECUTABLE_LINK_HOPS={MAX_EXECUTABLE_LINK_HOPS}"
                    )
                pending.extendleft(reversed(candidate.readlink().parts))
            else:
                directory = candidate
        except OSError as exc:
            raise CommandValidationError(f"executable is unavailable: {candidate}") from exc
    if pending:
        raise CommandValidationError(
            f"executable exceeds MAX_EXECUTABLE_WALK_STEPS={MAX_EXECUTABLE_WALK_STEPS}"
        )
    return tuple(links)


def _darwin_acl_library() -> ctypes.CDLL:
    """Declare the native extended-ACL interface before inspecting directory entries."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.acl_get_link_np.argtypes = [ctypes.c_char_p, ctypes.c_int]
    libc.acl_get_link_np.restype = ctypes.c_void_p
    libc.acl_get_entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]
    libc.acl_get_entry.restype = ctypes.c_int
    libc.acl_get_tag_type.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
    libc.acl_get_tag_type.restype = ctypes.c_int
    libc.acl_free.argtypes = [ctypes.c_void_p]
    libc.acl_free.restype = ctypes.c_int
    return libc


def _darwin_acl_entries_allow(libc: ctypes.CDLL, acl: int) -> bool:
    """Reject allow entries, unreadable tags and ACLs exceeding the native entry bound."""
    entry = ctypes.c_void_p()
    tag = ctypes.c_int()
    for index in range(MAX_DARWIN_ACL_ENTRIES + 1):
        ctypes.set_errno(0)
        status = libc.acl_get_entry(acl, 0 if index == 0 else -1, ctypes.byref(entry))
        if status != 0:
            return status != -1 or ctypes.get_errno() != errno.EINVAL
        if index == MAX_DARWIN_ACL_ENTRIES:
            return True
        if (
            libc.acl_get_tag_type(entry, ctypes.byref(tag)) != 0
            or tag.value != _DARWIN_ACL_EXTENDED_DENY
        ):
            return True
    return True


def _darwin_acl_allows_entries(path: Path) -> bool:
    """Fail closed unless a directory has no extended ACL or only readable deny entries."""
    try:
        libc = _darwin_acl_library()
    except (OSError, AttributeError):
        return True
    ctypes.set_errno(0)
    acl = libc.acl_get_link_np(os.fsencode(path), _DARWIN_ACL_TYPE_EXTENDED)
    if not acl:
        return ctypes.get_errno() != errno.ENOENT
    verdict = True
    try:
        verdict = _darwin_acl_entries_allow(libc, acl)
    finally:
        if libc.acl_free(acl) != 0:
            verdict = True
    return verdict


def _directory_allows_link_retargeting(directory: os.stat_result, path: Path) -> bool:
    """Reject macOS ACL grants and nonsticky group/other directory write permission."""
    if sys.platform == "darwin" and _darwin_acl_allows_entries(path):
        return True
    return not directory.st_mode & stat.S_ISVTX and bool(directory.st_mode & 0o022)


def _darwin_entry_protected(entry: Path) -> bool | None:
    """Read the entry's own unlink/rename protections; unreadable flags stay unknown."""
    try:
        return bool(os.lstat(entry).st_flags & _DARWIN_REPLACEMENT_PROTECTION)
    except (OSError, AttributeError):
        return None


def _linux_file_protected(entry: Path) -> bool | None:
    """Read Linux inode protections on machines using the generic ioctl encoding."""
    if platform.machine() not in _LINUX_GENERIC_IOCTL_MACHINES:
        return None
    # include/uapi/linux/fs.h: _IOR('f', 1, long); asm-generic/ioctl.h: _IOC_READ=2.
    size = ctypes.sizeof(ctypes.c_long)
    request = (2 << 30) | (size << 16) | (ord("f") << 8) | 1
    try:
        fd = os.open(entry, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            try:
                flags = fcntl.ioctl(fd, request, bytes(size))
            except OSError as exc:
                if exc.errno in (errno.ENOTTY, errno.EOPNOTSUPP, errno.ENOSYS, errno.EINVAL):
                    return False
                return None
        finally:
            os.close(fd)
    except OSError:
        return None
    if not isinstance(flags, bytes) or len(flags) != size:
        return None
    return bool(int.from_bytes(flags, sys.byteorder) & _LINUX_REPLACEMENT_PROTECTION)


def _executable_entry_protected(entry: Path, *, symlink: bool = False) -> bool | None:
    """Read replacement protection; Linux symlinks have no inode protection flags."""
    if sys.platform == "darwin":
        return _darwin_entry_protected(entry)
    return False if symlink else _linux_file_protected(entry)


def _executable_link_trusted(
    link: Path,
    resolved_directory: os.stat_result,
    trusted_owners: tuple[int, int],
    resolved_protected: bool | None,
) -> bool:
    """Check owners and rights, exempting shared directories with matching exposure."""
    try:
        owner = link.lstat().st_uid
        directory = link.parent.stat()
    except OSError:
        return False
    if owner not in trusted_owners or directory.st_uid not in trusted_owners:
        return False
    same_directory = (directory.st_dev, directory.st_ino) == (
        resolved_directory.st_dev,
        resolved_directory.st_ino,
    )
    if same_directory and (
        resolved_protected is False
        or (resolved_protected is True and _executable_entry_protected(link, symlink=True) is True)
    ):
        return True
    return not _directory_allows_link_retargeting(directory, link.parent)


def _executable_links_trusted(links: tuple[Path, ...], resolved: Path) -> bool:
    """Check every link's ownership, reading resolved-file protection once per launch."""
    if not (sys.platform.startswith("linux") or sys.platform == "darwin"):
        return False
    try:
        resolved_directory = resolved.parent.stat()
    except OSError:
        return False
    trusted_owners = (0, os.getuid())
    resolved_protected = _executable_entry_protected(resolved) if links else None
    return all(
        _executable_link_trusted(link, resolved_directory, trusted_owners, resolved_protected)
        for link in links
    )


def _resolve_launch_file(launch: Path) -> tuple[Path, Path]:
    """Walk before strict resolution, then evaluate link trust against the resolved file."""
    links = _executable_links(launch) if hasattr(os, "getuid") else ()
    resolved = launch.resolve(strict=True)
    trusted = hasattr(os, "getuid") and _executable_links_trusted(links, resolved)
    return (launch if trusted else resolved), resolved


def _resolve_absolute_executable(
    executable: str,
    requested: Path,
    allowed: _AllowedExecutables,
    search_path: str,
) -> tuple[Path, Path]:
    """Select a launch path and require its resolved file to match the allowlist."""
    try:
        launch, resolved = _resolve_launch_file(requested)
    except OSError as exc:
        raise CommandValidationError(f"executable is unavailable: {executable}") from exc
    name_matches: set[Path] = set()
    for name in allowed.names:
        found = shutil.which(name, path=search_path)
        if found is not None:
            name_matches.add(Path(found).resolve(strict=True))
    if resolved not in allowed.paths and resolved not in name_matches:
        raise CommandValidationError(f"executable is not allowlisted: {resolved}")
    return launch, resolved


def _checked_executable(
    executable: str, allowed_values: Collection[Arg], search_path: str
) -> tuple[str, Path]:
    """Validate the resolved file and preserve trusted POSIX links for venv discovery.

    Untrusted chains and platforms without os.getuid launch the resolved file instead.
    """
    allowed = _parse_allowed_executables(allowed_values)

    requested = Path(executable).expanduser()
    if requested.is_absolute():
        launch, resolved = _resolve_absolute_executable(executable, requested, allowed, search_path)
    elif len(requested.parts) == 1:
        if executable not in allowed.names:
            raise CommandValidationError(f"executable name is not allowlisted: {executable}")
        found = shutil.which(executable, path=search_path)
        if found is None:
            raise CommandValidationError(f"executable is unavailable: {executable}")
        # Keep PATH links for trust validation before selecting the launch path.
        launch, resolved = _resolve_launch_file(Path(found).absolute())
    else:
        raise CommandValidationError(f"executable path must be absolute or bare: {executable}")

    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise CommandValidationError(f"executable is not a runnable file: {resolved}")
    return str(launch), resolved


def _resolve_allowed_executable(
    executable: str, allowed_values: Collection[Arg], search_path: str
) -> str:
    """Return the allowlisted executable file for callers that need its canonical path."""
    return str(_checked_executable(executable, allowed_values, search_path)[1])


def executable_argument(executable: Arg, root: Arg) -> str:
    """Return `executable` as `run()` accepts it, resolving a relative path against `root`.

    `run()` takes a bare name (looked up on PATH) or an absolute path and refuses
    a relative path with a directory part. A caller that reads a path such as
    `core/build/tools/vmaf` from an option or the environment passes it through
    here: a bare name stays as it is, an absolute path stays as it is, and any
    other non-absolute value is made absolute against `root`.
    The final name is kept, not resolved: `vmafx` is a symlink to `vmaf`, and
    the CLI picks its mode from the name it was started as.
    """
    text = _stringify(executable, label="executable")
    path = Path(text).expanduser()
    if path.is_absolute():
        return str(path)
    separators = {"/", os.sep, os.altsep or os.sep}
    if text not in (".", "..") and not any(sep in text for sep in separators):
        return text
    # absolute() + normpath(), not resolve(): the final symlink keeps its name.
    return os.path.normpath((Path(root) / path).absolute())


def resolve_executable(executable: Arg, root: Arg | None = None) -> str:
    """Return the absolute, runnable path `run()` would execute for `executable`.

    Resolves a bare name on PATH or a relative path against `root` (defaulting
    to the current working directory).

    Returns:
        The validated, absolute path to the runnable executable file.

    Raises:
        CommandValidationError: If the executable is missing, not a regular file,
            or not executable.
    """
    value = executable_argument(executable, root if root is not None else Path.cwd())
    return _resolve_allowed_executable(value, (value,), os.environ.get("PATH", os.defpath))


def _validated_argv(
    argv: Sequence[Arg], allowed_executables: Collection[Arg], search_path: str
) -> tuple[str, ...]:
    """Validate bounded arguments and select the trusted absolute launch path."""
    if not argv:
        raise CommandValidationError("argv must contain an executable")
    if len(argv) > MAX_ARG_COUNT:
        raise CommandValidationError(f"argv exceeds {MAX_ARG_COUNT} entries")
    values = tuple(_stringify(value, label=f"argv[{index}]") for index, value in enumerate(argv))
    if sum(len(os.fsencode(value)) + 1 for value in values) > MAX_ARGV_BYTES:
        raise CommandValidationError(f"argv exceeds {MAX_ARGV_BYTES} encoded bytes")
    executable = _checked_executable(values[0], allowed_executables, search_path)[0]
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
    settings: _RunSettings,
    encoding: str | None,
) -> _RunConfiguration:
    _validate_run_options(
        input_data=input_data,
        capture_output=capture_output,
        stdout=stdout,
        stderr=stderr,
        stderr_to_stdout=stderr_to_stdout,
        text=settings.text,
        timeout_seconds=settings.timeout_seconds,
        max_output_bytes=settings.max_output_bytes,
    )
    errors = settings.errors
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
    """Close child input after writing, tolerating a child that closes its pipe early."""
    try:
        writer.write(data)
        await writer.drain()
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        writer.close()


async def _terminate_windows_process_tree(pid: int) -> bool:
    """Terminate the process tree rooted at `pid` on Windows using taskkill.

    Returns:
        True when taskkill exits with status 0; False when taskkill fails,
        times out, or is unavailable.
    """
    try:
        taskkill = resolve_executable("taskkill")
    except CommandValidationError:
        return False
    try:
        process = await asyncio.create_subprocess_exec(
            taskkill,
            "/PID",
            str(pid),
            "/T",
            "/F",
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL,
        )
    except OSError:
        return False
    try:
        await asyncio.wait_for(process.wait(), timeout=WINDOWS_TREE_KILL_TIMEOUT_SECONDS)
    except TimeoutError:
        with contextlib.suppress(OSError):
            process.kill()
        with contextlib.suppress(TimeoutError, OSError):
            await asyncio.wait_for(process.wait(), timeout=TERMINATION_GRACE_SECONDS)
        return False
    except OSError:
        return False
    return process.returncode == 0


def terminate_windows_process_tree(pid: int) -> bool:
    """Terminate the process tree rooted at `pid` on Windows using taskkill.

    Returns:
        True when taskkill exits with status 0; False when taskkill fails,
        times out, or is unavailable.

    Raises:
        CommandValidationError: When `pid` is not a positive integer.
    """
    if isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0:
        raise CommandValidationError(f"invalid process id: {pid!r}")
    return asyncio.run(_terminate_windows_process_tree(pid))


async def _terminate_process_group(process: asyncio.subprocess.Process) -> None:
    """End the child's process group on POSIX, and on Windows end the process tree first, then
    terminate, kill and reap the direct child.
    """
    if os.name == "posix":
        await _terminate_posix_process_group(process)
        return
    if process.returncode is not None:
        return
    await _terminate_windows_process_tree(process.pid)
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


def signal_process_group(group_id: object, selected_signal: int) -> bool:
    """Signal one child's process group; refuse every target that is not one.

    ``killpg(1, sig)`` is ``kill(-1, sig)`` and reaches every process the user
    owns, ``killpg(0, sig)`` reaches the caller's own group, and a test double's
    pid can index to 1 (``operator.index(MagicMock().pid)``). Such a target
    raises :class:`ValueError` before anything is sent. Returns ``False`` when
    the group no longer exists.
    """
    if (
        not isinstance(group_id, int)
        or isinstance(group_id, bool)
        or group_id <= 1
        or group_id == os.getpgrp()
    ):
        raise ValueError(f"refusing to signal process group {group_id!r}")
    try:
        os.killpg(group_id, selected_signal)
    except ProcessLookupError:
        return False
    return True


async def _terminate_posix_process_group(process: asyncio.subprocess.Process) -> None:
    """Terminate the whole session even when its original leader already exited."""
    if not signal_process_group(process.pid, signal.SIGTERM):
        return

    loop = asyncio.get_running_loop()
    deadline = loop.time() + TERMINATION_GRACE_SECONDS
    while _posix_process_group_exists(process.pid) and loop.time() < deadline:
        await asyncio.sleep(min(0.05, max(0.0, deadline - loop.time())))
    if _posix_process_group_exists(process.pid):
        signal_process_group(process.pid, signal.SIGKILL)
    if process.returncode is None:
        try:
            await asyncio.wait_for(process.wait(), timeout=TERMINATION_GRACE_SECONDS)
        except TimeoutError as exc:
            raise RuntimeError(f"unable to reap timed-out process {process.pid}") from exc


async def _supervise(
    process: asyncio.subprocess.Process,
    configuration: _RunConfiguration,
    settings: _RunSettings,
) -> tuple[bytes | None, bytes | None, bool, bool]:
    timeout_seconds = settings.timeout_seconds
    stdout_bytes = bytearray()
    stderr_bytes = bytearray()
    budget = _CaptureBudget(settings.max_output_bytes)
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
    stdout = bytes(stdout_bytes) if configuration.capture_stdout else None
    stderr = bytes(stderr_bytes) if configuration.capture_stderr else None
    return stdout, stderr, timed_out, overflowed


def _finalize_result(
    command: tuple[str, ...],
    process: asyncio.subprocess.Process,
    configuration: _RunConfiguration,
    captured: tuple[bytes | None, bytes | None, bool, bool],
    settings: _RunSettings,
) -> CommandResult:
    stdout, stderr, timed_out, overflowed = captured
    text, errors, check = settings.text, settings.errors, settings.check
    timeout_seconds, max_output_bytes = settings.timeout_seconds, settings.max_output_bytes
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
    result: CommandResult
    if text:
        assert decoded_stdout is None or isinstance(decoded_stdout, str)
        assert decoded_stderr is None or isinstance(decoded_stderr, str)
        result = TextCommandResult(
            command,
            process.returncode,
            decoded_stdout or "",
            decoded_stderr or "",
        )
    else:
        assert decoded_stdout is None or isinstance(decoded_stdout, bytes)
        assert decoded_stderr is None or isinstance(decoded_stderr, bytes)
        result = BinaryCommandResult(
            command,
            process.returncode,
            decoded_stdout or b"",
            decoded_stderr or b"",
        )
    if check:
        result.check_returncode()
    return result


@overload
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
    text: Literal[True],
    encoding: str | None = None,
    errors: str = "strict",
    check: bool = False,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES,
) -> TextCommandResult: ...


@overload
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
    text: Literal[False] = False,
    encoding: str | None = None,
    errors: str = "strict",
    check: bool = False,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES,
) -> BinaryCommandResult: ...


@overload
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
    text: bool,
    encoding: str | None = None,
    errors: str = "strict",
    check: bool = False,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES,
) -> CommandResult: ...


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
    # Read the caller's sequence once: validation and launch both use this copy,
    # so a caller that changes its list meanwhile cannot swap the name launched.
    argv = tuple(argv)
    environment = _validated_environment(env)
    command = _validated_argv(argv, allowed_executables, environment.get("PATH", os.defpath))
    settings = _RunSettings(text, errors, check, timeout_seconds, max_output_bytes)
    configuration = _prepare_run_configuration(
        cwd=cwd,
        environment=environment,
        input_data=input_data,
        capture_output=capture_output,
        stdout=stdout,
        stderr=stderr,
        stderr_to_stdout=stderr_to_stdout,
        settings=settings,
        encoding=encoding,
    )
    # Keep trusted launch links intact; untrusted chains launch the resolved file.
    # Preserve the caller's argv[0]: a multi-call program (the `vmafx` link to `vmaf`)
    # selects its mode from that name.
    process = await asyncio.create_subprocess_exec(
        _stringify(argv[0], label="argv[0]"),
        *command[1:],
        executable=command[0],
        **configuration.process_kwargs,
    )
    captured = await _supervise(process, configuration, settings)
    return _finalize_result(command, process, configuration, captured, settings)


@overload
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
    text: Literal[True],
    encoding: str | None = None,
    errors: str = "strict",
    check: bool = False,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES,
) -> TextCommandResult: ...


@overload
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
    text: Literal[False] = False,
    encoding: str | None = None,
    errors: str = "strict",
    check: bool = False,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES,
) -> BinaryCommandResult: ...


@overload
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
    text: bool,
    encoding: str | None = None,
    errors: str = "strict",
    check: bool = False,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
    max_output_bytes: int = DEFAULT_MAX_OUTPUT_BYTES,
) -> CommandResult: ...


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
