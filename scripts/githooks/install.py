#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Install regular hook dispatchers without owning the user's custom hooks."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
from importlib.resources import files
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

HOOKS = ("pre-commit", "commit-msg", "pre-push", "pre-rebase")
SOURCES = {
    "pre-commit": "scripts/githooks/pre-commit.sh",
    "pre-push": "scripts/git-hooks/pre-push",
    "pre-rebase": "scripts/git-hooks/pre-rebase",
}


def git(*args: str) -> str:
    executable = shutil.which("git")
    if executable is None:
        raise SystemExit("install-hooks: Git is missing from PATH")
    result = run_command(
        (executable, *args),
        allowed_executables=(executable,),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=60,
    )
    assert isinstance(result.stdout, str)
    return result.stdout.strip()


def framework_hook(path: Path) -> bool:
    """Only replace an unmodified framework template, never a user wrapper."""
    before, rest = (
        files("pre_commit.resources").joinpath("hook-tmpl").read_text().split("# start templated\n")
    )
    _, after = rest.split("# end templated\n")
    contents = path.read_text()
    lines = contents.splitlines()
    python_lines = [line for line in lines if line.startswith("INSTALL_PYTHON=")]
    if len(python_lines) != 1:
        return False
    expected = (
        before
        + "# start templated\n"
        + python_lines[0]
        + "\n"
        + f"ARGS=(hook-impl --config=.pre-commit-config.yaml --hook-type={path.name})\n"
        + "# end templated\n"
        + after
    )
    return contents == expected


def managed_source_link(path: Path, common: Path) -> bool:
    source = SOURCES.get(path.name)
    if source is None:
        return False
    target = (path.parent / path.readlink()).resolve()
    # Recognize only this clone's historical source links, including removed
    # worktrees. Arbitrary user symlinks remain untouched.
    main = common.parent
    if target == main / source:
        return True
    try:
        relative = target.relative_to(main / ".claude/worktrees")
    except ValueError:
        return False
    return len(relative.parts) == 1 + len(Path(source).parts) and Path(*relative.parts[1:]) == Path(
        source
    )


def managed(path: Path, common: Path, template: bytes) -> bool:
    if path.is_symlink():
        return managed_source_link(path, common)
    if not path.exists():
        return True
    if not path.is_file():
        return False
    data = path.read_bytes()
    if data in (template, template.replace(b"mode=framework", b"mode=native")):
        return True
    try:
        return framework_hook(path)
    except (ImportError, UnicodeError, ValueError):
        return False


def validate_sources(root: Path, hooks: Path, common: Path, template: bytes) -> None:
    """Reject missing sources and hooks that the installer does not own."""
    for source in SOURCES.values():
        if not os.access(root / source, os.X_OK):
            raise SystemExit(f"install-hooks: missing executable {root / source}")
    unknown = [str(hooks / hook) for hook in HOOKS if not managed(hooks / hook, common, template)]
    if unknown:
        raise SystemExit(
            "install-hooks: custom hooks left untouched; review and relocate them explicitly before retrying:\n"
            + "\n".join(unknown)
        )


def prepare_framework(root: Path, framework: str) -> None:
    """Validate config and prepare hook environments before live replacement."""
    run_command(
        (framework, "validate-config"),
        allowed_executables=(framework,),
        cwd=root,
        check=True,
        timeout_seconds=120,
    )
    run_command(
        (framework, "install-hooks"),
        allowed_executables=(framework,),
        cwd=root,
        check=True,
        timeout_seconds=120,
    )


def install_dispatchers(hooks: Path, content: bytes) -> None:
    """Atomically replace only the hook paths validated as managed."""
    hooks.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".vmafx-install-", dir=hooks) as scratch:
        for hook in HOOKS:
            destination = hooks / hook
            if (
                destination.is_file()
                and not destination.is_symlink()
                and destination.read_bytes() == content
            ):
                continue
            replacement = Path(scratch) / hook
            replacement.write_bytes(content)
            replacement.chmod(0o755)
            if os.path.lexists(destination):
                descriptor, backup = tempfile.mkstemp(prefix=f"{hook}.vmafx-backup-", dir=hooks)
                os.close(descriptor)
                # copy2 cannot replace a regular tempfile with a symlink.
                if destination.is_symlink():
                    Path(backup).unlink()
                shutil.copy2(destination, backup, follow_symlinks=False)
                print(f"install-hooks: preserved {destination} at {backup}")
            replacement.replace(destination)
            print(f"install-hooks: installed {destination}")


def main() -> None:
    root = Path(git("rev-parse", "--show-toplevel"))
    common = Path(git("rev-parse", "--path-format=absolute", "--git-common-dir"))
    hooks = Path(git("rev-parse", "--path-format=absolute", "--git-path", "hooks"))
    framework = shutil.which("pre-commit")
    if framework is None:
        raise SystemExit("install-hooks: install pre-commit in the active Python environment first")
    template = (root / "scripts/githooks/dispatch.sh").read_bytes()
    mode = os.environ.get("VMAFX_NATIVE_HOOKS", "0")
    if mode not in ("0", "1"):
        raise SystemExit("install-hooks: VMAFX_NATIVE_HOOKS must be 0 or 1")
    validate_sources(root, hooks, common, template)
    prepare_framework(root, framework)
    content = template.replace(b"mode=framework", b"mode=native") if mode == "1" else template
    install_dispatchers(hooks, content)
    print(
        f"install-hooks: {'native' if mode == '1' else 'framework'} pre-commit; framework commit-msg/pre-push; pre-rebase guard"
    )


if __name__ == "__main__":
    main()
