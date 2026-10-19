#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every documented GPU dispatch variable is read by code that runs (ADR-1571).

`docs/usage/env-vars.md` lists the `VMAF_*_DISPATCH` variables a user can
set. `VMAF_HIP_DISPATCH` was read by `vmaf_hip_dispatch_supports()` and
`VMAF_CUDA_DISPATCH` by `vmaf_cuda_select_strategy()`, and no library code
called either function, so neither variable did anything although the pages
described what they did. This test reads the sources: each documented
variable must be read in `core/src`, the function that reads it must be called
from another function in `core/src`, and every dispatch variable the library
reads must be documented. The checker is also run on planted inputs, so a
broken scanner cannot pass silently.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOCS = ROOT / "docs" / "usage" / "env-vars.md"
SOURCES = ROOT / "core" / "src"
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".mm", ".cu", ".cuh", ".hip"}
BODY_SUFFIXES = SOURCE_SUFFIXES - {".h", ".cuh"}

DOC_ROW = re.compile(r"^\| `(VMAF_[A-Z0-9]+_DISPATCH)` \|", re.M)
READ = re.compile(r'(?:vmaf_gpu_dispatch_env_get|getenv)\(\s*"(VMAF_[A-Z0-9]+_DISPATCH)"\s*\)')
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# A function definition: a return type, the name, a parameter list, a brace.
DEFINITION = re.compile(
    r"^[A-Za-z_][\w\s\*:<>,]*?[\s\*]([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?\{",
    re.M,
)
NOT_TYPES = {"return", "else", "case", "if", "while", "for", "switch", "sizeof"}


def strip_comments(text: str) -> str:
    """Comments blanked with spaces, so offsets and line starts are kept."""
    return COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def documented(markdown: str) -> set[str]:
    return set(DOC_ROW.findall(markdown))


def readers(sources: dict[str, str]) -> dict[str, set[str]]:
    """variable -> names of the functions whose bodies read it."""
    found: dict[str, set[str]] = {}
    for text in sources.values():
        code = strip_comments(text)
        definitions = [(m.start(), m.group(1)) for m in DEFINITION.finditer(code)]
        for read in READ.finditer(code):
            enclosing = [name for start, name in definitions if start < read.start()]
            name = enclosing[-1] if enclosing else "<file scope>"
            found.setdefault(read.group(1), set()).add(name)
    return found


def is_declaration(line: str, name: str) -> bool:
    """True when `name(` on this line is declared or defined, not called."""
    match = re.match(rf"^\s*(?:[A-Za-z_]\w*[\s\*]+)+{name}\s*\(", line)
    if match is None:
        return False
    first = line.split(maxsplit=1)[0] if line.split() else ""
    return first not in NOT_TYPES and "=" not in line[: match.end()]


def callers(sources: dict[str, str], name: str) -> list[str]:
    """`path:line` of each call of `name` in a source file with bodies."""
    calls: list[str] = []
    pattern = re.compile(rf"(?<![\w.>]){name}\s*\(")
    for path, text in sources.items():
        if Path(path).suffix not in BODY_SUFFIXES:
            continue
        for number, line in enumerate(strip_comments(text).splitlines(), start=1):
            if pattern.search(line) and not is_declaration(line, name):
                calls.append(f"{path}:{number}")
    return calls


def failures(markdown: str, sources: dict[str, str]) -> list[str]:
    problems: list[str] = []
    docs = documented(markdown)
    read = readers(sources)
    for variable in sorted(docs):
        if variable not in read:
            problems.append(f"{variable} is documented but nothing in core/src reads it")
            continue
        for function in sorted(read[variable]):
            if not callers(sources, function):
                problems.append(f"{variable} is read by {function}(), which nothing calls")
    for variable in sorted(set(read) - docs):
        problems.append(f"{variable} is read in core/src but not documented in env-vars.md")
    return problems


def tree_sources() -> dict[str, str]:
    return {
        path.relative_to(ROOT).as_posix(): path.read_text(encoding="utf-8", errors="replace")
        for path in sorted(SOURCES.rglob("*"))
        if path.suffix in SOURCE_SUFFIXES and path.is_file()
    }


DOC_FIXTURE = "| `VMAF_X_DISPATCH` | string | `direct` | x |\n"
READER_FIXTURE = (
    "int vmaf_x_select(const char *f)\n{\n"
    '    const char *e = vmaf_gpu_dispatch_env_get("VMAF_X_DISPATCH");\n'
    "    return e != NULL;\n}\n"
)
CALLER_FIXTURE = 'static int use(void)\n{\n    return vmaf_x_select("vif");\n}\n'


class CheckerTest(unittest.TestCase):
    """The checker refuses each defect it exists for."""

    def test_reader_without_caller_fails(self) -> None:
        problems = failures(
            DOC_FIXTURE, {"a.c": READER_FIXTURE, "a.h": "int vmaf_x_select(const char *f);\n"}
        )
        self.assertEqual(
            problems, ["VMAF_X_DISPATCH is read by vmaf_x_select(), which nothing calls"]
        )

    def test_reader_with_caller_passes(self) -> None:
        self.assertEqual(failures(DOC_FIXTURE, {"a.c": READER_FIXTURE, "b.c": CALLER_FIXTURE}), [])

    def test_undocumented_and_unread_variables_fail(self) -> None:
        sources = {"a.c": READER_FIXTURE, "b.c": CALLER_FIXTURE}
        self.assertEqual(
            failures("| `VMAF_Y_DISPATCH` | string | x | x |\n", sources),
            [
                "VMAF_Y_DISPATCH is documented but nothing in core/src reads it",
                "VMAF_X_DISPATCH is read in core/src but not documented in env-vars.md",
            ],
        )

    def test_commented_call_is_no_caller(self) -> None:
        commented = '/* vmaf_x_select("vif"); */\n// vmaf_x_select("adm");\n'
        self.assertEqual(
            failures(DOC_FIXTURE, {"a.c": READER_FIXTURE, "b.c": commented}),
            ["VMAF_X_DISPATCH is read by vmaf_x_select(), which nothing calls"],
        )


class TreeTest(unittest.TestCase):
    def test_every_documented_dispatch_variable_is_consulted(self) -> None:
        problems = failures(DOCS.read_text(encoding="utf-8"), tree_sources())
        self.assertEqual(problems, [], "\n".join(problems))

    def test_the_documented_set(self) -> None:
        self.assertEqual(
            documented(DOCS.read_text(encoding="utf-8")),
            {"VMAF_CUDA_DISPATCH", "VMAF_SYCL_DISPATCH"},
        )


if __name__ == "__main__":
    unittest.main()
