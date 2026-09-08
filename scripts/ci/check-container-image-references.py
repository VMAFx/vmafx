#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check FROM/COPY references for the ADR-1231 base-image gate.

The shell caller supplies image-key names and tracked Dockerfiles. This helper
handles instruction case, flags and continuations; Docker still validates the
complete build syntax. No registry access or Docker daemon is needed.
"""

from __future__ import annotations

import re
import shlex
import sys
from collections.abc import Iterator
from pathlib import Path

VARIABLE = re.compile(r"\$(?:\{([A-Z][A-Z0-9_]*)\}|([A-Z][A-Z0-9_]*))\Z")
# These two consumers extend images built by this repository. A tag alone is
# never evidence that an arbitrary image is local (ADR-1231).
LOCAL_FROM = {("Dockerfile.ffmpeg", "vmaf:latest")}
LOCAL_ARGUMENT = {("dev/Containerfile.runner", "BASE_IMAGE", "vmaf-dev-mcp:local")}


def instructions(text: str) -> Iterator[tuple[int, str, list[str]]]:
    """Read relevant logical instructions, retaining their first line number."""
    pending = ""
    start = 0
    escape = "\\"
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("#"):
            directive = re.fullmatch(r"#\s*escape\s*=\s*([\\`])", stripped, re.IGNORECASE)
            if directive and not start:
                escape = directive.group(1)
            continue
        if not stripped:
            continue
        if not pending:
            start = number
        if stripped.endswith(escape):
            pending += stripped[: -len(escape)] + " "
            continue
        logical = pending + stripped
        pending = ""
        # split() also accepts tabs between the instruction and its arguments.
        command, *body = logical.split(maxsplit=1)
        command = command.upper()
        if command in {"ARG", "FROM", "COPY"}:
            arguments = body[0] if body else ""
            yield start, command, shlex.split(arguments)
    if pending:
        raise ValueError(f"line {start}: unterminated instruction continuation")


def check_from(
    path: Path, ref: str, image_keys: set[str], stages: set[str], arguments: dict[str, str]
) -> str | None:
    variable = VARIABLE.fullmatch(ref)
    if variable:
        key = variable.group(1) or variable.group(2)
        local = (str(path), key, arguments.get(key)) in LOCAL_ARGUMENT
        central = key in image_keys and bool(arguments.get(key))
        if not (central or local):
            return f"FROM argument {key} needs a global default owned by build-config.env"
    elif ref != "scratch" and ref.lower() not in stages and (str(path), ref) not in LOCAL_FROM:
        return f"FROM hardcodes a base image: {ref}"
    return None


def check_file(path: Path, image_keys: set[str]) -> list[str]:
    text = path.read_text(encoding="utf-8")
    parsed = list(instructions(text))
    stages: set[str] = set()
    arguments: dict[str, str] = {}
    stage_count = 0
    problems = []
    for lineno, command, tokens in parsed:
        if command == "ARG" and stage_count == 0:
            for token in tokens:
                key, separator, value = token.partition("=")
                # The shared mirror writer owns one ARG default per physical
                # line. A continued declaration must not evade that check.
                declaration = rf"\s*(?i:ARG)\s+{re.escape(key)}="
                if separator and re.match(declaration, text.splitlines()[lineno - 1]):
                    arguments[key] = value
            continue
        if command not in {"FROM", "COPY"}:
            continue
        flags = []
        while tokens and tokens[0].startswith("--"):
            flags.append(tokens.pop(0))
        if command == "COPY":
            refs = [flag.partition("=")[2] for flag in flags if flag.split("=")[0] == "--from"]
            if not refs:
                continue
            ref = refs[0]
            local = ref.lower() in stages or (ref.isdecimal() and int(ref) < stage_count - 1)
            if len(refs) != 1 or not local:
                problems.append(
                    f"{path}:{lineno} COPY --from hardcodes a base image or unknown stage: {ref}"
                )
            continue
        if not tokens:
            raise ValueError(f"{path}:{lineno} FROM has no image")
        problem = check_from(path, tokens[0], image_keys, stages, arguments)
        if problem:
            problems.append(f"{path}:{lineno} {problem}")
        match tokens:
            case [_, keyword, alias] if keyword.upper() == "AS":
                stages.add(alias.lower())
        stage_count += 1
    return problems


def main() -> int:
    image_keys = set(sys.argv[1].split())
    problems = []
    for name in sys.argv[2:]:
        try:
            problems.extend(check_file(Path(name), image_keys))
        except (OSError, ValueError) as error:
            problems.append(f"{name}: {error}")
    for problem in problems:
        print(f"::error title=base-image drift::{problem}", file=sys.stderr)
    if problems:
        print(
            "Use a build-config.env ARG default and a named stage for external images.",
            file=sys.stderr,
        )
    return int(bool(problems))


if __name__ == "__main__":
    raise SystemExit(main())
