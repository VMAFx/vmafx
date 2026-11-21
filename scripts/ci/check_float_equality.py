#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Compile ``core/test/`` C sources with -Wfloat-equal (issue 3303).

Semgrep cannot see the return type of a header-declared function or the field
type of an anonymous struct, so a comparison like

    vmaf_float_motion_scaler(table[i].bpc) == table[i].expected

slips past ``vmaf-test-float-identity-via-float-bits``.  A compiler knows both
types.  This check reuses the flags of every ``core/test`` translation unit in
a Meson ``compile_commands.json``, adds ``-fsyntax-only -Wfloat-equal
-Werror=float-equal`` and fails on a diagnostic that the Semgrep rule would not
itself exempt: a comparison against a literal, or the ``x != x`` NaN idiom.

The check fails closed when the compile database is missing, or when a source
fails to compile for a reason other than the float-equality diagnostic.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run

FLOAT_EQUAL = "[-Wfloat-equal]"
FLOAT_EQUAL_TAGS = (
    "[-Wfloat-equal]",
    "[-Werror=float-equal]",
    "[-Werror,-Wfloat-equal]",
)
DIAGNOSTIC = re.compile(
    r"^(?P<path>.+?):(?P<line>\d+):(?P<col>\d+): (?P<level>error|warning): "
    r"(?P<message>.*?)(?:" + "|".join(re.escape(tag) for tag in FLOAT_EQUAL_TAGS) + r")\s*$"
)
COMPILER_ERROR = re.compile(r"(?:^|:\s)(?:fatal )?error:")
LITERAL = (
    r"(?:[+-] *)?(?:"
    r"0[xX](?:[0-9a-fA-F]+\.?[0-9a-fA-F]*|\.[0-9a-fA-F]+)[pP][+-]?[0-9]+[fFlL]?"
    r"|(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?[fFlL]?"
    r"|(?:0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+)[uUlL]*)"
)
NAME = r"[A-Za-z_][A-Za-z_0-9]*"
# Only boundaries that cannot join the simple token to a higher-precedence
# expression. Line ends alone cannot prove a boundary in multi-line C code.
LEFT_BOUNDARY = r"(?:\breturn\b|[([{;,?:]|&&|\|\||(?<![<>=!])=)"
RIGHT_BOUNDARY = r"(?:[)\]};,?:]|&&|\|\|)"
LEFT_OPERAND = re.compile(rf"{LEFT_BOUNDARY}\s*(?P<operand>{LITERAL}|{NAME})\s*\Z")
RIGHT_OPERAND = re.compile(rf"\s*(?P<operand>{LITERAL}|{NAME})\s*{RIGHT_BOUNDARY}")
NUMERIC_LITERAL = re.compile(LITERAL)
AMBIGUOUS_SOURCE = re.compile(r"""["'\\#]|/\*|\*/""")


def load_units(
    build_dir: Path, root: Path, prefix: str = "core/test/"
) -> list[tuple[Path, list[str], Path]]:
    """Return ``(source, argv, directory)`` for tracked *prefix* C units."""
    compdb = build_dir / "compile_commands.json"
    try:
        entries = json.loads(compdb.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ValueError(f"cannot read {compdb}: {error}") from error
    units: list[tuple[Path, list[str], Path]] = []
    for entry in entries:
        directory = Path(entry.get("directory", build_dir))
        source = Path(entry.get("file", ""))
        if not source.is_absolute():
            source = directory / source
        try:
            relative = source.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            continue
        if not relative.startswith(prefix) or source.suffix != ".c":
            continue
        arguments = entry.get("arguments")
        if isinstance(arguments, list):
            argv = [str(argument) for argument in arguments]
        else:
            argv = shlex.split(str(entry.get("command", "")))
        if not argv:
            raise ValueError(f"{relative}: compile command has no arguments")
        units.append((source, argv, directory))
    return sorted(units)


def syntax_only(argv: list[str]) -> list[str]:
    """Drop ``-o`` output and ask the compiler not to link."""
    result: list[str] = []
    skip = False
    for argument in argv:
        if skip:
            skip = False
            continue
        if argument == "-o":
            skip = True
            continue
        if argument.startswith("-o") and argument != "-o":
            continue
        result.append(argument)
    result.extend(["-fsyntax-only", "-Wfloat-equal", "-Werror=float-equal"])
    return result


def source_line(path: Path, number: int) -> str:
    if number < 1:
        return ""
    try:
        return path.read_text(encoding="utf-8").splitlines()[number - 1]
    except (OSError, IndexError, UnicodeError):
        return ""


def diagnostic_is_exempt(line: str, column: int) -> bool:
    """Exempt a proven literal operand or same-name NaN test at this column."""
    # Keep the compiler's offsets. Reject ambiguous lexical context and
    # prefixes where byte offsets and display columns can differ.
    code = line.partition("//")[0]
    index = column - 1
    if index < 0 or not code[:index].isascii() or "\t" in code[:index]:
        return False
    operator = code[index : index + 2]
    if operator not in ("==", "!=") or AMBIGUOUS_SOURCE.search(code):
        return False
    left = LEFT_OPERAND.search(code[:index])
    right = RIGHT_OPERAND.match(code[index + 2 :])
    if left is not None and NUMERIC_LITERAL.fullmatch(left["operand"]):
        return True
    if right is not None and NUMERIC_LITERAL.fullmatch(right["operand"]):
        return True
    return bool(operator == "!=" and left and right and left["operand"] == right["operand"])


def classify_diagnostics(output: str, directory: Path) -> tuple[list[str], list[str], bool]:
    """Return unexempt float findings, other errors and whether a float error occurred."""
    result: list[str] = []
    other_errors: list[str] = []
    float_error = False
    for diagnostic in output.splitlines():
        match = DIAGNOSTIC.match(diagnostic)
        if not match:
            if COMPILER_ERROR.search(diagnostic):
                other_errors.append(diagnostic)
            continue
        float_error |= match.group("level") == "error"
        path = Path(match.group("path"))
        line = int(match.group("line"))
        column = int(match.group("col"))
        if diagnostic_is_exempt(source_line(directory / path, line), column):
            continue
        result.append(f"{path}:{line}:{column}: {match.group('message').strip()}{FLOAT_EQUAL}")
    return result, other_errors, float_error


def findings(output: str, directory: Path) -> list[str]:
    """Keep only float-equal diagnostics the rule does not exempt."""
    return classify_diagnostics(output, directory)[0]


def check(
    build_dir: Path, root: Path, compiler: str | None = None, prefix: str = "core/test/"
) -> list[str]:
    units = load_units(build_dir, root, prefix)
    if not units:
        raise ValueError(f"compile database selected no {prefix} C sources")
    problems: list[str] = []
    for source, argv, directory in units:
        command = syntax_only([compiler or argv[0], *argv[1:]])
        result = run(
            command,
            allowed_executables=(command[0],),
            capture_output=True,
            text=True,
            check=False,
            cwd=str(directory),
            timeout_seconds=120,
        )
        if result.returncode == 0:
            continue
        output = result.stdout + result.stderr
        diagnostic_findings, other_errors, float_error = classify_diagnostics(output, directory)
        problems.extend(diagnostic_findings)
        problems.extend(other_errors)
        # Only an ordinary compilation failure explained by float errors can be
        # excused. Warnings alone or a compiler crash must still fail closed.
        if result.returncode != 1 or (not float_error and not other_errors):
            problems.append(
                f"{source}: compiler exited {result.returncode} without an explaining float-equal error"
            )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--compiler")
    options = parser.parse_args(argv)
    try:
        problems = check(options.build_dir, options.root, options.compiler)
    except ValueError as error:
        print(f"check_float_equality: {error}", file=sys.stderr)
        return 2
    if problems:
        for problem in problems:
            print(problem)
        print(f"check_float_equality: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
