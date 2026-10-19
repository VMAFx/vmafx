#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hold the root licence files and every package's licence field to the files.

ADR-1250 makes each file's SPDX header the truth about its licence: fork-authored
files are EUPL-1.2, files that carry Netflix's or another project's code keep those
terms. Two places restate that truth for a reader who never opens a file, and both
are checked here against the files themselves (ADR-1699):

- **The repository root.** GitHub, licence scanners and people read the root
  licence files as the project's terms. The root holds one licence file,
  `LICENSE`, byte for byte `LICENSES/EUPL-1.2.txt`, and `NOTICE`, Netflix's
  BSD-2-Clause-Patent text with its copyright notice, under a name licence
  detectors do not read as a licence file, so `LICENSE` is the only root licence
  file they find. Every other root file whose name licensee 10.1.0 scores as a
  licence file (`LICENSE*`, `LICENCE*`, `COPYING*`, `COPYRIGHT*`, `OFL*`,
  `PATENTS*`, `<x>-LICENSE*`) is refused: a root `LICENSE-MIT` offered a
  permissive branch ADR-1250 withdrew.
- **Package manifests.** Every `pyproject.toml`, `Cargo.toml`, Helm `Chart.yaml`
  and `package.json` that declares a licence declares exactly the licences of the
  files its package ships, read with the licence tool's own reader
  (`tools/rc1-tester/image/licensing.py`: the file's SPDX header, else
  `REUSE.toml`). What a package ships is read from its build configuration; a
  configuration this module does not model fails instead of being guessed.

Exit status: 0 everything agrees, 1 a problem was found (one line each), 2 the
check could not run.
"""

from __future__ import annotations

import argparse
import ast
import fnmatch
import functools
import importlib.util
import json
import os
import re
import runpy
import sys
import tarfile
import types
from pathlib import Path
from typing import Any
from unittest import mock

import tomllib

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.lib.safe_subprocess import (  # noqa: E402
    CommandTimedOut,
    CommandValidationError,
)
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

LICENSING_TOOL = REPO_ROOT / "tools" / "rc1-tester" / "image" / "licensing.py"
LICENSING_MANIFEST = Path("tools") / "rc1-tester" / "image" / "licensing.json"
GIT_TIMEOUT_S = 60

# ------------------------------------------------------------------ root layout

EUPL_TEXT = "LICENSES/EUPL-1.2.txt"
NETFLIX_TEXT = "NOTICE"
ROOT_LICENCE_FILES = ("LICENSE",)
NETFLIX_MARKERS = (
    "SPDX short identifier: BSD-2-Clause-Patent",
    "Copyright (c) 2020 Netflix, Inc.",
)
# The file names licensee 10.1.0 (the detector behind GitHub's licence field) gives
# a non-zero score as licence files: `Licensee::ProjectFiles::LicenseFile::
# FILENAME_REGEXES`, without its catch-all, matched case-insensitively.
_LIC, _COPYING, _COPYRIGHT = r"(?:un)?licen[sc]e", r"copying", r"copyright"
_PREFERRED_EXT = r"\.(?:md|markdown|txt|html)"
_LICENSE_EXT = r"\.(?!spdx|header)(?:[^./]|\.\d)+"
_OTHER_EXT = r"\.(?!xml|sh|go|gemspec)(?:[^./]|\.\d)+"
_ANY_EXT = r"\.(?:[^./]|\.\d)+"
LICENCE_FILE_NAMES = tuple(
    re.compile(pattern, re.IGNORECASE)
    for pattern in (
        rf"{_LIC}(?:{_PREFERRED_EXT}|{_LICENSE_EXT})?",
        rf"{_COPYING}(?:{_PREFERRED_EXT}|{_ANY_EXT})?",
        rf"(?:{_LIC}|{_COPYING}|{_COPYRIGHT})[-_][^.]*(?:{_OTHER_EXT})?",
        rf"\w+[-_](?:{_LIC}|{_COPYING})[^.]*(?:{_OTHER_EXT})?",
        rf"ofl(?:{_PREFERRED_EXT}|{_OTHER_EXT})?",
        rf"{_COPYRIGHT}(?:{_PREFERRED_EXT}|{_OTHER_EXT})?",
        rf"patents(?:{_OTHER_EXT})?",
    )
)

# ------------------------------------------------------------------ packages

MANIFEST_NAMES = frozenset({"pyproject.toml", "Cargo.toml", "Chart.yaml", "package.json"})
# Manifests that describe no package, with the reason. Every other manifest that
# declares no licence fails.
NOT_PACKAGES = {
    "pyproject.toml": "the repository's tool configuration, never built or published (ADR-1127)",
}
# Upper bound on the files one extension's include closure may visit.
MAX_INCLUDE_VISITS = 1 << 14
INCLUDE_LINE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"]+)[>"]', re.MULTILINE)
CYTHON_EXTERN_LINE = re.compile(r'^[ \t]*cdef[ \t]+extern[ \t]+from[ \t]+"([^"]+)"', re.MULTILINE)
HATCH_BACKEND = "hatchling.build"
# hatchling options that change which files the sdist holds.
HATCH_SELECTION_KEYS = {"include", "exclude", "only-include", "packages", "sources"}
SETUPTOOLS_BACKENDS = ("setuptools.build_meta", "setuptools.build_meta:__legacy__")
# Cargo keys that change which files a crate holds, or add a licence file.
CARGO_SELECTION_KEYS = ("include", "exclude", "license-file")
# `artifacthub.io/license`, quoted or not: one SPDX identifier, or an AND
# expression of them where the chart's files carry several (ADR-2673).
CHART_LICENCE = re.compile(
    r"^[ \t]+artifacthub\.io/license:[ \t]*([\"']?)([^\"'#\n]*?)\1[ \t]*(?:#.*)?$", re.MULTILINE
)
SPDX_AND_EXPRESSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9.+-]*(?: AND [A-Za-z0-9][A-Za-z0-9.+-]*)*")
# Largest Chart.yaml read from a subchart archive, and where it sits in one
# (`<chart name>/Chart.yaml`).
MAX_CHART_BYTES = 1 << 20
SUBCHART_MANIFEST_DEPTH = 2


class ModelError(Exception):
    """A package configuration this module does not model."""


def _load_licensing_tool() -> types.ModuleType:
    spec = importlib.util.spec_from_file_location("vmafx_licensing", LICENSING_TOOL)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {LICENSING_TOOL}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


licensing = _load_licensing_tool()


def git_environment(root: Path) -> dict[str, str]:
    """The environment git runs in for `root`. In this checkout a commit hook's
    GIT_INDEX_FILE names the staged state, which is what to check; for any other
    tree (a test fixture) the caller's GIT_* variables would point git at the
    caller's repository, so they are dropped."""
    if root.resolve() == REPO_ROOT.resolve():
        return dict(os.environ)
    return {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}


def tracked_files(root: Path, paths: list[Path]) -> set[Path]:
    """The regular files git tracks under `paths`, resolved."""
    relative = [path.resolve().relative_to(root.resolve()).as_posix() for path in paths]
    result = run_command(
        ["git", "-C", str(root), "ls-files", "-z", "--", *relative],
        allowed_executables=("git",),
        env=git_environment(root),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=GIT_TIMEOUT_S,
    )
    if result.returncode != 0:
        raise ModelError(f"git ls-files exited {result.returncode} in {root}: {result.stderr}")
    files = (root / name for name in result.stdout.split("\0") if name)
    return {path.resolve() for path in files if path.is_file()}


def _relative(root: Path, path: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


@functools.cache
def reuse_annotations(root: Path) -> list[dict[str, Any]]:
    annotations: list[dict[str, Any]] = licensing.load_reuse(root)
    return annotations


def shipped_licences(root: Path, files: set[Path]) -> tuple[set[str], list[str]]:
    """(identifiers in the files' SPDX expressions, files that state none)."""
    identifiers: set[str] = set()
    unlicensed: list[str] = []
    for path in sorted(files):
        relative = _relative(root, path)
        expression, _ = licensing.file_licence(path, relative, reuse_annotations(root))
        if expression:
            identifiers |= licensing.spdx_ids(expression)
        else:
            unlicensed.append(relative)
    return identifiers, unlicensed


def declared_problems(name: str, field: str, declared: object, identifiers: set[str]) -> list[str]:
    """Why `declared` is not the AND of exactly `identifiers`. An OR would let a
    recipient take one branch, which is how a permissive option defeats ADR-1250."""
    if not isinstance(declared, str) or not declared.strip() or " OR " in f" {declared} ":
        return [f"{name}: {field} must be an SPDX AND expression, is {declared!r}"]
    if licensing.spdx_ids(declared) != identifiers:
        carried = " AND ".join(sorted(identifiers))
        return [f"{name}: {field} = {declared!r}, but its files carry {carried}"]
    return []


def _unlicensed_problems(unlicensed: list[str]) -> list[str]:
    return [f"{path} states no licence (SPDX header or REUSE.toml)" for path in unlicensed]


# ------------------------------------------------------------------ root layout


def _root_text_problems(root: Path) -> list[str]:
    problems = []
    licence, eupl = root / "LICENSE", root / EUPL_TEXT
    if not licence.is_file() or not eupl.is_file() or licence.read_bytes() != eupl.read_bytes():
        problems.append(
            f"LICENSE is not {EUPL_TEXT} byte for byte: fork-authored code is EUPL-1.2 (ADR-1250)"
        )
    netflix = root / NETFLIX_TEXT
    if not netflix.is_file():
        problems.append(
            f"{NETFLIX_TEXT} is missing: Netflix's BSD-2-Clause-Patent text and copyright "
            "notice stay at the root (ADR-1250, ADR-1699)"
        )
        return problems
    text = netflix.read_text(encoding="utf-8", errors="replace")
    missing = [marker for marker in NETFLIX_MARKERS if marker not in text]
    if missing:
        problems.append(
            f"{NETFLIX_TEXT} lacks {missing}: it holds Netflix's BSD-2-Clause-Patent text and "
            "copyright notice (ADR-1250, ADR-1699)"
        )
    return problems


def _artifact_text_problems(root: Path) -> list[str]:
    """The artifacts' licence texts are the root's: Netflix's file for its licence."""
    manifest = root / LICENSING_MANIFEST
    if not manifest.is_file():
        return [f"{LICENSING_MANIFEST.as_posix()} is missing"]
    texts = licensing.load_manifest(manifest)["spdx_texts"]
    wanted = {"BSD-2-Clause-Patent": NETFLIX_TEXT, "EUPL-1.2": EUPL_TEXT}
    return [
        f"{LICENSING_MANIFEST.as_posix()}: spdx_texts[{identifier!r}] is "
        f"{texts.get(identifier)!r}, not {path!r}"
        for identifier, path in wanted.items()
        if texts.get(identifier) != path
    ]


def is_licence_file_name(name: str) -> bool:
    """Whether licensee reads a root file of this name as a licence file."""
    return any(pattern.fullmatch(name) for pattern in LICENCE_FILE_NAMES)


def root_licence_problems(root: Path, files: list[str]) -> list[str]:
    """Why the root licence files contradict ADR-1250's layout (ADR-1699)."""
    allowed = ", ".join(ROOT_LICENCE_FILES)
    problems = [
        f"{name}: a second root licence file (allowed: {allowed}; Netflix's text is "
        f"{NETFLIX_TEXT})"
        for name in sorted(files)
        if "/" not in name and is_licence_file_name(name) and name not in ROOT_LICENCE_FILES
    ]
    return problems + _root_text_problems(root) + _artifact_text_problems(root)


# ------------------------------------------------------------------ Python packages
#
# hatchling: the source distribution holds every tracked file of the project
# directory and the nearest .gitignore (hatchling's default sdist selection), and
# the wheel adds its force-included paths; a package that configures its own sdist
# selection fails until it is modelled. setuptools (`vmaf`): the wheel holds the
# modules of the listed packages, their package data, and the extension compiled
# from every `.pyx` in them plus the sources setup.py appends, so it ships the code
# of each repository file that extension includes (ADR-1560).


def extension_include_dirs(setup_py: Path) -> list[str]:
    """The string entries of every `include_dirs = [...]` assignment in `setup_py`."""
    include_dirs: list[str] = []
    for node in ast.walk(ast.parse(setup_py.read_text(encoding="utf-8"))):
        if not isinstance(node, ast.Assign) or not isinstance(node.value, ast.List):
            continue
        if not any(
            isinstance(target, ast.Attribute) and target.attr == "include_dirs"
            for target in node.targets
        ):
            continue
        include_dirs.extend(
            element.value
            for element in node.value.elts
            if isinstance(element, ast.Constant) and isinstance(element.value, str)
        )
    return include_dirs


def appended_extension_sources(setup_py: Path) -> set[tuple[str, ...]]:
    """The path parts of every `<list>.append(os.path.join(...))` call in `setup_py`."""
    return {
        tuple(
            part.value
            for part in node.args[0].args
            if isinstance(part, ast.Constant) and isinstance(part.value, str)
        )
        for node in ast.walk(ast.parse(setup_py.read_text(encoding="utf-8")))
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "append"
        and len(node.args) == 1
        and isinstance(node.args[0], ast.Call)
    }


def nearest_gitignore(root: Path, project_dir: Path) -> list[Path]:
    """The .gitignore hatchling copies into the sdist: the first one found from
    the project directory up to the repository root."""
    top = root.resolve()
    for directory in (project_dir.resolve(), *project_dir.resolve().parents):
        if (directory / ".gitignore").is_file():
            return [directory / ".gitignore"]
        if directory == top:
            break
    return []


def _hatch_shipped(root: Path, project_dir: Path, data: dict[str, Any]) -> set[Path]:
    build = data.get("tool", {}).get("hatch", {}).get("build", {})
    targets = build.get("targets", {})
    if HATCH_SELECTION_KEYS & set(build) or "sdist" in targets:
        raise ModelError(f"{project_dir} selects its own sdist files; model that selection")
    force_included = targets.get("wheel", {}).get("force-include", {})
    roots = [project_dir, *nearest_gitignore(root, project_dir)]
    return tracked_files(root, [*roots, *(project_dir / source for source in force_included)])


def setup_arguments(setup_py: Path) -> dict[str, Any]:
    """The keyword arguments `setup_py` passes to setuptools.setup(), without
    building and without setuptools: the script imports a stand-in whose
    `setup` only records them."""
    captured: dict[str, Any] = {}
    stand_in = types.ModuleType("setuptools")
    vars(stand_in)["setup"] = lambda **kwargs: captured.update(kwargs)
    with mock.patch.dict(sys.modules, {"setuptools": stand_in}):
        runpy.run_path(str(setup_py), run_name="vmafx_setup_metadata_probe")
    return captured


def _package_directory(project_dir: Path, package_dir: dict[str, str], package: str) -> Path:
    """Where setuptools reads `package` from, given setup()'s `package_dir`."""
    parts = package.split(".")
    for cut in range(len(parts), -1, -1):
        prefix = ".".join(parts[:cut])
        if prefix in package_dir:
            return (project_dir / package_dir[prefix]).joinpath(*parts[cut:]).resolve()
    return project_dir.joinpath(*parts).resolve()


def _included_files(source: Path, include_dirs: list[Path], root: Path) -> list[Path]:
    """Repository files `source` includes (`#include`, Cython `cdef extern from`),
    resolved as a C preprocessor does: the includer's directory, then the include
    path. Conditional inclusion is not evaluated, so the set can only be larger."""
    text = source.read_text(encoding="utf-8", errors="replace")
    found = []
    for name in INCLUDE_LINE.findall(text) + CYTHON_EXTERN_LINE.findall(text):
        for base in (source.parent, *include_dirs):
            candidate = Path(os.path.normpath(base / name))
            if candidate.is_file() and candidate.resolve().is_relative_to(root.resolve()):
                found.append(candidate.resolve())
                break
    return found


def include_closure(sources: list[Path], include_dirs: list[Path], root: Path) -> set[Path]:
    """`sources` and every repository file they include, transitively."""
    seen: set[Path] = set()
    pending = [source.resolve() for source in sources]
    for _ in range(MAX_INCLUDE_VISITS):
        if not pending:
            return seen
        source = pending.pop()
        if source not in seen:
            seen.add(source)
            pending.extend(_included_files(source, include_dirs, root))
    raise ModelError(f"the include closure of {sources} exceeds {MAX_INCLUDE_VISITS} visits")


def _setuptools_shipped(root: Path, project_dir: Path) -> set[Path]:
    setup_py = project_dir / "setup.py"
    arguments = setup_arguments(setup_py)
    shipped: set[Path] = set()
    extension_sources = [
        project_dir.joinpath(*parts) for parts in appended_extension_sources(setup_py)
    ]
    for package in arguments["packages"]:
        directory = _package_directory(project_dir, arguments.get("package_dir", {}), package)
        patterns = arguments.get("package_data", {}).get(package, [])
        for path in tracked_files(root, [directory]):
            relative = path.relative_to(directory).as_posix()
            module = path.parent == directory and path.suffix == ".py"
            if module or any(fnmatch.fnmatchcase(relative, pattern) for pattern in patterns):
                shipped.add(path)
            if path.parent == directory and path.suffix == ".pyx":
                extension_sources.append(path)
    include_dirs = [project_dir / entry for entry in extension_include_dirs(setup_py)]
    return shipped | include_closure(extension_sources, include_dirs, root)


def python_shipped_files(root: Path, pyproject: Path, data: dict[str, Any]) -> set[Path]:
    """Every repository file the package's sdist or wheel carries."""
    backend = data["build-system"].get("build-backend", "setuptools.build_meta:__legacy__")
    if backend == HATCH_BACKEND:
        return _hatch_shipped(root, pyproject.parent, data)
    if backend in SETUPTOOLS_BACKENDS:
        return _setuptools_shipped(root, pyproject.parent)
    raise ModelError(f"{pyproject}: what the build backend {backend!r} ships is not modelled")


def python_licence_text_problems(
    name: str, project_dir: Path, project: dict[str, Any], identifiers: set[str]
) -> list[str]:
    """PEP 639 texts: `LICENSES/<id>.txt` for exactly the shipped identifiers,
    byte for byte the repository's copy (licensing.json `spdx_texts`)."""
    if project.get("license-files") != ["LICENSES/*"]:
        return [
            f"{name}: license-files must be ['LICENSES/*'], is {project.get('license-files')!r}"
        ]
    shipped = {path.stem for path in (project_dir / "LICENSES").glob("*.txt")}
    problems = [f"{name}/LICENSES lacks {i}.txt" for i in sorted(identifiers - shipped)]
    problems += [
        f"{name}/LICENSES/{i}.txt is no shipped licence" for i in sorted(shipped - identifiers)
    ]
    texts = licensing.load_manifest()["spdx_texts"]
    for identifier in sorted(identifiers & shipped):
        reference = REPO_ROOT / texts[identifier]
        if not reference.is_file():
            problems.append(f"{name}/LICENSES/{identifier}.txt: {texts[identifier]} is missing")
        elif (
            project_dir / "LICENSES" / f"{identifier}.txt"
        ).read_bytes() != reference.read_bytes():
            problems.append(f"{name}/LICENSES/{identifier}.txt differs from {texts[identifier]}")
    return problems


def python_package_problems(root: Path, pyproject: Path) -> list[str]:
    """Why `pyproject`'s licence metadata does not describe the files it ships."""
    name = _relative(root, pyproject.parent)
    data = tomllib.loads(pyproject.read_text(encoding="utf-8"))
    project = data["project"]
    texts = {
        path.resolve()
        for pattern in project.get("license-files", [])
        for path in pyproject.parent.glob(pattern)
    }
    shipped = python_shipped_files(root, pyproject, data) - texts
    identifiers, unlicensed = shipped_licences(root, shipped)
    problems = _unlicensed_problems(unlicensed)
    problems += declared_problems(name, "license", project.get("license"), identifiers)
    return problems + python_licence_text_problems(name, pyproject.parent, project, identifiers)


# ------------------------------------------------------------------ Rust crates


def _cargo_workspace(root: Path, manifest: Path) -> tuple[Path, dict[str, Any]] | None:
    """The nearest Cargo.toml above `manifest` with a [workspace] table."""
    top = root.resolve()
    for directory in manifest.parent.resolve().parents:
        candidate = directory / "Cargo.toml"
        if candidate.is_file():
            data = tomllib.loads(candidate.read_text(encoding="utf-8"))
            if "workspace" in data:
                return candidate, data
        if directory == top:
            break
    return None


def _cargo_field(
    package: dict[str, Any], key: str, workspace: tuple[Path, dict[str, Any]] | None
) -> object:
    """`package[key]`, or the workspace's value when the crate inherits it."""
    value = package.get(key)
    if not (isinstance(value, dict) and value.get("workspace") is True):
        return value
    if workspace is None:
        raise ModelError(f"{key}.workspace = true without a workspace")
    return workspace[1]["workspace"].get("package", {}).get(key)


def _cargo_readme(
    crate: Path, package: dict[str, Any], workspace: tuple[Path, dict[str, Any]] | None
) -> list[Path]:
    """The readme cargo copies into the crate from outside its directory. Cargo
    resolves an inherited path from the workspace root and keeps the crate's own
    file of the same name when there is one."""
    readme = _cargo_field(package, "readme", workspace)
    if not isinstance(readme, str):
        return []
    inherited = isinstance(package.get("readme"), dict)
    base = workspace[0].parent if inherited and workspace else crate
    path = (base / readme).resolve()
    if path.is_relative_to(crate) or (crate / path.name).exists() or not path.is_file():
        return []
    return [path]


def cargo_shipped_files(root: Path, manifest: Path, data: dict[str, Any]) -> set[Path]:
    """Every repository file `cargo package` puts into the crate: the tracked files
    of its directory outside nested packages, the workspace lock file, the
    workspace manifest whose fields it inherits, and a readme from outside."""
    package, crate = data["package"], manifest.parent.resolve()
    selected = [key for key in CARGO_SELECTION_KEYS if key in package]
    if selected:
        raise ModelError(f"{manifest}: {selected} change what the crate ships; model them")
    workspace = _cargo_workspace(root, manifest)
    files = tracked_files(root, [crate])
    nested = {path.parent for path in files if path.name == "Cargo.toml" and path.parent != crate}
    files = {path for path in files if not any(path.is_relative_to(n) for n in nested)}
    if workspace is not None:
        lock = workspace[0].parent / "Cargo.lock"
        inherits = any(isinstance(v, dict) and v.get("workspace") for v in package.values())
        extra = [lock] + ([workspace[0]] if inherits else [])
        files |= {path.resolve() for path in extra if path.is_file()}
    return files | set(_cargo_readme(crate, package, workspace))


def cargo_package_problems(root: Path, manifest: Path) -> list[str]:
    data = tomllib.loads(manifest.read_text(encoding="utf-8"))
    if "package" not in data:
        return []
    name = _relative(root, manifest)
    declared = _cargo_field(data["package"], "license", _cargo_workspace(root, manifest))
    identifiers, unlicensed = shipped_licences(root, cargo_shipped_files(root, manifest, data))
    return _unlicensed_problems(unlicensed) + declared_problems(
        name, "license", declared, identifiers
    )


# ------------------------------------------------------------------ Helm charts


def chart_licence(text: str) -> str | None:
    """The chart's `artifacthub.io/license` annotation, or None unless exactly one
    that is an SPDX identifier or an AND expression of identifiers."""
    found: list[str] = [value.strip() for _, value in CHART_LICENCE.findall(text)]
    if len(found) != 1 or not SPDX_AND_EXPRESSION.fullmatch(found[0]):
        return None
    return found[0]


def _subchart_licence(archive: Path) -> str | None:
    """The licence a packaged subchart declares in its own Chart.yaml."""
    with tarfile.open(archive, "r:gz") as bundle:
        for member in bundle.getmembers():
            parts = member.name.split("/")
            if (
                len(parts) == SUBCHART_MANIFEST_DEPTH
                and parts[-1] == "Chart.yaml"
                and member.isfile()
            ):
                if member.size > MAX_CHART_BYTES:
                    raise ModelError(f"{archive}: {member.name} is larger than {MAX_CHART_BYTES}")
                handle = bundle.extractfile(member)
                if handle is None:
                    return None
                return chart_licence(handle.read().decode("utf-8", errors="replace"))
    return None


def _subchart_problems(root: Path, archives: set[Path]) -> list[str]:
    """A subchart is a package of its own: its Chart.yaml declares its licence, and
    the repository's record of the archive (REUSE.toml) must say the same."""
    problems = []
    for archive in sorted(archives):
        relative = _relative(root, archive)
        recorded, _ = licensing.file_licence(archive, relative, reuse_annotations(root))
        declared = _subchart_licence(archive)
        if declared is None or licensing.spdx_ids(declared) != licensing.spdx_ids(recorded):
            problems.append(
                f"{relative}: the subchart declares {declared!r}, REUSE.toml records {recorded!r}"
            )
    return problems


def helm_chart_problems(root: Path, chart: Path) -> list[str]:
    """The chart's own files (its subcharts are packages of their own) against its
    `artifacthub.io/license`: one SPDX identifier, as Artifact Hub asks, or an AND
    expression when the chart's files carry several licences (ADR-2673)."""
    chart_dir = chart.parent.resolve()
    if (chart_dir / ".helmignore").exists():
        raise ModelError(f"{chart_dir}/.helmignore changes what the chart ships; model it")
    files = tracked_files(root, [chart_dir])
    subcharts = chart_dir / "charts"
    archives = {path for path in files if path.is_relative_to(subcharts) and path.suffix == ".tgz"}
    own = {path for path in files if not path.is_relative_to(subcharts)}
    identifiers, unlicensed = shipped_licences(root, own)
    declared = chart_licence(chart.read_text(encoding="utf-8"))
    problems = _unlicensed_problems(unlicensed)
    problems += declared_problems(
        _relative(root, chart), "artifacthub.io/license", declared, identifiers
    )
    return problems + _subchart_problems(root, archives)


# ------------------------------------------------------------------ npm packages


def npm_package_problems(root: Path, manifest: Path) -> list[str]:
    """A package.json with a licence declares its files'; one without may only be
    private, which npm refuses to publish."""
    data = json.loads(manifest.read_text(encoding="utf-8"))
    name = _relative(root, manifest)
    if "license" not in data:
        return (
            []
            if data.get("private") is True
            else [f"{name}: a publishable package declares no licence"]
        )
    if "files" in data:
        raise ModelError(f"{name}: `files` changes what the package ships; model it")
    package_dir = manifest.parent.resolve()
    files = tracked_files(root, [package_dir])
    nested = {p.parent for p in files if p.name == "package.json" and p.parent != package_dir}
    files = {path for path in files if not any(path.is_relative_to(n) for n in nested)}
    identifiers, unlicensed = shipped_licences(root, files)
    return _unlicensed_problems(unlicensed) + declared_problems(
        name, "license", data["license"], identifiers
    )


# ------------------------------------------------------------------ driver


def manifest_problems(root: Path, manifest: Path) -> list[str]:
    """Dispatch one manifest to the model of its package kind."""
    relative = _relative(root, manifest)
    if relative in NOT_PACKAGES and root.resolve() == REPO_ROOT.resolve():
        return []
    checks = {
        "pyproject.toml": python_package_problems,
        "Cargo.toml": cargo_package_problems,
        "Chart.yaml": helm_chart_problems,
        "package.json": npm_package_problems,
    }
    return checks[manifest.name](root, manifest)


def check(root: Path) -> tuple[list[str], int]:
    """(problems, number of manifests read) for the tree at `root`."""
    names = [_relative(root, path) for path in tracked_files(root, [root])]
    problems = root_licence_problems(root, names)
    manifests = sorted(root / name for name in names if Path(name).name in MANIFEST_NAMES)
    for manifest in manifests:
        problems += manifest_problems(root, manifest)
    return problems, len(manifests)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    args = parser.parse_args(argv)
    try:
        problems, count = check(args.root.resolve())
    except (
        ModelError,
        licensing.LicensingError,
        OSError,
        ValueError,
        KeyError,
        tarfile.TarError,
        CommandTimedOut,
        CommandValidationError,
    ) as error:
        print(f"check_licence_metadata: cannot check: {error}", file=sys.stderr)
        return 2
    for problem in problems:
        print(problem)
    if problems:
        print(f"licence metadata: {len(problems)} problem(s); see ADR-1250 and ADR-1699")
        return 1
    print(f"licence metadata: the root licence files and {count} manifests agree with the files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
