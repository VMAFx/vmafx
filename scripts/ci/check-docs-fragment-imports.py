#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
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

The gate fails closed on its own blind spots: an import closure cut short by
the file bound, a file that cannot be read or parsed, and a recipe line
`shlex` cannot tokenize are each findings, never a silent pass. Recipe lines
joined with a trailing backslash are joined before tokenizing, and
interpreter options (`python3 -B x.py`) do not hide the script. A Python
interpreter spelled with an `env` prefix (`env LC_ALL=C python3 x.py`), a make
variable (`$(PYTHON) x.py`), a path (`/usr/bin/python3 x.py`) or a versioned
name (`python3.14 x.py`) is scanned the same way; a `-m` module run names no
script and is itself a finding.

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
# Third-party modules one script imports behind a graceful fallback: the target
# runs without them, so they stay out of docs-check.in by design. Each entry
# names its one importer and the reason. The exemption holds only while every
# import of the module sits in that file inside a `try` that catches
# ImportError; an import anywhere else, or without the fallback, is a finding
# like any other undeclared module.
OPTIONAL_IMPORTS = {
    "vl_convert": (
        "scripts/docs/generate-charts.py",
        "optional renderer: without vl-convert-python the --check pass verifies "
        "the committed data and skips the render comparison (declared for the "
        "render path in docs/requirements-lock.txt)",
    ),
}
_IMPORT_ERRORS = {"ImportError", "ModuleNotFoundError"}
MAX_FILES = 500  # bound on the import-closure walk


def _recipe_tokens(line: str, issues: list[str]) -> list[str]:
    """The words of one recipe line, make's silencing prefixes stripped. A line
    `shlex` cannot split becomes a finding instead of a crash or a skip."""
    try:
        tokens = shlex.split(line.strip())
    except ValueError as error:
        issues.append(f"recipe line cannot be tokenized ({error}): {line.strip()[:80]}")
        return []
    while tokens and tokens[0][:1] in {"@", "-", "+"}:
        tokens[0] = tokens[0][1:]
    return [token for token in tokens if token]


# Interpreter options that consume a separate value (`python3 --help`: `-W arg`,
# `-X opt`, `--check-hash-based-pycs`); the value never names the script, so
# both words are skipped together. `-c cmd` and `-m mod` end CPython's option
# list and run inline code or a module instead of a file: the scan cannot read
# either, so each is a finding, never a silent skip.
_VALUE_OPTS = {"-W", "-X", "--check-hash-based-pycs"}
_STOP_OPTS = "cm"
# `python3`, `python3.14`, `python`, and `env`'s `VAR=value` words, which carry
# no script either.
_INTERPRETER = re.compile(r"python[0-9.]*")
_MAKE_VARIABLE = re.compile(r"\$(?:\([^)]*\)|\{[^}]*\})")
_ASSIGNMENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*=")
# A helper line that may start a Python interpreter, checked before tokenizing.
_PYTHON_WORD = re.compile(r"(?<![\w.-])python[0-9.]*(?![\w-])")


def _python_interpreter(word: str) -> bool:
    """A Python interpreter however the recipe spells it: bare (`python3`), a
    path (`/usr/bin/python3`), a versioned name (`python3.14`), or a make
    variable that holds one (`$(PYTHON)`)."""
    return bool(_INTERPRETER.fullmatch(word.rsplit("/", 1)[-1])) or bool(
        _MAKE_VARIABLE.fullmatch(word)
    )


def _without_env_prefix(tokens: list[str]) -> list[str]:
    """The words after a leading `env` and its assignments: `env LC_ALL=C python3
    x.py` runs `python3 x.py`."""
    words = list(tokens)
    if words and words[0].rsplit("/", 1)[-1] == "env":
        words = words[1:]
        while words and (words[0].startswith("-") or _ASSIGNMENT.match(words[0])):
            words = words[1:]
    return words


def _option_words(word: str) -> int:
    """How many words one interpreter option takes: 0 for `-c`/`-m` (the option
    list ends there), 2 for an option whose value is the next word (`-W ignore`,
    `-X utf8`), 1 otherwise. Short options group (`-Bc`, `-BX utf8`), and an
    attached value (`-Xutf8`, `-Wignore`) ends the group."""
    if word.startswith("--"):
        return 2 if word in _VALUE_OPTS else 1
    for index, letter in enumerate(word[1:], start=1):
        if letter in _STOP_OPTS:
            return 0
        if f"-{letter}" in _VALUE_OPTS:
            return 2 if index == len(word) - 1 else 1
    return 1


def _python_script(tokens: list[str]) -> tuple[str | None, bool]:
    """The .py file a `python3 ...` invocation runs, interpreter options and
    their values skipped (`python3 -W ignore -X utf8 -B x.py` names x.py), and
    whether the invocation runs a `-m` module or `-c` code instead, which names
    no file (any later word is that code's argument, not a script)."""
    rest = tokens[1:]
    while rest and rest[0].startswith("-") and rest[0] != "-":
        taken = _option_words(rest[0])
        if taken == 0:
            return None, True
        rest = rest[taken:]
    return (rest[0] if rest and rest[0].endswith(".py") else None), False


def _split_recipe_scripts(
    tokens: list[str], python: list[Path], shell: list[Path], issues: list[str]
) -> None:
    """Sort one recipe line's words into the python and bash scripts it runs.

    A Python interpreter spelled any way the recipe can spell it — an `env`
    prefix, a make variable, an absolute path or a versioned name — is scanned
    like `python3`. A `-m` module or `-c` code run names no script and is a
    finding, so a later recipe edit cannot drop one from the scan without a word."""
    words = _without_env_prefix(tokens)
    if not words:
        return
    if _python_interpreter(words[0]):
        script, code_run = _python_script(words)
        if script:
            python.append(Path(script))
        elif code_run:
            issues.append(
                "recipe line runs a `python -m` module or `python -c` code, which "
                f"names no script the scan can read: {' '.join(tokens)[:100]}"
            )
    elif len(words) > 1 and words[1].endswith(".sh") and words[0] in {"bash", "sh"}:
        shell.append(Path(words[1]))


def _scripts_from_helper(helper: Path, makefile_dir: Path, issues: list[str]) -> list[Path]:
    """The python scripts one bash helper of the recipe invokes, read with the
    recipe's own interpreter rules (`_python_interpreter`, `_python_script`) so a
    `-m` or `-c` run, a versioned name or an `env` prefix reads the same in both
    places (`$SCRIPT_DIR` is the helper's directory). A missing helper is
    returned as its would-be python script so findings() can name it."""
    source = makefile_dir / helper
    if not source.is_file():
        return [source.with_suffix(".py")]
    scripts: list[Path] = []
    for line in source.read_text(encoding="utf-8", errors="replace").splitlines():
        if not _PYTHON_WORD.search(line):
            continue  # lines inside a multi-line awk or heredoc quote never split alone
        try:
            words = shlex.split(line, comments=True)
        except ValueError as error:
            issues.append(f"{helper.as_posix()} line cannot be tokenized ({error}): {line[:80]}")
            continue
        start = next((index for index, word in enumerate(words) if _python_interpreter(word)), None)
        if start is None:
            continue
        script, code_run = _python_script(words[start:])
        if code_run:
            issues.append(
                f"{helper.as_posix()} runs a `python -m` module or `python -c` code, which "
                f"names no script the scan can read: {line.strip()[:100]}"
            )
        elif script:
            token = re.sub(r"\$\{?SCRIPT_DIR\}?", helper.parent.as_posix(), script)
            relative = "/" in token and not token.startswith("/")
            scripts.append(Path(token) if relative else helper.parent / token)
    return scripts


def recipe_scripts(makefile: Path) -> tuple[list[Path], list[str]]:
    """The Python scripts the `docs-fragments-check` recipe runs, in recipe
    order, plus the recipe's tokenizing findings.

    `python3 x.py` lines are taken directly; `bash x.sh` lines are scanned for
    the `python3` invocations the helper makes. Recipe lines joined with a
    trailing backslash are joined before tokenizing, the way `make` hands one
    logical line to the shell. The target's recipes only ever run repository
    scripts, so every path is repo-relative.
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
    issues: list[str] = []
    pending = ""
    for line in lines[start:]:
        if not pending and not line.startswith("\t"):
            break  # a non-recipe line ends the recipe; a continuation never does
        candidate = pending + " " + line.strip() if pending else line.rstrip("\n")
        if candidate.rstrip().endswith("\\"):
            pending = candidate.rstrip()[:-1].rstrip() + " "
            continue
        pending = ""
        tokens = _recipe_tokens(candidate, issues)
        if tokens:
            _split_recipe_scripts(tokens, python, shell, issues)
    if pending:
        tokens = _recipe_tokens(pending, issues)
        if tokens:
            _split_recipe_scripts(tokens, python, shell, issues)
    for helper in shell:
        python.extend(_scripts_from_helper(helper, makefile.parent, issues))
    return python, issues


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
    # `from .a.b import c` runs a/__init__.py and a/b/__init__.py, as
    # _module_files does for an absolute import.
    for depth in range(1, len(module) + 1):
        stem = anchor.joinpath(*module[:depth])
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
                # `from pkg import mod` also loads pkg/mod.py when it exists;
                # a name the repository does not resolve (stdlib attribute,
                # package-`__init__` re-export) contributes nothing.
                for alias in node.names:
                    if alias.name == "*":
                        continue
                    child = f"{node.module}.{alias.name}"
                    if _module_files(child, source, root):
                        names.append(child)
    names.extend(_dynamic_import_names(tree))
    return names, files


def scan_imports(entry: Path, root: Path) -> tuple[set[str], list[Path], list[str]]:
    """Third-party top-level modules `entry` — or any repository module its
    imports reach — brings in, the closure itself, and the walk's incomplete
    cases (a closure cut short by the file bound, a file that cannot be read
    or parsed): each incomplete case is a finding, never a silent pass.
    Relative imports stay inside the closure; stdlib names are skipped before
    any filesystem work."""
    third_party: set[str] = set()
    seen: set[Path] = set()
    issues: list[str] = []
    queue = [entry.resolve()]
    while queue:
        if len(seen) >= MAX_FILES:
            issues.append(
                f"import-closure walk stopped at the {MAX_FILES}-file bound; "
                f"{len(queue)} module(s) were never scanned"
            )
            break
        source = queue.pop(0)
        if source in seen:
            continue
        seen.add(source)
        try:
            tree = ast.parse(source.read_text(encoding="utf-8"))
        except (OSError, SyntaxError, UnicodeDecodeError) as error:
            issues.append(
                f"{source.relative_to(root).as_posix() if source.is_relative_to(root) else source.as_posix()}: "
                f"unreadable or unparsable ({type(error).__name__}); its imports are unchecked"
            )
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
    return third_party, sorted(seen), issues


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


def _guarded_nodes(tree: ast.AST) -> set[int]:
    """ids of the nodes inside a `try` body whose handlers catch ImportError."""
    guarded: set[int] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Try):
            continue
        caught = {
            name.id
            for handler in node.handlers
            if handler.type is not None
            for name in ast.walk(handler.type)
            if isinstance(name, ast.Name)
        }
        if caught & _IMPORT_ERRORS:
            guarded.update(id(inner) for stmt in node.body for inner in ast.walk(stmt))
    return guarded


def _imports_of(tree: ast.AST, module: str) -> list[ast.AST]:
    """The nodes of `tree` that import top-level `module`, dynamic ones included."""
    nodes: list[ast.AST] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names = [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom) and not node.level:
            names = [node.module or ""]
        elif isinstance(node, ast.Call):
            names = _dynamic_import_names(ast.Expression(body=node))
        else:
            continue
        if any(name.split(".", 1)[0] == module for name in names):
            nodes.append(node)
    return nodes


def _fallback_holds(module: str, closure: list[Path], root: Path) -> bool:
    """True while every import of the optional `module` in `closure` sits in its
    one declared importer, inside a `try` that catches ImportError."""
    importer = (root / OPTIONAL_IMPORTS[module][0]).resolve()
    for source in closure:
        try:
            tree = ast.parse(source.read_text(encoding="utf-8"))
        except (OSError, SyntaxError, UnicodeDecodeError):
            continue  # scan_imports already reports this file
        nodes = _imports_of(tree, module)
        if not nodes:
            continue
        guarded = _guarded_nodes(tree)
        if source != importer or any(id(node) not in guarded for node in nodes):
            return False
    return True


def findings(root: Path) -> list[str]:
    """The gate's findings against `root`; an empty list means clean."""
    problems: list[str] = []
    makefile = root / MAKEFILE
    lock_input = root / LOCK_INPUT
    if not makefile.is_file():
        raise LookupError(f"{makefile} does not exist")
    if not lock_input.is_file():
        raise LookupError(f"{lock_input} does not exist")

    scripts, recipe_issues = recipe_scripts(makefile)
    for issue in recipe_issues:
        problems.append(f"{MAKEFILE}:{TARGET} recipe: {issue}")
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
        third_party, closure, scan_issues = scan_imports(entry, root)
        for issue in scan_issues:
            problems.append(f"{script.as_posix()}: {issue}")
        optional = {
            name
            for name in third_party & set(OPTIONAL_IMPORTS)
            if _fallback_holds(name, closure, root)
        }
        for module in sorted(third_party - declared - optional):
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
