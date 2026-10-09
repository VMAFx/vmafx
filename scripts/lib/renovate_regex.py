#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Renovate's regular expressions, evaluated by the repository's Python tests.

Renovate compiles `matchStrings`, `extractVersionTemplate` and `/regex/` file
patterns with RE2 or JavaScript RegExp, which spell a named group
`(?<name>...)`. Python's `re` spells it `(?P<name>...)` and rejects the other
form. The patterns in renovate.json otherwise use syntax the three engines
share (docs/research/renovate-file-pattern-delimiters.md), so a test that
converts the named groups evaluates the configured pattern itself.
"""

from __future__ import annotations

import re

# A named group: `(?<` followed by an identifier and `>`. A lookbehind,
# `(?<=` or `(?<!`, has no identifier and is left as it is.
JS_GROUP = re.compile(r"\(\?<([A-Za-z_][A-Za-z0-9_]*)>")


def to_python(pattern: str) -> str:
    """`pattern` with every JavaScript named group spelled for Python's `re`."""
    return JS_GROUP.sub(r"(?P<\1>", pattern)
