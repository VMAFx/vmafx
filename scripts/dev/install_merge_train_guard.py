#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Preview/install paused, hash-bound local train adapters; never control PIDs."""

from __future__ import annotations

import argparse
import ast
import fcntl
import hashlib
import json
import os
import shlex
import shutil
import stat
import sys
import tempfile
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandFailed
from scripts.lib.safe_subprocess import run as run_command

SOURCE_PATH = "scripts/dev/merge_train_guard.py"
COMPOSER_PATH = "scripts/ci/squash_message.py"
RUNTIME_FILES = ("train.sh", "rebase-clean.sh", "watchdog.sh", "merge_train_operator.py")
CONTROL_FILES = (*RUNTIME_FILES, "policy.json", "hold.txt", "PAUSED")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(source: Path, *args: str) -> str:
    executable = shutil.which("git")
    if executable is None:
        raise ValueError("git is unavailable")
    result = run_command(
        [executable, "-C", str(source), *args],
        allowed_executables=(executable,),
        env={key: value for key, value in os.environ.items() if not key.startswith("GIT_")},
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=60,
    )
    return result.stdout


def snapshot(path: Path) -> dict[str, Any] | None:
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(mode):
        raise ValueError(f"refusing non-regular runtime target: {path}")
    return {"sha256": sha256(path.read_bytes()), "mode": stat.S_IMODE(mode)}


def load_committed_blob(source: Path, head: str, relative_path: str, label: str) -> bytes:
    """Load committed Git blob for a relative path; raises ValueError if on-disk file differs."""
    committed = git(source, "show", f"{head}:{relative_path}").encode()
    if (source / relative_path).read_bytes() != committed:
        raise ValueError(f"{label} source must match its committed Git blob")
    return committed


def import_names(node: ast.AST, relative_path: str) -> tuple[set[str], set[str]]:
    """Separate explicit module names from possible submodules of absolute from-imports.

    Raises ValueError on a relative import: the closure resolves absolute names only, so
    a relative one would ship a release whose import fails only when the train runs.
    """
    if isinstance(node, ast.Import):
        return {alias.name for alias in node.names}, set()
    if not isinstance(node, ast.ImportFrom):
        return set(), set()
    if node.level != 0:
        raise ValueError(
            f"relative import in {relative_path}:{node.lineno}; use an absolute scripts.lib import"
        )
    if node.module is None:
        return set(), set()
    return {node.module}, {f"{node.module}.{alias.name}" for alias in node.names}


def is_library_module(name: str) -> bool:
    return name == "scripts.lib" or name.startswith("scripts.lib.")


def refuse_outside_library(names: set[str], relative_path: str) -> None:
    """Refuse `scripts.*` imports outside `scripts.lib`; the release ships no other part."""
    outside = sorted(
        name for name in names if name.startswith("scripts.") and not is_library_module(name)
    )
    if outside:
        raise ValueError(
            f"{relative_path} imports {outside[0]} outside scripts.lib; "
            "the release ships only scripts.lib"
        )


def library_imports(data: bytes, relative_path: str) -> tuple[set[str], set[str]]:
    """Collect absolute library imports and possible from-import submodules without execution."""
    try:
        tree = ast.parse(data, filename=relative_path)
    except SyntaxError as exc:
        raise ValueError(f"cannot parse imports in {relative_path}: {exc}") from exc
    required: set[str] = set()
    optional: set[str] = set()
    for node in ast.walk(tree):
        modules, submodules = import_names(node, relative_path)
        refuse_outside_library(modules | submodules, relative_path)
        required.update(filter(is_library_module, modules))
        optional.update(filter(is_library_module, submodules))
    return required, optional


def library_module_paths(module: str, available: set[str]) -> set[str]:
    """Resolve a committed module and every package initializer along its import path."""
    stem = module.replace(".", "/")
    candidates = (f"{stem}/__init__.py", f"{stem}.py")
    relative = next((path for path in candidates if path in available), None)
    if relative is None:
        raise ValueError(f"missing committed library module: {module}")
    parents = {f"{parent}/__init__.py" for parent in Path(relative).parents}
    return {relative} | (parents & available)


def committed_libraries(source: Path, head: str, gateway: bytes) -> dict[str, bytes]:
    """Close gateway/package imports over committed library blobs, including cycles once."""
    available = set(
        git(source, "ls-tree", "-r", "--name-only", head, "--", "scripts/lib").splitlines()
    )
    pending = {SOURCE_PATH, "scripts/lib/__init__.py"}
    visited: set[str] = set()
    libraries: dict[str, bytes] = {}
    for _ in range(len(available) + 1):
        if not pending:
            return dict(sorted(libraries.items()))
        relative = min(pending)
        pending.remove(relative)
        visited.add(relative)
        data = (
            gateway
            if relative == SOURCE_PATH
            else load_committed_blob(source, head, relative, f"library {relative}")
        )
        if relative != SOURCE_PATH:
            libraries[relative] = data
        required, optional = library_imports(data, relative)
        optional = {
            module
            for module in optional
            if f"{module.replace('.', '/')}.py" in available
            or f"{module.replace('.', '/')}/__init__.py" in available
        }
        for module in sorted(required | optional):
            pending.update(library_module_paths(module, available) - visited)
    if pending:
        raise ValueError("committed library import closure exceeded its module bound")
    return dict(sorted(libraries.items()))


def plan(
    source: Path, root: Path, state: Path, branches: list[str], worktrees: list[str]
) -> tuple[dict[str, Any], bytes]:
    """Compute installation plan and gateway bytes; raises ValueError on invalid policy or drift.

    Returns:
        Tuple of (plan_dict, committed_gateway_bytes).
    """
    head = git(source, "rev-parse", "HEAD").strip()
    committed = load_committed_blob(source, head, SOURCE_PATH, "gateway")
    committed_composer = load_committed_blob(source, head, COMPOSER_PATH, "composer")
    libraries = committed_libraries(source, head, committed)
    old = {name: snapshot(state / name) for name in CONTROL_FILES}
    policy: dict[str, Any] = {"schema": 1, "protected_branches": [], "protected_worktrees": []}
    if old["policy.json"] is not None:
        policy = json.loads((state / "policy.json").read_text())
    if not isinstance(policy, dict) or policy.get("schema") != 1:
        raise ValueError("unsupported existing policy; preserve it for manual review")
    for key, added in (("protected_branches", branches), ("protected_worktrees", worktrees)):
        if not isinstance(policy.get(key), list) or not all(
            isinstance(item, str) for item in policy[key]
        ):
            raise ValueError(f"invalid policy list: {key}")
        policy[key] = sorted(set(policy[key]) | set(added))
    for path in policy["protected_worktrees"]:
        if not Path(path).is_absolute():
            raise ValueError("protected worktree paths must be absolute")
    result = {
        "schema": 1,
        "source_commit": head,
        "source": str(source),
        "repository_root": str(root),
        "state": str(state),
        "gateway_sha256": sha256(committed),
        "composer_sha256": sha256(committed_composer),
        "library_sha256": {path: sha256(data) for path, data in libraries.items()},
        "before": old,
        "policy": policy,
        "installer_sha256": sha256(Path(__file__).read_bytes()),
    }
    return result, committed


def plan_hash(value: dict[str, Any]) -> str:
    return sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def launch_code(
    gateway: Path,
    expected: str,
    composer_expected: str,
    root: Path,
    state: Path,
    library_sha256: dict[str, str],
) -> str:
    """Verify runtime bytes and bind scripts to the release before entering the real gateway."""
    checks = {
        "merge_train_guard.py": (expected, "gateway hash changed"),
        "squash_message.py": (composer_expected, "composer hash changed"),
        **{
            path: (digest, f"library hash changed: {path}")
            for path, digest in sorted(library_sha256.items())
        },
    }
    return f"""import sys
sys.path[:] = [entry for entry in sys.path if entry and entry != {str(state)!r}]
import hashlib, runpy, types
from pathlib import Path
p = Path({str(gateway)!r})
for relative, (expected, label) in {checks!r}.items():
    message = 'refused: installed ' + label
    try:
        actual = hashlib.sha256((p.parent / relative).read_bytes()).hexdigest()
    except OSError:
        sys.exit(message)
    if actual != expected:
        sys.exit(message)
sys.path[:] = [str(p.parent), *(entry for entry in sys.path
    if entry and Path(entry).resolve() not in (Path.cwd(), Path({str(state)!r})))]
for name in tuple(sys.modules):
    if name == 'scripts' or name.startswith('scripts.'):
        del sys.modules[name]
scripts = types.ModuleType('scripts')
scripts.__path__ = [str(p.parent / 'scripts')]
sys.modules['scripts'] = scripts
sys.dont_write_bytecode = True
sys.argv = [str(p), *sys.argv[1:], '--repo-root', {str(root)!r}, '--state-dir', {str(state)!r}]
runpy.run_path(str(p), run_name='__main__')
"""


def adapters(
    gateway: Path,
    expected: str,
    composer_expected: str,
    root: Path,
    state: Path,
    library_sha256: dict[str, str],
) -> dict[str, bytes]:
    """Generate runtime adapters bound to every release hash, with an isolated scripts package.

    Args:
        expected: Expected SHA-256 digest of the gateway script.
        composer_expected: Expected SHA-256 digest of the composer script.
        library_sha256: Committed library paths and expected SHA-256 digests.
    """
    code = launch_code(gateway, expected, composer_expected, root, state, library_sha256)
    launcher = f"exec python3 -I -c {shlex.quote(code)}"
    train = f'#!/bin/sh\n# Generated by install_merge_train_guard.py; default read-only.\nset -eu\n{launcher} cycle "$@"\n'
    rebase = f'#!/bin/sh\n# Explicit PR and --apply required by the gateway.\nset -eu\n{launcher} rebase "$@"\n'
    operator = (
        "#!/usr/bin/env -S python3 -I\n# Retired unrestricted operator; inspection only.\n"
        "# -I in the shebang: site hooks such as sitecustomize.py run before line 1.\n"
        "import posix, sys\n"
        "if not sys.flags.isolated:\n"
        "    if not sys.executable:\n        raise SystemExit('refused: interpreter path unknown')\n"
        "    posix.execv(sys.executable, [sys.executable, '-I', __file__, *sys.argv[1:]])\n"
        "if len(sys.argv) != 1:\n    raise SystemExit('operator is inspection-only; no arguments accepted')\n"
        "sys.argv = [sys.argv[0], 'cycle']\n" + code + "\n"
    )
    train_path = shlex.quote(str(state / "train.sh"))
    watchdog = (
        "#!/bin/sh\n# Observe only; process start/stop belongs to the operator.\nset -eu\n"
        'if [ "$#" -ne 0 ]; then\n\techo "watchdog is inspection-only" >&2\n\texit 2\nfi\n'
        f"while :; do\n\tif ! {train_path}; then echo 'guarded observer refused this pass' >&2; fi\n\tsleep 60\ndone\n"
    )
    return {
        name: content.encode()
        for name, content in zip(RUNTIME_FILES, (train, rebase, watchdog, operator), strict=True)
    }


def atomic_write(path: Path, data: bytes, mode: int) -> None:
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(mode)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def install(
    value: dict[str, Any],
    committed: bytes,
    expected: str,
    composer_committed: bytes | None = None,
) -> Path:
    """Apply the previewed plan under the mutation lock.

    Raises ValueError when the plan or runtime bytes differ from the preview, and
    BlockingIOError when another holder has the mutation lock.

    Args:
        expected: Expected SHA-256 digest of the preview plan.

    Returns:
        Path to the migration backup directory.
    """
    if plan_hash(value) != expected:
        raise ValueError("preview plan changed; inspect a fresh plan before applying")
    if sha256(committed) != value["gateway_sha256"]:
        raise ValueError("gateway bytes do not match the preview")
    if composer_committed is None:
        source = Path(value["source"])
        head = str(value["source_commit"])
        composer_committed = git(source, "show", f"{head}:{COMPOSER_PATH}").encode()
    if sha256(composer_committed) != value["composer_sha256"]:
        raise ValueError("composer bytes do not match the preview")
    libraries = previewed_libraries(value)
    state = Path(value["state"])
    state.mkdir(parents=True, exist_ok=True)
    with (state / "mutation.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return install_locked(value, committed, expected, state, composer_committed, libraries)


def previewed_libraries(value: dict[str, Any]) -> dict[str, bytes]:
    """Re-read committed library bytes and check every digest before touching runtime state."""
    libraries: dict[str, bytes] = {}
    for relative, expected in sorted(value["library_sha256"].items()):
        data = load_committed_blob(
            Path(value["source"]), str(value["source_commit"]), relative, f"library {relative}"
        )
        if sha256(data) != expected:
            raise ValueError(f"library bytes do not match the preview: {relative}")
        libraries[relative] = data
    return libraries


def release_directory(directory: Path) -> None:
    """Create a release directory without following an existing symlink."""
    if directory.is_symlink():
        raise ValueError(f"refusing symlink release directory: {directory}")
    directory.mkdir(exist_ok=True)


def prepare_release_file(
    release: Path, relative: str, data: bytes, expected: str, label: str
) -> None:
    """Write a read-only release file, refusing symlink parents and differing evidence."""
    directory = release
    for component in Path(relative).parts[:-1]:
        directory = directory / component
        release_directory(directory)
    target = release / relative
    existing = snapshot(target)
    if existing is not None and existing["sha256"] != expected:
        raise ValueError(f"release path contains unexpected {label}evidence; refusing overwrite")
    if existing is None:
        atomic_write(target, data, 0o444)


def prepare_release(
    release: Path,
    gateway_bytes: bytes,
    gateway_sha: str,
    composer_bytes: bytes,
    composer_sha: str,
    libraries: dict[str, bytes],
    library_sha256: dict[str, str],
) -> Path:
    """Write the read-only runtime scripts and libraries, keeping matching release files.

    Raises ValueError when a release directory is a symlink or an existing release file has a
    different hash.

    Returns:
        Path to the installed gateway script.
    """
    for directory in (release.parent, release):
        release_directory(directory)
    gateway = release / "merge_train_guard.py"
    prepare_release_file(release, gateway.name, gateway_bytes, gateway_sha, "")
    prepare_release_file(release, "squash_message.py", composer_bytes, composer_sha, "composer ")
    for relative, data in libraries.items():
        prepare_release_file(release, relative, data, library_sha256[relative], "library ")
    return gateway


def release_name(value: dict[str, Any]) -> str:
    """Name the release directory after all committed runtime hashes.

    A change to any runtime file installs into a new immutable directory instead of
    meeting an older file there that prepare_release refuses to overwrite.

    Returns:
        SHA-256 hex digest of the canonical JSON runtime hash manifest.
    """
    return plan_hash(
        {key: value[key] for key in ("gateway_sha256", "composer_sha256", "library_sha256")}
    )


def migration_backup(value: dict[str, Any], state: Path) -> Path:
    """Preserve previewed runtime files and their modes before pausing or replacing them."""
    backup = Path(tempfile.mkdtemp(prefix="migration-", dir=state))
    atomic_write(backup / "plan.json", (json.dumps(value, indent=2) + "\n").encode(), 0o644)
    for name, previous in value["before"].items():
        path = state / name
        if snapshot(path) != previous:
            raise ValueError(f"runtime target changed during installation: {path}")
        if previous is not None:
            shutil.copy2(path, backup / name)
    return backup


def install_locked(
    value: dict[str, Any],
    committed: bytes,
    expected: str,
    state: Path,
    composer_committed: bytes,
    libraries: dict[str, bytes],
) -> Path:
    """Back up the runtime targets, pause the train, then install release files and adapters.

    Raises ValueError when a runtime target changed since the preview.

    Returns:
        Path to the migration backup directory.
    """
    backup = migration_backup(value, state)
    # Install no live bypass: leave paused even if a later step fails.
    if not (state / "PAUSED").exists():
        atomic_write(
            state / "PAUSED", b"Runtime migration: review before enabling mutations.\n", 0o644
        )
    release = state / "releases" / release_name(value)
    gateway = prepare_release(
        release,
        committed,
        value["gateway_sha256"],
        composer_committed,
        value["composer_sha256"],
        libraries,
        value["library_sha256"],
    )
    targets = adapters(
        gateway,
        value["gateway_sha256"],
        value["composer_sha256"],
        Path(value["repository_root"]),
        state,
        value["library_sha256"],
    )
    targets["policy.json"] = (json.dumps(value["policy"], indent=2) + "\n").encode()
    if value["before"]["hold.txt"] is None:
        targets["hold.txt"] = b"# Operator holds; #1213 is always excluded.\n"
    for name, data in targets.items():
        if snapshot(state / name) != value["before"][name]:
            raise ValueError(f"runtime target changed before replacement: {name}")
        atomic_write(state / name, data, 0o755 if name in RUNTIME_FILES else 0o644)
    receipt = {
        "plan": value,
        "plan_sha256": expected,
        "backup": str(backup),
        "after": {name: snapshot(state / name) for name in CONTROL_FILES},
    }
    atomic_write(backup / "receipt.json", (json.dumps(receipt, indent=2) + "\n").encode(), 0o644)
    return backup


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--state-dir", type=Path, required=True)
    parser.add_argument("--protect-branch", action="append", default=[])
    parser.add_argument("--protect-worktree", action="append", default=[])
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--expect-plan", help="sha256 printed by the reviewed preview")
    args = parser.parse_args(argv)
    try:
        value, committed = plan(
            args.source_root.resolve(),
            args.repo_root.resolve(),
            args.state_dir.resolve(),
            args.protect_branch,
            args.protect_worktree,
        )
        if not args.apply:
            print(json.dumps({"plan_sha256": plan_hash(value), "plan": value}, indent=2))
        else:
            if not args.expect_plan:
                raise ValueError("--apply requires the reviewed --expect-plan hash")
            backup = install(value, committed, args.expect_plan)
            print(f"Installed paused adapters; originals and receipt: {backup}")
        return 0
    except (OSError, ValueError, TypeError, KeyError, CommandFailed) as exc:
        print(f"merge-train install refused: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
