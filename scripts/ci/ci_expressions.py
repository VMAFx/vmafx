#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Evaluate the subset of GitHub Actions expressions that job-level ``if:`` uses.

The routing contract test (``scripts/ci/tests/test_ci_routing_contract.py``)
needs to know which jobs of the real workflow files run for a synthetic event.
GitHub does not offer that offline, so this module evaluates the expressions
the way the documentation describes them:

* ``&&`` and ``||`` return an operand, not a boolean;
* values of different types compare as numbers (``null == false`` holds);
* strings compare case-insensitively;
* a missing property is ``null``; ``a.*.b`` maps over an array.

The parser is iterative (shunting-yard), as the HISS standard forbids
recursion. Anything outside the subset raises ``ExpressionError`` instead of
guessing: a contract test must not pass on an expression it did not read.
"""

from __future__ import annotations

import json
import math
import re
from collections.abc import Callable, Mapping, Sequence
from typing import Any

TOKEN = re.compile(
    r"""\s*(?:
    (?P<string>'(?:[^']|'')*')
    |(?P<number>-?\d+(?:\.\d+)?)
    |(?P<op>\|\||&&|==|!=|<=|>=|<|>|!|\(|\)|,)
    |(?P<name>[A-Za-z_][A-Za-z0-9_-]*(?:\.(?:\*|[A-Za-z0-9_-]+))*)
    )""",
    re.VERBOSE,
)
PRECEDENCE = {"||": 1, "&&": 2, "==": 3, "!=": 3, "<": 4, ">": 4, "<=": 4, ">=": 4, "!": 5}
BINARY = frozenset(PRECEDENCE) - {"!"}
STATUS_FUNCTIONS = frozenset({"always", "cancelled", "failure", "success"})
MAX_TOKENS = 4096

Token = tuple[str, str]
Value = Any


class ExpressionError(ValueError):
    """The expression uses something outside the supported subset."""


def tokenize(text: str) -> list[Token]:
    """Split an expression into ``(kind, text)`` tokens."""
    tokens: list[Token] = []
    position = 0
    stripped = text.strip()
    while position < len(stripped):
        match = TOKEN.match(stripped, position)
        if match is None or match.end() == position:
            raise ExpressionError(f"cannot tokenize {stripped[position:]!r} in {text!r}")
        kind = match.lastgroup or ""
        tokens.append((kind, match.group(kind)))
        position = match.end()
        if len(tokens) > MAX_TOKENS:
            raise ExpressionError("expression too long")
    return tokens


def strip_wrapper(text: str) -> str:
    """Remove a whole-string ``${{ ... }}`` wrapper, if present."""
    stripped = text.strip()
    if stripped.startswith("${{") and stripped.endswith("}}"):
        return stripped[3:-2]
    return stripped


def _is_function(tokens: Sequence[Token], index: int) -> bool:
    kind, _ = tokens[index]
    following = tokens[index + 1] if index + 1 < len(tokens) else ("", "")
    return kind == "name" and following == ("op", "(")


def _flush_operators(output: list[Token], stack: list[Token], incoming: str) -> None:
    """Pop operators of higher or equal precedence before pushing ``incoming``."""
    while stack and stack[-1][0] == "op" and stack[-1][1] in PRECEDENCE:
        top = stack[-1][1]
        if incoming == "!" or PRECEDENCE[top] < PRECEDENCE[incoming]:
            break
        output.append(stack.pop())


def _close_group(output: list[Token], stack: list[Token], arity: list[list[int]]) -> None:
    while stack and stack[-1] != ("op", "("):
        output.append(stack.pop())
    if not stack:
        raise ExpressionError("unbalanced parentheses")
    stack.pop()
    if stack and stack[-1][0] == "func":
        count, seen = arity.pop()
        name = stack.pop()[1]
        output.append(("call", f"{name}/{count + (1 if seen else 0)}"))
    _mark_operand(arity)


def _mark_operand(arity: list[list[int]]) -> None:
    if arity:
        arity[-1][1] = 1


def to_postfix(tokens: Sequence[Token]) -> list[Token]:
    """Shunting-yard: infix tokens to postfix, function calls as ``name/arity``."""
    output: list[Token] = []
    stack: list[Token] = []
    arity: list[list[int]] = []
    for index, (kind, text) in enumerate(tokens):
        if _is_function(tokens, index):
            stack.append(("func", text))
        elif kind in ("string", "number", "name"):
            output.append((kind, text))
            _mark_operand(arity)
        elif text == "(":
            if stack and stack[-1][0] == "func":
                arity.append([0, 0])
            stack.append((kind, text))
        elif text == ")":
            _close_group(output, stack, arity)
        elif text == ",":
            while stack and stack[-1] != ("op", "("):
                output.append(stack.pop())
            arity[-1][0] += 1
        else:
            _flush_operators(output, stack, text)
            stack.append((kind, text))
    while stack:
        top = stack.pop()
        if top == ("op", "("):
            raise ExpressionError("unbalanced parentheses")
        output.append(top)
    return output


def truthy(value: Value) -> bool:
    if value is None or value is False:
        return False
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return bool(value != 0 and not math.isnan(value))
    return bool(value != "")


def _number(value: Value) -> float:
    if value is None:
        return 0.0
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value) if value.strip() else 0.0
        except ValueError:
            return math.nan
    return math.nan


def loose_equals(left: Value, right: Value) -> bool:
    """Equality as the Actions documentation defines it."""
    if isinstance(left, str) and isinstance(right, str):
        return left.lower() == right.lower()
    if type(left) is type(right) and not isinstance(left, (list, dict)):
        return bool(left == right)
    if isinstance(left, (list, dict)) or isinstance(right, (list, dict)):
        return left is right
    return _number(left) == _number(right)


def resolve(path: str, context: Mapping[str, Any]) -> Value:
    """Walk a dotted path; ``*`` maps over the items of an array or object."""
    values: list[Any] = [context]
    mapped = False
    for part in path.split("."):
        if part == "*":
            values = [
                item
                for value in values
                for item in (list(value.values()) if isinstance(value, dict) else list(value or []))
            ]
            mapped = True
            continue
        values = [value.get(part) if isinstance(value, dict) else None for value in values]
        if not mapped and values[0] is None:
            return None
    return values if mapped else values[0]


def _contains(search: Value, item: Value) -> bool:
    if isinstance(search, list):
        return any(loose_equals(entry, item) for entry in search)
    return str(item or "").lower() in str(search or "").lower()


def _from_json(text: Value) -> Value:
    return json.loads(text) if isinstance(text, str) and text else None


FUNCTIONS: dict[str, Callable[..., Value]] = {
    "contains": _contains,
    "startsWith": lambda text, prefix: str(text or "")
    .lower()
    .startswith(str(prefix or "").lower()),
    "endsWith": lambda text, suffix: str(text or "").lower().endswith(str(suffix or "").lower()),
    "fromJson": _from_json,
    "fromJSON": _from_json,
}


def _apply_binary(operator: str, left: Value, right: Value) -> Value:
    if operator == "&&":
        return right if truthy(left) else left
    if operator == "||":
        return left if truthy(left) else right
    if operator == "==":
        return loose_equals(left, right)
    if operator == "!=":
        return not loose_equals(left, right)
    first, second = _number(left), _number(right)
    ordered = {
        "<": first < second,
        ">": first > second,
        "<=": first <= second,
        ">=": first >= second,
    }
    return ordered[operator]


def _call(name: str, args: list[Value], statuses: Mapping[str, bool]) -> Value:
    function, _, _ = name.partition("/")
    if function in STATUS_FUNCTIONS:
        return statuses[function]
    if function not in FUNCTIONS:
        raise ExpressionError(f"unsupported function {function}()")
    return FUNCTIONS[function](*args)


def _operand(kind: str, text: str, context: Mapping[str, Any]) -> Value:
    if kind == "string":
        return text[1:-1].replace("''", "'")
    if kind == "number":
        return float(text)
    literals = {"true": True, "false": False, "null": None}
    return literals[text] if text in literals else resolve(text, context)


def _step(
    kind: str,
    item: str,
    stack: list[Value],
    context: Mapping[str, Any],
    status: Mapping[str, bool],
) -> None:
    """Apply one postfix token to the value stack."""
    if kind in ("string", "number", "name"):
        stack.append(_operand(kind, item, context))
    elif item == "!":
        stack.append(not truthy(stack.pop()))
    elif kind == "call":
        count = int(item.rpartition("/")[2])
        args = [stack.pop() for _ in range(count)][::-1]
        stack.append(_call(item, args, status))
    else:
        right, left = stack.pop(), stack.pop()
        stack.append(_apply_binary(item, left, right))


def evaluate(
    text: str, context: Mapping[str, Any], statuses: Mapping[str, bool] | None = None
) -> Value:
    """Evaluate one expression against a context."""
    status = statuses or {"always": True, "cancelled": False, "failure": False, "success": True}
    stack: list[Value] = []
    try:
        for kind, item in to_postfix(tokenize(strip_wrapper(text))):
            _step(kind, item, stack, context, status)
    except IndexError as error:
        raise ExpressionError(f"malformed expression {text!r}") from error
    if len(stack) != 1:
        raise ExpressionError(f"malformed expression {text!r}")
    return stack[0]


def uses_status_function(text: str) -> bool:
    """True when an ``if:`` contains a status function (it then overrides the needs result)."""
    return any(
        kind == "name" and item in STATUS_FUNCTIONS for kind, item in tokenize(strip_wrapper(text))
    )
