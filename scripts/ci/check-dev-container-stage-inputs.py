#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that every dev/Containerfile stage copies the repository files it reads.

CI builds dev/Containerfile only up to libvmaf-build (ADR-0819), so a later
stage that reads a file no stage copied, or copies a path the repository does
not contain, reaches master with every check green. This static check closes
that gap without a build (ADR-1343):

* every pip requirement or constraint file a RUN reads under the build root
  must be provided by a COPY into that stage or one of its parent stages, and
  the repository file it maps to must exist;
* every COPY source that does not come from another stage must exist.

Instruction parsing is shared with check-container-image-references.py.
"""

from __future__ import annotations

import importlib.util
import re
import shlex
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
CONTAINERFILE = "dev/Containerfile"
BUILD_ROOT = "/build/vmaf/"
# A COPY needs at least one source and a destination; "FROM image AS name" is
# three tokens.
MIN_COPY_PATHS = 2
FROM_AS_TOKENS = 3
REQUIREMENT_ARG = re.compile(r"(?:^|\s)(?:-r|-c|--requirement|--constraint)(?:=|\s+)(\S+)")

_SPEC = importlib.util.spec_from_file_location(
    "container_image_references", ROOT / "scripts/ci/check-container-image-references.py"
)
assert _SPEC is not None and _SPEC.loader is not None
_PARSER = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_PARSER)


class Stage:
    """One build stage: its parent stage, working directory and repository COPYs."""

    def __init__(self, parent: Stage | None, workdir: str) -> None:
        self.parent = parent
        self.workdir = workdir
        self.copies: list[tuple[str, list[str]]] = []

    def lineage(self) -> list[Stage]:
        stages: list[Stage] = []
        stage: Stage | None = self
        while stage is not None:
            stages.append(stage)
            stage = stage.parent
        return stages


def absolute(path: str, workdir: str) -> str:
    """Resolve a COPY destination against the stage WORKDIR, keeping a trailing slash."""
    if path.startswith("/"):
        return path
    joined = str(PurePosixPath(workdir) / path)
    return joined + "/" if path.endswith("/") else joined


def repository_source(path: str, stage: Stage) -> str | None:
    """Map an in-image path to the repository path a COPY provided it from."""
    for ancestor in stage.lineage():
        for dest, sources in ancestor.copies:
            if dest.endswith("/"):
                for source in sources:
                    if source.endswith("/") and path.startswith(dest):
                        return source + path[len(dest) :]
                    if not source.endswith("/") and path == dest + PurePosixPath(source).name:
                        return source
            elif path == dest and len(sources) == 1:
                return sources[0]
    return None


def pip_requirement_paths(arguments: str) -> list[str]:
    """Requirement and constraint files read by the pip-install segments of a RUN."""
    paths = []
    for segment in re.split(r"&&|;|\|\|", arguments):
        if "pip" in segment and "install" in segment:
            paths.extend(match.group(1) for match in REQUIREMENT_ARG.finditer(segment))
    return paths


def check_copy(stage: Stage, lineno: int, tokens: list[str], root: Path) -> list[str]:
    flags = [token for token in tokens if token.startswith("--")]
    paths = [token for token in tokens if not token.startswith("--")]
    if any(flag.startswith("--from") for flag in flags) or len(paths) < MIN_COPY_PATHS:
        return []
    *sources, dest = paths
    stage.copies.append((absolute(dest, stage.workdir), sources))
    problems = []
    for source in sources:
        if not any(root.glob(source.rstrip("/"))):
            problems.append(f"{CONTAINERFILE}:{lineno} COPY source {source} does not exist")
    return problems


def check_run(stage: Stage, lineno: int, arguments: str, root: Path) -> list[str]:
    problems = []
    for path in pip_requirement_paths(arguments):
        if not path.startswith(BUILD_ROOT):
            continue
        source = repository_source(path, stage)
        if source is None:
            problems.append(
                f"{CONTAINERFILE}:{lineno} RUN reads {path}, but no COPY into this stage "
                "or its parents provides it"
            )
        elif not (root / source).is_file():
            problems.append(f"{CONTAINERFILE}:{lineno} RUN reads {path}; {source} does not exist")
    return problems


def check(text: str, root: Path = ROOT) -> list[str]:
    stages: dict[str, Stage] = {}
    current: Stage | None = None
    problems: list[str] = []
    for lineno, command, arguments in _PARSER.logical_instructions(text):
        if command == "FROM":
            tokens = [token for token in shlex.split(arguments) if not token.startswith("--")]
            parent = stages.get(tokens[0].lower())
            current = Stage(parent, parent.workdir if parent else "/")
            name = tokens[2] if len(tokens) >= FROM_AS_TOKENS and tokens[1].upper() == "AS" else ""
            stages[name.lower() or f"#{len(stages)}"] = current
        elif current is None:
            continue
        elif command == "WORKDIR":
            current.workdir = absolute(shlex.split(arguments)[0], current.workdir)
        elif command == "COPY":
            problems.extend(check_copy(current, lineno, shlex.split(arguments), root))
        elif command == "RUN":
            problems.extend(check_run(current, lineno, arguments, root))
    return problems


def main() -> int:
    problems = check((ROOT / CONTAINERFILE).read_text(encoding="utf-8"))
    for problem in problems:
        print(f"dev-container-stage-inputs: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("dev-container-stage-inputs: OK — every stage copies the repository files it reads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
