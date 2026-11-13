# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""The one shared Conventional-Commit definition (issue #2750).

Three consumers read this module so a type cannot exist in one place and
drift away in another:

* the ``PR Title Gate`` (``scripts/ci/check-pr-title.py``, wired as
  ``PR Title Gate`` in ``rule-enforcement.yml``) validates every
  pull-request title with :func:`is_conventional`;
* the PR-type label workflow derives its ``type:*`` label with
  :func:`parse_type` / ``check-pr-title.py --emit-label``;
* the commit-msg hook (``compilerla/conventional-pre-commit``, pinned
  v4.5.0 in ``.pre-commit-config.yaml``) is a remote pre-commit hook and
  cannot import Python, so ``scripts/ci/tests/test_conventional_commits_shared.py``
  pins its ``args`` list to :data:`TYPES` — a drift in either file fails
  that test.

Scopes are free-form: the hook passes no scope list and none exists to
share; a scope such as ``(sycl)`` or ``(cache)`` is whatever the subject
needs.

The validation logic mirrors the pinned upstream hook's
``conventional_pre_commit/format.py`` (Apache-2.0, compilerla/
conventional-pre-commit v4.5.0) so the gate refuses exactly what the
commit-msg hook refuses; see the test module for the mirrored cases.
"""

from __future__ import annotations

import re
from typing import Optional

# The exact types the commit-msg hook enforces, in the order
# .pre-commit-config.yaml passes them (test-pinned; keep both in step).
# `security` and `style` are deliberately absent: no commit on master
# ever carried them, and the hook refuses them, so the label workflow
# must not label what the hook would reject.
TYPES: tuple[str, ...] = (
    "feat",
    "fix",
    "perf",
    "refactor",
    "docs",
    "test",
    "build",
    "ci",
    "chore",
    "revert",
    "port",
    "sycl",
    "cuda",
    "simd",
)

# type -> the `type:*` label the release notes group by (.github/release.yml).
LABELS: dict[str, str] = {
    "feat": "type:feature",
    "fix": "type:bug",
    "perf": "type:perf",
    "refactor": "type:refactor",
    "docs": "type:docs",
    "test": "type:test",
    "build": "type:build",
    "ci": "type:ci",
    "chore": "type:chore",
    "revert": "type:revert",
    "port": "type:port",
    "sycl": "type:sycl",
    "cuda": "type:cuda",
    "simd": "type:simd",
}

# The missing-component names :func:`errors` can report, in the order the
# upstream hook lists them.
_COMPONENTS = ("type", "scope", "delim", "subject", "body")


def _r_types() -> str:
    """The case-insensitive alternation of :data:`TYPES`."""
    return "(?i:" + "|".join(sorted(TYPES)) + ")"


def _r_scope() -> str:
    """The optional free-form ``(scope)`` group, delimiters as upstream."""
    escaped = list(map(re.escape, [":", ",", "-", "/", ".", "#"]))
    return "(" + r"\([\w " + "".join(escaped) + r"]+\)" + ")?"


def _regex() -> "re.Pattern[str]":
    """The full Conventional-Commit pattern, groups named as upstream."""
    pattern = (
        "^(?P<type>"
        + _r_types()
        + ")?"
        + "(?P<scope>"
        + _r_scope()
        + ")?"
        + "(?P<delim>"
        + r"!?:"
        + ")?"
        + "(?P<subject>"
        + r" .+$"
        + ")?"
        + "(?P<body>"
        + r"(?P<multi>\r?\n(?P<sep>^$\r?\n)?(?P<break>(?i:BREAKING[ -]CHANGE: ))?.+)?"
        + ")?"
    )
    return re.compile(pattern, re.MULTILINE)


def _strip_comments(message: str) -> str:
    """Drop ``#`` comment lines, as git leaves them in COMMIT_EDITMSG."""
    return re.sub(r"^#.*\r?\n?", "", message, flags=re.MULTILINE)


def parse_type(title: str) -> Optional[str]:
    """The leading Conventional-Commit type of ``title``, or ``None``.

    Matches the prefix contract the label workflow always used: the first
    word before ``(``, ``!`` or ``:``, lower-cased, only when it is one of
    :data:`TYPES`. A title that merely carries a known prefix passes even
    if the rest of the title is malformed — the PR Title Gate refuses
    malformed titles, the label workflow only needs the type.
    """
    match = re.match(r"^[A-Za-z]+", title or "")
    if not match:
        return None
    candidate = match.group(0).lower()
    return candidate if candidate in TYPES else None


def errors(message: str) -> list[str]:
    """The missing Conventional-Commit components of ``message``.

    With an invalid type the rest of the components go unmatched even when
    the structure is right, because a valid type must come first; like the
    upstream hook, only the type is then reported.
    """
    match = _regex().match(_strip_comments(message or ""))
    groups = match.groupdict() if match else {}
    if not groups.get("type"):
        groups.pop("delim", None)
        groups.pop("subject", None)
        groups.pop("body", None)
    # Scopes are optional (the hook runs without --force-scope): a missing
    # scope is not a missing component.
    groups.pop("scope", None)
    if not groups.get("body"):
        groups.pop("multi", None)
        groups.pop("sep", None)
    return [name for name in _COMPONENTS if name in groups and not groups[name]]


def _is_valid_body(match: "re.Match[str]") -> bool:
    """A body must be separated by a blank line; a ``BREAKING CHANGE:``
    footer must be upper case, exactly as the hook requires."""
    if not match.group("body"):
        return True
    if match.group("multi") and match.group("sep"):
        if match.group("break"):
            return match.group("break").isupper()
        return True
    return False


def is_conventional(message: str) -> bool:
    """True when ``message`` is a Conventional Commit the hook would accept."""
    match = _regex().match(_strip_comments(message or ""))
    return bool(match) and all(
        [
            match.group("type"),
            match.group("delim"),
            match.group("subject"),
            _is_valid_body(match),
        ]
    )
