# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Every Dockerfile stage that runs a tester script has what the script imports.

`docker/Dockerfile.tester` copies only part of the repository into each build
stage. A script of `tools/rc1-tester/image/` that imports `vmaf_rc1_tester` runs
only in a stage that also holds `tools/rc1-tester/src`; without it the image
build dies with `ModuleNotFoundError`, which no other test sees because the
scripts run from a full checkout everywhere else.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DOCKERFILE = ROOT / "docker" / "Dockerfile.tester"
PACKAGE = "vmaf_rc1_tester"
PACKAGE_SOURCE = "tools/rc1-tester/src"
SCRIPT_RE = re.compile(r"python3\s+(?:/repo/|/src/)?(tools/rc1-tester/image/\w+\.py)")
# The stages that configure the Meson tree. `meson setup` resolves every
# `files()` argument at configure time, so a file the tree names outside
# `core/` must be in each of them (a missing one failed every GPU image).
MESON_ROOT = "core"
MESON_BUILD_STAGES = ("vmaf-build", "sycl-build", "cuda-build", "hip-build")
MESON_FILES_RE = re.compile(r"files\(([^)]*)\)")
QUOTED_RE = re.compile(r"'([^']+)'")


def logical_lines(text: str) -> list[str]:
    """Join backslash continuations into one instruction line each."""
    lines: list[str] = []
    pending = ""
    for raw in text.splitlines():
        stripped = raw.strip()
        if not pending and stripped.startswith("#"):
            continue
        if stripped.endswith("\\"):
            pending += stripped[:-1] + " "
            continue
        lines.append(pending + stripped)
        pending = ""
    return lines


def parse_stages(text: str) -> dict[str, dict]:
    """Stage name -> parent stage, repository paths present, scripts run."""
    stages: dict[str, dict] = {}
    current: dict | None = None
    for line in logical_lines(text):
        head = re.match(r"FROM\s+(\S+)(?:\s+AS\s+(\S+))?", line, re.IGNORECASE)
        if head:
            current = {"parent": head.group(1), "paths": set(), "scripts": set()}
            stages[head.group(2) or head.group(1)] = current
            continue
        if current is None:
            continue
        if line.startswith("COPY ") and "--from=" not in line:
            current["paths"].update(t.rstrip("/") for t in line.split()[1:-1] if "=" not in t)
        for source in re.findall(r"--mount=type=bind,source=([^,\s]+)", line):
            if "from=" not in line.split(source)[0].split("--mount")[-1]:
                current["paths"].add(source.rstrip("/"))
        if line.startswith("RUN "):
            current["scripts"].update(SCRIPT_RE.findall(line))
    return stages


def holds(stages: dict[str, dict], name: str, path: str) -> bool:
    """True when the stage, or a stage it is built FROM, copies `path` or a parent of it."""
    seen: set[str] = set()
    while name in stages and name not in seen:
        seen.add(name)
        if any(path == p or path.startswith(p + "/") for p in stages[name]["paths"]):
            return True
        name = stages[name]["parent"]
    return False


def imports_package(script: Path) -> bool:
    tree = ast.parse(script.read_text(encoding="utf-8"))
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and (node.module or "").split(".")[0] == PACKAGE:
            return True
        if isinstance(node, ast.Import) and any(
            a.name.split(".")[0] == PACKAGE for a in node.names
        ):
            return True
    return False


def missing_imports(text: str) -> list[str]:
    stages = parse_stages(text)
    found = []
    for name, stage in stages.items():
        for script in sorted(stage["scripts"]):
            if imports_package(ROOT / script) and not holds(stages, name, PACKAGE_SOURCE):
                found.append(f"{name}: {script} imports {PACKAGE}, stage lacks {PACKAGE_SOURCE}")
    return found


def test_every_stage_has_the_package_its_scripts_import() -> None:
    assert missing_imports(DOCKERFILE.read_text(encoding="utf-8")) == []


def test_the_check_sees_the_build_stages_scripts() -> None:
    stages = parse_stages(DOCKERFILE.read_text(encoding="utf-8"))
    runners = {
        n for n, s in stages.items() if any(x.endswith("prepare_build.py") for x in s["scripts"])
    }
    assert {"vmaf-build", "sycl-build", "cuda-build", "hip-build"} <= runners


def test_a_stage_without_the_package_is_refused() -> None:
    dockerfile = (
        "FROM base AS build\n"
        "COPY tools/rc1-tester/image tools/rc1-tester/image\n"
        "RUN python3 tools/rc1-tester/image/prepare_build.py info /x\n"
    )
    assert len(missing_imports(dockerfile)) == 1
    fixed = dockerfile.replace("RUN", "COPY tools/rc1-tester/src tools/rc1-tester/src\nRUN")
    assert missing_imports(fixed) == []


def meson_inputs_outside(root: Path) -> set[str]:
    """Repository paths outside `core/` that a `files()` call of the Meson tree names."""
    found: set[str] = set()
    for build in sorted((root / MESON_ROOT).rglob("meson.build")):
        if "subprojects" in build.relative_to(root).parts:
            continue
        for call in MESON_FILES_RE.findall(build.read_text(encoding="utf-8")):
            for rel in QUOTED_RE.findall(call):
                path = (build.parent / rel).resolve()
                if not path.is_relative_to(root):
                    continue
                repo_rel = path.relative_to(root)
                if repo_rel.parts and repo_rel.parts[0] != MESON_ROOT:
                    found.add(repo_rel.as_posix())
    return found


def missing_meson_inputs(text: str, inputs: set[str]) -> list[str]:
    stages = parse_stages(text)
    return [
        f"{stage}: meson setup reads {path}, the stage does not copy it"
        for stage in MESON_BUILD_STAGES
        for path in sorted(inputs)
        if not holds(stages, stage, path)
    ]


def test_every_meson_stage_has_the_files_the_tree_names() -> None:
    inputs = meson_inputs_outside(ROOT)
    # The reference that broke the CUDA, SYCL and HIP images on 2026-10-05.
    assert "scripts/ci/exact_twin_matrix.py" in inputs
    assert missing_meson_inputs(DOCKERFILE.read_text(encoding="utf-8"), inputs) == []


def test_a_meson_stage_without_an_input_is_refused() -> None:
    stages = "".join(f"FROM base AS {name}\nCOPY core core\n" for name in MESON_BUILD_STAGES)
    inputs = {"scripts/ci/x.py"}
    assert len(missing_meson_inputs(stages, inputs)) == len(MESON_BUILD_STAGES)
    fixed = stages.replace("COPY core core\n", "COPY core core\nCOPY scripts/ci/x.py scripts/ci/\n")
    assert missing_meson_inputs(fixed, inputs) == []


def test_the_scan_reads_multi_argument_files_calls(tmp_path: Path) -> None:
    (tmp_path / "core" / "test").mkdir(parents=True)
    (tmp_path / "core" / "test" / "meson.build").write_text(
        "x = files('a.c', '../../scripts/one.py',\n  '../../tools/two.py')\n", encoding="utf-8"
    )
    assert meson_inputs_outside(tmp_path) == {"scripts/one.py", "tools/two.py"}
