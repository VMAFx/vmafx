#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Decide how much of the tree the Pre-Commit CI job checks, and run it (HISS-18).

A pull request is checked on its own diff: ``pre-commit run --from-ref <base sha>
--to-ref HEAD`` runs each hook on the files the pull request changed, so a
documentation-only change never pays for the C, Go and Rust hooks. Every other
event (master push, release tag, schedule, dispatch) and any pull request whose
base commit is missing keep the whole tree: ``--all-files``. The required
context ``Pre-Commit`` is reported by the job either way; the scope is printed
with its reason so a narrowed run is never mistaken for a full one.

A pull request that changes what a hook checks also gets the whole tree: a
change to ``.pre-commit-config.yaml``, to any file under ``scripts/ci/`` or
``scripts/lib/``, to a path a hook's command names, or to a module a hook script
imports can add findings in files the pull request never
touched. On the diff alone such a hook passed on its pull request and failed
the next master push (``check-fork-python-subprocess``, #3237 / a3ea48309).

Usage (workflow): ``precommit_scope.py --event "$EVENT" --base-sha "$BASE_SHA"``
(``--print`` shows the arguments without running anything).
"""

from __future__ import annotations

import argparse
import ast
import itertools
import re
import shlex
import shutil
import sys
from collections.abc import Collection, Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import yaml  # type: ignore[import-untyped]

from scripts.lib.safe_subprocess import CommandOutputLimitExceeded, CommandTimedOut
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ".pre-commit-config.yaml"

PULL_REQUEST_EVENTS = frozenset({"pull_request", "pull_request_target"})
SHA = re.compile(r"[0-9a-f]{40}")
BASE_ARGS = ("--show-diff-on-failure", "--color=always")
PRE_COMMIT_TIMEOUT_S = 1800.0
GIT_TIMEOUT_S = 120.0


# Directories whose every file is a hook input: scripts/ci/ is CI authority
# (scripts/ci/AGENTS.md), scripts/lib/ holds the modules the hook scripts import.
AUTHORITY_PREFIXES = ("scripts/ci/", "scripts/lib/")
MODULE = re.compile(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)+")
UNPARSABLE = "\0unparsable"  # an entry shlex cannot split: every path is an input
MAX_TOKENS = 100_000  # bound on entry tokens, nested shell commands included
MAX_IMPORT_FILES = 5_000  # bound on the import walk


def entry_tokens(command: str) -> list[str]:
    """The words of a hook command, with the commands inside ``bash -c`` / ``sh -c``."""
    pending: list[str] = [command]
    words: list[str] = []
    while pending and len(words) < MAX_TOKENS:
        try:
            split = shlex.split(pending.pop(), comments=True)
        except ValueError:
            return [UNPARSABLE]
        words.extend(split)
        pending.extend(split[i + 1] for i, w in enumerate(split[:-1]) if w == "-c")
    return words


def token_paths(token: str, root: Path) -> set[str]:
    """Repository files one command word names (a missing file still counts)."""
    value = token.split("=", 1)[1] if token.startswith("-") and "=" in token else token
    value = value.split("*", 1)[0].rstrip("/") if "*" in value else value
    if not value or any(c.isspace() for c in value) or value.startswith(("-", "/", "$", ".")):
        return set()
    if "://" in value or value.endswith("/"):
        return set()
    if "/" in value:
        return {Path(value).as_posix()}
    if MODULE.fullmatch(value) and (root / value.split(".", 1)[0]).is_dir():
        stem = value.replace(".", "/")
        return {f"{stem}.py", f"{stem}/"}
    return {value} if (root / value).is_file() else set()


def search_bases(source: Path, root: Path) -> tuple[Path, ...]:
    """Where an absolute import may resolve: the root, then every directory from the
    file up to the root with its ``src/`` child (``tests/`` beside ``src/vmaf_rc1_tester``)."""
    bases = [root]
    for directory in source.parents:
        if directory == root or root not in directory.parents:
            break
        bases += [directory, directory / "src"]
    return tuple(bases)


def from_targets(node: ast.ImportFrom, source: Path, root: Path) -> set[tuple[Path, str]]:
    """(base, dotted name) pairs one ``from ... import`` statement may load."""
    if node.level:
        if node.level > len(source.parents):
            return set()
        anchor = source.parents[node.level - 1]
        if anchor != root and root not in anchor.parents:
            return set()
        bases: tuple[Path, ...] = (anchor,)
    else:
        bases = search_bases(source, root)
    module = node.module or ""
    names = {f"{module}.{alias.name}".strip(".") for alias in node.names}
    if module:
        names.add(module)
    return {(base, name) for base in bases for name in names}


def module_paths(base: Path, name: str, root: Path) -> set[str]:
    """The repository file a dotted name loads from `base`; a missing module whose
    directory exists still counts (a pull request may delete it), stdlib names do not."""
    stem = base / name.replace(".", "/")
    found = {c for c in (stem.with_suffix(".py"), stem / "__init__.py") if c.is_file()}
    top = name.split(".", 1)[0]
    if not found and stem.parent.is_dir() and not stem.is_dir():
        if top not in sys.stdlib_module_names:
            found = {stem.with_suffix(".py")}
    return {c.relative_to(root).as_posix() for c in found}


def python_imports(source: Path, root: Path) -> set[str]:
    """Repository modules (as paths) a Python file imports, relative imports included."""
    try:
        tree = ast.parse(source.read_text(encoding="utf-8"))
    except (OSError, SyntaxError, UnicodeDecodeError):
        return set()
    targets: set[tuple[Path, str]] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            bases = search_bases(source, root)
            targets.update((base, alias.name) for base in bases for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            targets.update(from_targets(node, source, root))
    return {path for base, name in targets for path in module_paths(base, name, root)}


def python_files(paths: Collection[str], root: Path) -> list[str]:
    """The Python files the input paths name: files as they are, directories expanded."""
    found: list[str] = []
    for path in sorted(paths):
        target = root / path
        if path.endswith(".py") and target.is_file():
            found.append(path)
        elif path not in (".", "") and target.is_dir():
            files = itertools.islice(sorted(target.rglob("*.py")), MAX_IMPORT_FILES)
            found.extend(f.relative_to(root).as_posix() for f in files)
    return found


def import_closure(paths: set[str], root: Path) -> set[str]:
    """The Python files reachable through imports from the given input paths."""
    queue = python_files(paths, root)
    seen: set[str] = set()
    while queue and len(seen) < MAX_IMPORT_FILES:
        current = queue.pop()
        if current in seen:
            continue
        seen.add(current)
        if (root / current).is_file():
            queue.extend(python_imports(root / current, root) - seen)
    return seen


def hook_inputs(config_text: str, root: Path = ROOT) -> frozenset[str]:
    """The config, the CI-authority prefixes and every path any hook's command names.

    Each entry is read with its ``args``, from local and remote repositories alike;
    the commands inside ``bash -c`` are split again. A word counts when it names a
    repository path, whether or not the file still exists (a pull request may delete
    it), a dotted module under a top-level directory (``python -m scripts.lib.x``), or
    an existing top-level file (``Makefile`` for ``make``). Python files are followed
    through their imports. Entries are prefixes: ``scripts/ci/tests`` covers its files.
    """
    inputs = {CONFIG, *AUTHORITY_PREFIXES}
    for repo in (yaml.safe_load(config_text) or {}).get("repos", []):
        for hook in repo.get("hooks", []):
            command = " ".join(
                shlex.quote(str(part)) for part in [hook.get("entry", ""), *hook.get("args", [])]
            )
            words = entry_tokens(str(hook.get("entry", ""))) + entry_tokens(command)[1:]
            if words[:1] == ["make"]:
                inputs.add("Makefile")
            if UNPARSABLE in words:
                inputs.add(".")
            for word in words:
                inputs.update(token_paths(word, root))
    return frozenset(inputs | import_closure(inputs, root))


def touched_inputs(changed: Collection[str], inputs: Collection[str]) -> list[str]:
    """The changed paths that are, or lie under, a hook input."""
    if "." in inputs:
        return sorted(changed)
    prefixes = tuple(p if p.endswith("/") else f"{p}/" for p in inputs)
    return sorted(c for c in changed if c in inputs or c.startswith(prefixes))


def scope(
    event: str,
    base_sha: str,
    changed: Collection[str] | None = None,
    inputs: Collection[str] = (CONFIG,),
) -> tuple[list[str], str]:
    """Return the pre-commit selection arguments and the reason for them.

    ``changed`` is the pull request's file list; ``None`` (unknown) fails closed.
    """
    if event not in PULL_REQUEST_EVENTS:
        return ["--all-files"], f"event {event or 'unknown'} is not a pull request: whole tree"
    if not SHA.fullmatch(base_sha):
        return ["--all-files"], "pull request without a usable base sha: whole tree (fail closed)"
    if changed is None:
        return ["--all-files"], "pull request file list unreadable: whole tree (fail closed)"
    touched = touched_inputs(changed, inputs)
    if touched:
        return [
            "--all-files"
        ], f"pull request changes hook inputs ({', '.join(touched)}): whole tree"
    return (
        ["--from-ref", base_sha, "--to-ref", "HEAD"],
        f"pull request: files changed since {base_sha[:12]}; whole tree runs on master push,"
        " release tags, schedule and dispatch",
    )


def changed_files(base_sha: str) -> list[str] | None:
    """The files changed between ``base_sha`` and HEAD, or ``None`` when git fails."""
    git = shutil.which("git")
    if git is None or not SHA.fullmatch(base_sha):
        return None
    try:
        result = run_command(
            [git, "diff", "--name-only", "--no-renames", base_sha, "HEAD"],
            allowed_executables=(git,),
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout_seconds=GIT_TIMEOUT_S,
        )
    except (CommandTimedOut, CommandOutputLimitExceeded, OSError):
        return None  # unreadable file list: scope() falls back to the whole tree
    if result.returncode != 0:
        return None
    return [line for line in result.stdout.splitlines() if line]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", default="")
    parser.add_argument("--base-sha", default="")
    parser.add_argument("--print", action="store_true", dest="print_only")
    args = parser.parse_args(argv)
    pull_request = args.event in PULL_REQUEST_EVENTS
    changed = changed_files(args.base_sha) if pull_request else []
    inputs = hook_inputs((ROOT / CONFIG).read_text(encoding="utf-8"))
    selection, reason = scope(args.event, args.base_sha, changed, inputs)
    print(f"precommit-scope: {reason}", file=sys.stderr)
    command = ["pre-commit", "run", *BASE_ARGS, *selection]
    if args.print_only:
        print(" ".join(command))
        return 0
    executable = shutil.which("pre-commit")
    if executable is None:
        print("precommit-scope: pre-commit not found", file=sys.stderr)
        return 2
    result = run_command(
        [executable, *command[1:]],
        allowed_executables=(executable,),
        text=True,
        timeout_seconds=PRE_COMMIT_TIMEOUT_S,
    )
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
