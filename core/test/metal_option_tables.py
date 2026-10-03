#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Option tables of a CPU extractor and its Metal twin, read from the sources.

test_metal_twin_option_parity.c compares the tables of every Metal twin with
its CPU extractor's on an Apple device; the Objective-C++ twins do not build
anywhere else. The device-free contracts of the Metal ports (ADR-1498) read
both `static const VmafOption options[]` tables from the sources with this
module and compare them the way that test does: name, alias, type, default,
range (INT and DOUBLE options only, as its same_default()) and flags.

The reader understands designated initializers (`.default_val.b = true`,
`.default_val = {.b = true}`), entries written through a function-like macro
of the same file (cambi.c's CAMBI_OPTION), and values written as object-like
macros or enum constants of the file and of the local headers it includes.
"""

from __future__ import annotations

import ast
import operator
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FEATURE = ROOT / "core" / "src" / "feature"
INCLUDE_DIRS = (FEATURE, ROOT / "core" / "src", ROOT / "core" / "include")

TABLE_START = re.compile(r"static\s+const\s+VmafOption\s+\w+\s*\[\s*\]\s*=\s*\{")
OBJECT_MACRO = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+)$", re.M)
FUNCTION_MACRO = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)\(([^)]*)\)[ \t]*(.*)$", re.M)
LOCAL_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
ENUM_BODY = re.compile(r"\benum\b[^{;]*\{([^}]*)\}", re.S)
NUMBER_SUFFIX = re.compile(r"(?<=[0-9.])[uUlLfF]+\b")
IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")
NULLS = {"NULL", "nullptr", "CAMBI_NULL_POINTER", "0"}
CONSTANTS = {
    "DBL_MAX": repr(sys.float_info.max),
    "INT_MAX": "2147483647",
    "true": "True",
    "false": "False",
}
DEFAULT_BY_TYPE = {"VMAF_OPT_TYPE_BOOL": False, "VMAF_OPT_TYPE_INT": 0, "VMAF_OPT_TYPE_DOUBLE": 0.0}
OPT_PREFIX = "VMAF_OPT_"
RANGED_TYPES = {"VMAF_OPT_TYPE_INT", "VMAF_OPT_TYPE_DOUBLE"}
UNARY = {ast.USub: operator.neg, ast.UAdd: operator.pos}
BINARY = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Div: operator.truediv,
    ast.LShift: operator.lshift,
    ast.RShift: operator.rshift,
}


@dataclass(frozen=True)
class Option:
    """One VmafOption entry, normalised."""

    name: str
    alias: str | None
    type: str
    default: object
    min: float
    max: float
    flags: frozenset[str]


def _skip_literal(text: str, i: int) -> int:
    """Index after the string or character literal starting at text[i]."""
    quote = text[i]
    i += 1
    while i < len(text) and text[i] != quote:
        i += 2 if text[i] == "\\" else 1
    return i + 1


def strip_comments(text: str) -> str:
    """The source with C and C++ comments replaced by a space; literals kept."""
    out: list[str] = []
    i = 0
    while i < len(text):
        if text[i] in "\"'":
            end = _skip_literal(text, i)
            out.append(text[i:end])
            i = end
        elif text.startswith("//", i):
            newline = text.find("\n", i)
            i = len(text) if newline < 0 else newline
        elif text.startswith("/*", i):
            i = text.index("*/", i + 2) + 2
            out.append(" ")
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _balanced_end(text: str, start: int) -> int:
    """Index of the bracket closing the one at text[start]."""
    depth = 0
    i = start
    while i < len(text):
        if text[i] in "\"'":
            i = _skip_literal(text, i)
            continue
        depth += text[i] in "({["
        depth -= text[i] in ")}]"
        if depth == 0:
            return i
        i += 1
    raise ValueError("unbalanced brackets")


def split_top(text: str) -> list[str]:
    """Comma-separated items of `text` at bracket depth 0, stripped, non-empty."""
    items: list[str] = []
    depth = 0
    start = 0
    i = 0
    while i < len(text):
        if text[i] in "\"'":
            i = _skip_literal(text, i)
            continue
        depth += text[i] in "({["
        depth -= text[i] in ")}]"
        if text[i] == "," and depth == 0:
            items.append(text[start:i].strip())
            start = i + 1
        i += 1
    items.append(text[start:].strip())
    return [item for item in items if item]


def options_block(text: str) -> str:
    """The inside of the file's `static const VmafOption x[] = {...}`."""
    match = TABLE_START.search(text)
    if not match:
        raise ValueError("no static const VmafOption table")
    end = _balanced_end(text, match.end() - 1)
    return text[match.end() : end]


def _resolve_include(name: str, base: Path) -> Path | None:
    for directory in (base, *INCLUDE_DIRS):
        candidate = (directory / name).resolve()
        if candidate.is_file():
            return candidate
    return None


def _enum_constants(text: str) -> dict[str, str]:
    constants: dict[str, str] = {}
    for body in ENUM_BODY.findall(text):
        value = -1
        for item in split_top(body):
            name, _, expr = item.partition("=")
            value = int(expr.strip(), 0) if expr.strip().lstrip("-").isdigit() else value + 1
            constants[name.strip()] = str(value)
    return constants


def _local_headers(text: str, path: Path, depth: int) -> list[tuple[str, Path]]:
    """(text, path) of the local headers `text` includes, `depth` levels deep,
    innermost first, so the including file's definitions win."""
    found: list[tuple[str, Path]] = []
    frontier = [(text, path)]
    for _ in range(depth):
        nested: list[tuple[str, Path]] = []
        for source, source_path in frontier:
            for name in LOCAL_INCLUDE.findall(source):
                header = _resolve_include(name, source_path.parent)
                if header is not None:
                    nested.append((strip_comments(header.read_text()), header))
        found = nested + found
        frontier = nested
    return found


def collect_macros(text: str, path: Path, depth: int = 3) -> dict[str, str]:
    """Object-like macros and enum constants of `text` and its local includes."""
    macros: dict[str, str] = {}
    for source, _ in [*_local_headers(text, path, depth), (text, path)]:
        macros.update(_enum_constants(source))
        macros.update((name, value.strip()) for name, value in OBJECT_MACRO.findall(source))
    # The option types and flags compare by name, as spelled in opt.h.
    return {name: value for name, value in macros.items() if not name.startswith(OPT_PREFIX)}


def _function_macros(text: str) -> dict[str, tuple[list[str], str]]:
    joined = text.replace("\\\n", " ")
    return {
        name: ([p.strip() for p in params.split(",")], body)
        for name, params, body in FUNCTION_MACRO.findall(joined)
    }


def _expand_call(item: str, macros: dict[str, tuple[list[str], str]]) -> str:
    """A function-like macro entry `NAME(args)` expanded to its braced body."""
    name = item[: item.index("(")].strip()
    params, body = macros[name]
    args = split_top(item[item.index("(") + 1 : _balanced_end(item, item.index("("))])
    table = dict(zip(params, args, strict=True))
    return IDENTIFIER.sub(lambda m: table.get(m.group(0), m.group(0)), body)


def _substitute(expr: str, macros: dict[str, str]) -> str:
    """`expr` with macros replaced until none is left (bounded)."""
    for _ in range(8):
        new = IDENTIFIER.sub(lambda m: macros.get(m.group(0), m.group(0)), expr)
        if new == expr:
            break
        expr = new
    return expr


def _children(node: ast.AST) -> list[ast.AST]:
    if isinstance(node, ast.UnaryOp) and type(node.op) in UNARY:
        return [node.operand]
    if isinstance(node, ast.BinOp) and type(node.op) in BINARY:
        return [node.left, node.right]
    raise ValueError(f"not a literal expression: {ast.dump(node)}")


def _arith(root: ast.AST) -> object:
    """The value of a literal arithmetic expression (numbers, + - * / << >>), by
    an explicit post-order walk: no names, no calls, no recursion."""
    pending: list[tuple[ast.AST, bool]] = [(root, False)]
    values: list[object] = []
    for _ in range(4096):
        if not pending:
            return values.pop()
        node, expanded = pending.pop()
        if isinstance(node, ast.Constant) and isinstance(node.value, (int, float, bool)):
            values.append(node.value)
        elif not expanded:
            pending.append((node, True))
            pending.extend((child, False) for child in reversed(_children(node)))
        elif isinstance(node, ast.UnaryOp):
            values.append(UNARY[type(node.op)](values.pop()))
        else:
            right = values.pop()
            values.append(BINARY[type(node.op)](values.pop(), right))
    raise ValueError("expression too long")


def _value(expr: str, macros: dict[str, str]) -> object:
    """A scalar initializer: a string, None, a bool or a number."""
    expr = _substitute(expr.strip(), macros).strip()
    while expr.startswith("(") and _balanced_end(expr, 0) == len(expr) - 1:
        expr = expr[1:-1].strip()
    if expr.startswith('"'):
        return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', expr))
    if expr in {"NULL", "nullptr"}:
        return None
    expr = IDENTIFIER.sub(lambda m: CONSTANTS.get(m.group(0), m.group(0)), expr)
    return _arith(ast.parse(NUMBER_SUFFIX.sub("", expr), mode="eval").body)


def _flags(expr: str, macros: dict[str, str]) -> frozenset[str]:
    expr = _substitute(expr, macros).replace("(", " ").replace(")", " ")
    return frozenset(part.strip() for part in expr.split("|") if part.strip() not in NULLS)


def _fields(entry: str) -> dict[str, str]:
    """`.a.b = v` items of a braced entry as {"a.b": "v"}; nested braces flattened."""
    fields: dict[str, str] = {}
    for item in split_top(entry.strip()[1:-1]):
        key, _, value = item.partition("=")
        key = key.strip().lstrip(".")
        value = value.strip()
        if value.startswith("{") and key == "default_val":
            inner_key, _, inner_value = value[1:-1].strip().partition("=")
            fields["default_val." + inner_key.strip().lstrip(".")] = inner_value.strip()
        else:
            fields[key] = value
    return fields


def _option(fields: dict[str, str], macros: dict[str, str]) -> Option:
    otype = _substitute(fields["type"], macros).strip("() ")
    member = next((k for k in fields if k.startswith("default_val.")), None)
    default = _value(fields[member], macros) if member else DEFAULT_BY_TYPE.get(otype)
    alias = _value(fields["alias"], macros) if "alias" in fields else None
    return Option(
        name=_value(fields["name"], macros),
        alias=alias,
        type=otype,
        default=default,
        min=float(_value(fields.get("min", "0"), macros)),
        max=float(_value(fields.get("max", "0"), macros)),
        flags=_flags(fields.get("flags", "0"), macros),
    )


def table_of_text(text: str, path: Path) -> list[Option]:
    """The options of the table in `text`, which is the source at `path`."""
    code = strip_comments(text)
    macros = collect_macros(code, path)
    function_macros = _function_macros(code)
    options: list[Option] = []
    for item in split_top(options_block(code)):
        entry = item if item.startswith("{") else _expand_call(item, function_macros)
        fields = _fields(entry)
        if "name" not in fields or _value(fields["name"], macros) is None:
            break
        options.append(_option(fields, macros))
    return options


def table(path: Path) -> list[Option]:
    return table_of_text(path.read_text(encoding="utf-8"), path)


def _same(a: Option, b: Option) -> bool:
    if (a.name, a.alias, a.type, a.default, a.flags) != (
        b.name,
        b.alias,
        b.type,
        b.default,
        b.flags,
    ):
        return False
    return a.type not in RANGED_TYPES or (a.min, a.max) == (b.min, b.max)


def differences(cpu: list[Option], twin: list[Option]) -> list[str]:
    """Why two tables differ, both directions; empty when they are equal."""
    found: list[str] = []
    twin_by_name = {opt.name: opt for opt in twin}
    cpu_by_name = {opt.name: opt for opt in cpu}
    for opt in cpu:
        other = twin_by_name.get(opt.name)
        if other is None:
            found.append(f"the twin lacks {opt.name}")
        elif not _same(opt, other):
            found.append(f"{opt.name}: cpu {opt} twin {other}")
    found += [f"the CPU has no {opt.name}" for opt in twin if opt.name not in cpu_by_name]
    return found
