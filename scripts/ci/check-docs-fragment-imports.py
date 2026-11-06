#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every third-party module a `make docs-fragments-check` script imports is declared.

Issue #3244: `make docs-fragments-check` stopped with
`error: python package jsonschema is required` in a fresh worktree, because the
repository virtual environment held only what the setup recipe declared and
nothing in it declared `jsonschema` (imported by
`scripts/ci/check-hardware-reports.py`) or `yaml` (imported by
`scripts/docs/credits_lib.py`, which `scripts/docs/generate-credits.py` and
`scripts/docs/check-credits.py` pull in). CI installs the hash-locked
`requirements/locks/docs-check.txt` before the Docs job; `make lint-tools` and
the documented recipe (docs/development/languages.md) do the same locally.

This gate keeps that declaration honest as the scripts evolve: when a script
the target runs — or any repository module its imports reach — gains a new
third-party import, the check fails until `requirements/locks/docs-check.in`
declares it. It is stdlib-only on purpose — the doctor must not need the
medicine — so it runs on the bare `python3` that `make docs-fragments-check`
resolves, however incomplete the environment is.

The script list is not duplicated here: the target's recipe in the `Makefile`
is parsed, and `bash` helper scripts in the recipe are scanned for the
`python3` invocations they make. The gate also verifies it is still wired into
the target it guards; a recipe that drops the check line is itself a finding.

Usage: check-docs-fragment-imports.py [--root REPO_ROOT]
Exit: 0 clean; 1 a finding; 2 the gate cannot run (missing inputs).
"""

from __future__ import annotations

import argparse
import ast
import re
import shlex
import sys
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[2]
MAKEFILE = "Makefile"
TARGET = "docs-fragments-check"
LOCK_INPUT = "requirements/locks/docs-check.in"
SELF = "scripts/ci/check-docs-fragment-imports.py"
# `tools/rc1-tester/src` is inserted on sys.path by
# scripts/ci/check-hardware-reports.py; the repo root is inserted by the
# scripts/docs entry points. A script's own directory is implicitly first on
# sys.path, so it is covered by the parents walk below.
EXTRA_SYS_PATH_BASES = ("tools/rc1-tester/src",)
# Package distribution name -> the top-level module it provides. Everything not
# listed here imports under its own lower-case name.
PACKAGE_IMPORT_NAMES = {"pyyaml": "yaml"}
# Third-party modules a script imports behind a graceful fallback: the target
# runs without them, so they stay out of docs-check.in by design. Each entry
# carries the reason, and the gate refuses them the day the fallback goes away
# (the import then fails the target in a declared-only environment like any
# other undeclared module).
OPTIONAL_IMPORTS = {
    "vl_convert": (
        "optional renderer of scripts/docs/generate-charts.py: without "
        "vl-convert-python the --check pass verifies the committed data and "
        "skips the render comparison (declared for the render path in "
        "docs/requirements-lock.txt)"
    ),
}
MAX_FILES = 500  # bound on the import-closure walk


def _recipe_tokens(line: str) -> list[str]:
    """The words of one recipe line, make's silencing prefixes stripped."""
    tokens = shlex.split(line.strip())
    while tokens and tokens[0][:1] in {"@", "-", "+"}:
        tokens[0] = tokens[0][1:]
    return [token for token in tokens if token]


def _split_recipe_scripts(tokens: list[str], python: list[Path], shell: list[Path]) -> None:
    """Sort one recipe line's words into the python and bash scripts it runs."""
    if len(tokens) > 1 and tokens[0] in {"python3", "python"} and tokens[1].endswith(".py"):
        python.append(Path(tokens[1]))
    elif len(tokens) > 1 and tokens[1].endswith(".sh") and tokens[0] in {"bash", "sh"}:
        shell.append(Path(tokens[1]))


def _scripts_from_helper(helper: Path, makefile_dir: Path) -> list[Path]:
    """The python scripts one bash helper of the recipe invokes (its
    `$SCRIPT_DIR` is the helper's directory). A missing helper is returned as
    its would-be python script so findings() can name it."""
    source = makefile_dir / helper
    if not source.is_file():
        return [source.with_suffix(".py")]
    text = source.read_text(encoding="utf-8", errors="replace")
    scripts: list[Path] = []
    for match in re.finditer(r"python3\s+(\"[^\"]+\"|\S+\.py)", text):
        token = match.group(1).strip('"')
        if "$" in token:
            token = re.sub(r"\$\{?SCRIPT_DIR\}?", helper.parent.as_posix(), token)
        if "/" in token and not token.startswith("/"):
            scripts.append(Path(token))
        else:
            scripts.append(helper.parent / token)
    return scripts


def recipe_scripts(makefile: Path) -> list[Path]:
    """The Python scripts the `docs-fragments-check` recipe runs, in recipe order.

    `python3 x.py` lines are taken directly; `bash x.sh` lines are scanned for
    the `python3` invocations the helper makes. The target's recipes only ever
    run repository scripts, so every path is repo-relative.
    """
    lines = makefile.read_text(encoding="utf-8").splitlines()
    start = None
    for index, line in enumerate(lines):
        if line.split(":", 1)[0].strip() == TARGET and line.rstrip().endswith(":"):
            start = index + 1
            break
    if start is None:
        raise LookupError(f"{makefile} has no {TARGET} target")

    python: list[Path] = []
    shell: list[Path] = []
    for line in lines[start:]:
        if not line.strip():
            continue
        if not line.startswith("\t"):
            break  # a non-recipe line ends the recipe
        tokens = _recipe_tokens(line)
        if tokens:
            _split_recipe_scripts(tokens, python, shell)
    for helper in shell:
        python.extend(_scripts_from_helper(helper, makefile.parent))
    return python


def _bases_for(source: Path, root: Path) -> tuple[Path, ...]:
    """Where an absolute import may resolve: the repo root, every directory from
    the file up to the root, then the extra sys.path entries the entry points
    insert (check-hardware-reports.py puts tools/rc1-tester/src on sys.path)."""
    bases: list[Path] = [root]
    for directory in source.parents:
        if directory == root:
            break
        bases.append(directory)
    for extra in EXTRA_SYS_PATH_BASES:
        bases.append(root / extra)
    ordered: list[Path] = []
    for base in bases:
        if base not in ordered:
            ordered.append(base)
    return tuple(ordered)


def _module_files(name: str, source: Path, root: Path) -> list[Path]:
    """Repository files the dotted module `name` may load when imported by
    `source`: every package prefix's `__init__.py` (importing `a.b` runs
    `a/__init__.py` and `a/b/__init__.py`) plus the module file itself."""
    parts = name.split(".")
    found: list[Path] = []
    for base in _bases_for(source, root):
        for depth in range(1, len(parts) + 1):
            stem = base.joinpath(*parts[:depth])
            for candidate in (stem.with_suffix(".py"), stem / "__init__.py"):
                if candidate.is_file():
                    resolved = candidate.resolve()
                    if resolved not in found:
                        found.append(resolved)
    return found


def _relative_files(node: ast.ImportFrom, source: Path, root: Path) -> list[Path]:
    """Repository files one relative `from ... import` may load."""
    if node.level > len(source.parents):
        return []
    anchor = source.parents[node.level - 1]
    module = (node.module or "").split(".") if node.module else []
    candidates: list[Path] = []
    if module:
        stem = anchor.joinpath(*module)
        candidates.extend((stem.with_suffix(".py"), stem / "__init__.py"))
    for alias in node.names:
        stem = anchor.joinpath(*module, alias.name)
        candidates.extend((stem.with_suffix(".py"), stem / "__init__.py"))
    found: list[Path] = []
    for candidate in candidates:
        if candidate.is_file():
            resolved = candidate.resolve()
            if resolved not in found:
                found.append(resolved)
    return found


def _dynamic_import_names(tree: ast.AST) -> list[str]:
    """Module names imported dynamically: `importlib.import_module('x')` and
    `__import__('x')` with a literal first argument."""
    names: list[str] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        function = node.func
        dynamic = None
        if isinstance(function, ast.Attribute) and function.attr == "import_module":
            dynamic = function.attr
        elif isinstance(function, ast.Name) and function.id == "__import__":
            dynamic = function.id
        if (
            dynamic
            and node.args
            and isinstance(node.args[0], ast.Constant)
            and isinstance(node.args[0].value, str)
        ):
            names.append(node.args[0].value)
    return names


def _import_targets(tree: ast.AST, source: Path, root: Path) -> tuple[list[str], list[Path]]:
    """(absolute dotted names, repository files) one module's import statements
    reach. Relative imports contribute files instead of names."""
    names: list[str] = []
    files: list[Path] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.extend(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            if node.level:
                files.extend(_relative_files(node, source, root))
            elif node.module:
                names.append(node.module)
    names.extend(_dynamic_import_names(tree))
    return names, files


def scan_imports(entry: Path, root: Path) -> tuple[set[str], list[Path]]:
    """Third-party top-level modules `entry` — or any repository module its
    imports reach — brings in, and the closure itself. Relative imports stay
    inside the closure; stdlib names are skipped before any filesystem work."""
    third_party: set[str] = set()
    seen: set[Path] = set()
    queue = [entry.resolve()]
    while queue and len(seen) < MAX_FILES:
        source = queue.pop(0)
        if source in seen:
            continue
        seen.add(source)
        try:
            tree = ast.parse(source.read_text(encoding="utf-8"))
        except (OSError, SyntaxError, UnicodeDecodeError):
            continue
        names, files = _import_targets(tree, source, root)
        queue.extend(files)
        for name in names:
            top = name.split(".", 1)[0]
            if top in sys.stdlib_module_names or top == "__future__":
                continue
            resolved = _module_files(name, source, root)
            if resolved:
                queue.extend(resolved)
            else:
                third_party.add(top)
    return third_party, sorted(seen)


def declared_modules(lock_input: Path) -> set[str]:
    """The import names `requirements/locks/docs-check.in` declares."""
    names: set[str] = set()
    for line in lock_input.read_text(encoding="utf-8").splitlines():
        stripped = line.split("#", 1)[0].strip()
        match = re.match(r"^([A-Za-z0-9][A-Za-z0-9._-]*)\s*==", stripped)
        if match:
            package = match.group(1).lower()
            names.add(PACKAGE_IMPORT_NAMES.get(package, package))
    return names


def findings(root: Path) -> list[str]:
    """The gate's findings against `root`; an empty list means clean."""
    problems: list[str] = []
    makefile = root / MAKEFILE
    lock_input = root / LOCK_INPUT
    if not makefile.is_file():
        raise LookupError(f"{makefile} does not exist")
    if not lock_input.is_file():
        raise LookupError(f"{lock_input} does not exist")

    scripts = recipe_scripts(makefile)
    if not any(path.as_posix() == SELF for path in scripts):
        problems.append(
            f"the {TARGET} recipe no longer runs {SELF}: "
            "the declared-imports gate is unwired from the target it guards"
        )
    declared = declared_modules(lock_input)
    for script in scripts:
        entry = root / script
        if not entry.is_file():
            problems.append(f"the {TARGET} recipe runs {script.as_posix()}, which does not exist")
            continue
        third_party, _closure = scan_imports(entry, root)
        for module in sorted(third_party - declared - set(OPTIONAL_IMPORTS)):
            problems.append(
                f"{script.as_posix()} imports '{module}': not declared in {LOCK_INPUT} "
                "(the scripts the target runs cannot import it from a declared environment)"
            )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="repository root")
    args = parser.parse_args(argv)
    try:
        problems = findings(args.root.resolve())
    except LookupError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)
    if problems:
        print(
            f"error: {len(problems)} undeclared import(s) behind '{MAKEFILE}:{TARGET}' — "
            f"declare them in {LOCK_INPUT} and regenerate the lock with "
            "`make python-locks-write`; CI installs it as docs-check.txt",
            file=sys.stderr,
        )
        return 1
    print(f"docs-fragment imports: every third-party import of the {TARGET} scripts is declared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
