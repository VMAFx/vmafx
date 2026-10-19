#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""torch only where training runs (ADR-1886).

PyTorch carries advisories with no fixed release (PYSEC-2025-189 and others,
see security/vex/torch.openvex.json). The two training environments may
depend on it: ``ai/`` (vmaf-train) and ``tools/ensemble-training-kit/``. No
other package may name a torch-family distribution in any dependency group,
and no hash lock inside another package may resolve one.

Checked per tracked ``pyproject.toml`` outside the training roots:
``project.dependencies``, every ``project.optional-dependencies`` group, every
``dependency-groups`` group and ``build-system.requires``. Below a package root
other than the repository root, every ``requirements*.txt`` / ``requirements*.in``
is checked line by line as well. Configuration that only names a module (a mypy
override list, for example) is not a dependency and is not read.

Exit 0 clean, 1 a violation, 2 a file that cannot be read.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Any

import tomllib

#: Distributions that are torch or pull it in.
TORCH_FAMILY = frozenset(
    {
        "torch",
        "torchvision",
        "torchaudio",
        "torchao",
        "torchmetrics",
        "pytorch-lightning",
        "lightning",
    }
)

#: Package roots that train models and may depend on torch.
TRAINING_ROOTS = ("ai", "tools/ensemble-training-kit")

_NAME = re.compile(r"^\s*([A-Za-z0-9][A-Za-z0-9._-]*)")
_REQUIREMENT_FILE = re.compile(r"(^|/)requirements[^/]*\.(txt|in)$")


def canonical(name: str) -> str:
    """PEP 503 normalised distribution name."""
    return re.sub(r"[-_.]+", "-", name).lower()


def requirement_name(spec: str) -> str | None:
    """The distribution a PEP 508 requirement or lock line names, if any."""
    stripped = spec.strip()
    if not stripped or stripped.startswith(("#", "-", "--")):
        return None
    match = _NAME.match(stripped)
    return canonical(match.group(1)) if match else None


def is_training(path: str) -> bool:
    return any(path == root or path.startswith(root + "/") for root in TRAINING_ROOTS)


def declared_requirements(document: dict[str, Any]) -> Iterable[tuple[str, str]]:
    """(group, requirement) for every dependency field of a pyproject."""
    project = document.get("project", {})
    for spec in project.get("dependencies", []):
        yield "dependencies", spec
    for group, specs in project.get("optional-dependencies", {}).items():
        for spec in specs:
            yield f"optional-dependencies.{group}", spec
    for group, specs in document.get("dependency-groups", {}).items():
        for spec in specs:
            if isinstance(spec, str):
                yield f"dependency-groups.{group}", spec
    for spec in document.get("build-system", {}).get("requires", []):
        yield "build-system.requires", spec


def pyproject_violations(path: str, text: str) -> list[str]:
    document = tomllib.loads(text)
    return [
        f"{path}: {group} names {name} ({spec.strip()})"
        for group, spec in declared_requirements(document)
        if (name := requirement_name(spec)) in TORCH_FAMILY
    ]


def requirement_file_violations(path: str, text: str) -> list[str]:
    found = []
    for number, line in enumerate(text.splitlines(), start=1):
        name = requirement_name(line)
        if name in TORCH_FAMILY:
            found.append(f"{path}:{number}: resolves {name}")
    return found


def scope(files: list[str]) -> tuple[list[str], list[str]]:
    """(runtime pyprojects, requirement files below a runtime package root)."""
    pyprojects = [f for f in files if f.endswith("pyproject.toml") and not is_training(f)]
    roots = [str(Path(f).parent) for f in pyprojects if "/" in f]
    requirements = [
        f
        for f in files
        if _REQUIREMENT_FILE.search(f)
        and not is_training(f)
        and any(f.startswith(root + "/") for root in roots)
    ]
    return pyprojects, requirements


def violations(files: list[str], read: Callable[[str], str]) -> list[str]:
    pyprojects, requirements = scope(files)
    found: list[str] = []
    for path in pyprojects:
        found += pyproject_violations(path, read(path))
    for path in requirements:
        found += requirement_file_violations(path, read(path))
    return found


def tracked_files(root: Path) -> list[str]:
    git = shutil.which("git") or "/usr/bin/git"
    result = subprocess.run(  # noqa: S603 -- fixed argv, resolved git
        [git, "-C", str(root), "ls-files"], capture_output=True, text=True, check=True
    )
    return result.stdout.splitlines()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    try:
        found = violations(
            tracked_files(args.root),
            lambda path: (args.root / path).read_text(encoding="utf-8"),
        )
    except (
        OSError,
        UnicodeDecodeError,
        tomllib.TOMLDecodeError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"check-torch-scope: cannot read the tree: {error}", file=sys.stderr)
        return 2
    for line in found:
        print(f"check-torch-scope: {line}", file=sys.stderr)
    if found:
        print(
            "check-torch-scope: torch belongs to the training environments only "
            f"({', '.join(TRAINING_ROOTS)}; ADR-1886)",
            file=sys.stderr,
        )
        return 1
    print("check-torch-scope: OK (torch only in " + ", ".join(TRAINING_ROOTS) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
