#!/usr/bin/env python3
"""Find non-constant initialisers inside `static` aggregates in C sources.

In C, a `static const double x = 1.0;` declares a *const-qualified object*,
not a constant expression (C23 6.6). An object with static storage duration
must be initialised by constant expressions (6.7.11p4), so

    static const double golden = 72.5;
    static const Case cases[] = { { golden, "msg" } };   /* invalid C */

is ill-formed. GCC and Clang accept it as a silent extension and emit no
diagnostic even under `-std=c23 -pedantic-errors -Weverything`. MSVC's C
frontend rejects it with C2099 "initializer is not a constant", then drops
the offending initialiser and mis-pairs the rest of the braced list, which
surfaces as a cascade of confusing C2440 conversion errors on the *following*
members.

That combination -- silent locally, loud only on a required Windows check --
is why this is a preflight rule. The fix is to spell such constants as
object-like macros (or enum constants for integers), which are constant
expressions in every context.

Prints `path:line: name` for each offending reference. Exit status is always
0; the caller decides what a hit means.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# `static const <scalar type> NAME = ...;` at file scope. Pointer and array
# declarators are excluded: an array name is not usable as an initialiser
# scalar anyway, and a `static const char *const s` is a constant expression
# for pointer members on the platforms we target.
DECL_RE = re.compile(
    r"^static\s+const\s+"
    r"(?:signed\s+|unsigned\s+|long\s+|short\s+)*"
    r"(?:double|float|int|char|long|short|size_t|ptrdiff_t|u?int(?:8|16|32|64)_t)"
    r"\s+(?P<name>\w+)\s*=",
)

# A `static` declaration whose initialiser is a braced list. Matching the
# opening line is enough; the block is closed by brace depth.
OPEN_RE = re.compile(r"^\s*static\b.*=\s*\{")

# Strip string and character literals so a name occurring inside a message
# does not count as a reference.
LITERAL_RE = re.compile(r'"(?:[^"\\]|\\.)*"' r"|'(?:[^'\\]|\\.)*'")


def scan(path: str) -> list[str]:
    try:
        with Path(path).open(encoding="utf-8", errors="replace") as handle:
            lines = handle.readlines()
    except OSError:
        return []

    names = {m.group("name") for line in lines if (m := DECL_RE.match(line))}
    if not names:
        return []
    name_re = re.compile(r"\b(" + "|".join(sorted(map(re.escape, names))) + r")\b")

    hits: list[str] = []
    depth = 0
    for lineno, raw in enumerate(lines, start=1):
        line = LITERAL_RE.sub('""', raw)
        if depth == 0 and not OPEN_RE.match(line):
            continue
        if depth > 0 or OPEN_RE.match(line):
            for found in name_re.findall(line):
                # The declaration line itself opens no aggregate, so any hit
                # inside a tracked block is a genuine reference.
                hits.append(f"{path}:{lineno}: {found}")
            depth += line.count("{") - line.count("}")
            depth = max(depth, 0)
    return hits


def main(argv: list[str]) -> int:
    for path in argv[1:]:
        for hit in scan(path):
            print(hit)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
