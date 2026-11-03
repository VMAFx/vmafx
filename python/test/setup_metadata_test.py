# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Regression tests for the compatibility package's setup metadata."""

from __future__ import annotations

import importlib.util
import os
import shutil
import sys
import tomllib
from pathlib import Path

import pytest

# `packaging` is a hard (non-extra) dependency of pytest itself
# (`pytest` requires `packaging>=22`), so it is available wherever this test
# runs and needs no entry in python/requirements.txt.
from packaging.requirements import Requirement
from packaging.version import InvalidVersion, Version

import vmaf

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

PYPROJECT = REPO_ROOT / "python" / "pyproject.toml"
SETUP_PY = REPO_ROOT / "python" / "setup.py"
PACKAGE_PYPROJECTS = (
    REPO_ROOT / "ai" / "pyproject.toml",
    REPO_ROOT / "dev-llm" / "pyproject.toml",
    REPO_ROOT / "mcp-server" / "vmaf-mcp" / "pyproject.toml",
    REPO_ROOT / "python" / "pyproject.toml",
    REPO_ROOT / "tools" / "ensemble-training-kit" / "pyproject.toml",
    REPO_ROOT / "tools" / "rc1-tester" / "pyproject.toml",
    REPO_ROOT / "tools" / "vmaf-roi-score" / "pyproject.toml",
    REPO_ROOT / "tools" / "vmaf-tune" / "pyproject.toml",
)

# The hash locks that install the build backend for `make cythonize` and the
# CI `setup.py` runs. The declared floor must admit the setuptools they pin;
# otherwise no locked environment could satisfy `[build-system].requires`.
BUILD_BACKEND_LOCKS = (
    REPO_ROOT / "requirements" / "locks" / "cythonize.txt",
    REPO_ROOT / "requirements" / "locks" / "package-build.txt",
)
# The newest setuptools that predates PEP 639 support, plus the version Ubuntu
# 24.04 ships in /usr/lib/python3/dist-packages. Both must be excluded by the
# declared floor, or `setup.py` aborts before it can report a version.
PRE_PEP639_SETUPTOOLS = ("76.1.0", "68.1.2")
# Releases affected by PYSEC-2025-49 (fixed in 78.1.1), PYSEC-2026-3447
# (fixed in 83.0.0) and GHSA-xmj3-9gf7-h52w (fixed in 84.1.0). The declared
# floor must exclude them as well.
VULNERABLE_SETUPTOOLS = ("78.1.0", "82.9.0", "84.0.0")

# The licence-metadata gate of ADR-1560 / ADR-1699 holds the model of what each
# package ships and reads licences with the licence tool of ADR-1503 / ADR-1513
# (one implementation, shared with the Licence Provenance job).
LICENCE_METADATA = REPO_ROOT / "scripts" / "ci" / "check_licence_metadata.py"
GIT_TIMEOUT_S = 60


def _load_licence_metadata():
    spec = importlib.util.spec_from_file_location("vmafx_licence_metadata", LICENCE_METADATA)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


metadata = _load_licence_metadata()
licensing = metadata.licensing


def _locked_version(lock: Path, name: str) -> str:
    """The exact version `lock` pins for `name`."""
    prefix = f"{name}=="
    for line in lock.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            return line[len(prefix) :].split()[0]
    raise AssertionError(f"{lock} pins no {name}")


def _setup_py_version() -> str:
    """What `setup.py --version` reports, i.e. the version setuptools ships."""
    result = run_command(
        [sys.executable, "setup.py", "--version"],
        allowed_executables=[sys.executable],
        cwd=REPO_ROOT / "python",
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        # `check=True` would raise CalledProcessError, whose message carries
        # the exit status and nothing else -- setuptools writes every
        # configuration error to stderr, which capture_output() then swallows.
        # That is how a metadata form the ambient setuptools could not parse
        # reached CI as a bare "returned non-zero exit status 1".
        raise AssertionError(
            f"`{sys.executable} setup.py --version` exited {result.returncode} "
            f"in {REPO_ROOT / 'python'}\n"
            f"--- stdout ---\n{result.stdout}\n"
            f"--- stderr ---\n{result.stderr}"
        )
    return result.stdout.strip()


def _build_requirement(name: str) -> Requirement:
    """The `[build-system].requires` entry for `name`, as a parsed Requirement."""
    build_requires = tomllib.loads(PYPROJECT.read_text(encoding="utf-8"))["build-system"][
        "requires"
    ]
    for raw in build_requires:
        requirement = Requirement(raw)
        if requirement.name.lower() == name:
            return requirement
    raise AssertionError(f"{PYPROJECT} declares no {name!r} build requirement: {build_requires}")


def test_cython_extension_include_dirs_cover_private_and_public_core_headers():
    """The direct-source ADM extension must resolve libvmaf's public headers."""
    assert {"../core/src", "../core/include"} <= set(metadata.extension_include_dirs(SETUP_PY)), (
        "the adm_dwt2 Cython extension directly includes core/src/feature/adm.c; "
        "its include_dirs must cover both private core/src and public core/include headers"
    )


def test_cython_extension_links_the_nonfinite_logger_implementation():
    """The text-included ADM source must not leave ``vmaf_log`` unresolved."""
    assert ("..", "core", "src", "log.c") in metadata.appended_extension_sources(SETUP_PY), (
        "adm.c now uses nonfinite_score.h logging helpers; the Cython extension "
        "must link core/src/log.c or its shared object imports with undefined symbol vmaf_log"
    )


def test_setup_metadata_version_matches_package_marker():
    """The release-please marker comment must not become the package version.

    `vmaf.__version__` carries the SemVer string release-please writes, which
    for a release candidate is a hyphenated prerelease such as ``1.0.0-rc.1``
    (ADR-1201). setuptools canonicalises whatever it is handed to PEP 440
    before publishing it, so the same release reaches `setup.py --version` as
    ``1.0.0rc1``. Comparing the two raw strings therefore fails on every RC
    even though nothing is wrong -- it did, on eleven build lanes at once, on
    the 1.0.0-rc.1 release PR.

    Comparing the parsed versions keeps what this test is actually for: a
    marker comment or a mangled substitution leaking into the shipped version
    still fails, either as an inequality or as `InvalidVersion`.
    """
    setup_version = _setup_py_version()

    assert Version(setup_version) == Version(vmaf.__version__), (
        f"setup.py ships {setup_version!r} but the package marker says "
        f"{vmaf.__version__!r}; these must describe the same release"
    )


def test_package_marker_is_a_valid_version():
    """A mangled release-please substitution must not parse as a version."""
    try:
        Version(vmaf.__version__)
    except InvalidVersion as exc:  # pragma: no cover - only on a broken release
        pytest.fail(f"vmaf.__version__ is not a valid version: {vmaf.__version__!r} ({exc})")

    assert "x-release-please" not in vmaf.__version__
    assert "${" not in vmaf.__version__


def test_build_backend_floor_covers_the_license_metadata_form():
    """`[build-system].requires` must exclude a setuptools that cannot read us.

    `[project].license` is a PEP 639 SPDX expression -- a bare string. Every
    setuptools before 77.0.1 rejects that shape outright, and the rejection is
    fatal: `setup.py --version` never gets as far as printing a version, so a
    stale build backend looks like a broken package rather than a stale build
    backend. `make cythonize` and the CI lanes run `setup.py` against the
    ambient interpreter, with no PEP 517 isolation to fetch a newer backend on
    their own, so the floor has to be declared here and honoured there.

    The table form (`license = { text = ... }`) needs no floor -- it is what
    old setuptools wants -- so this only asserts anything while we ship the
    SPDX expression.
    """
    project = tomllib.loads(PYPROJECT.read_text(encoding="utf-8"))["project"]
    if not isinstance(project["license"], str):
        return

    specifier = _build_requirement("setuptools").specifier
    for lock in BUILD_BACKEND_LOCKS:
        pinned = _locked_version(lock, "setuptools")
        assert specifier.contains(pinned), (
            f"{PYPROJECT} setuptools requirement {str(specifier)!r} excludes {pinned}, "
            f"the build backend {lock.relative_to(REPO_ROOT)} installs"
        )
    for version in PRE_PEP639_SETUPTOOLS:
        assert not specifier.contains(version), (
            f"{PYPROJECT} ships a PEP 639 license expression but its setuptools "
            f"requirement {str(specifier)!r} still admits {version}, which "
            "cannot parse one -- setup.py would abort with a configuration error"
        )
    for version in VULNERABLE_SETUPTOOLS:
        assert not specifier.contains(version), (
            f"{PYPROJECT} setuptools requirement {str(specifier)!r} still admits "
            f"{version}, a release with a known advisory"
        )


# ------------------------------------------------------------------ licences
#
# ADR-1250 makes each file's SPDX header the truth about its licence, and
# ADR-1513 has every Python package declare the union of the licences of the
# files it ships, with each text shipped through PEP 639 `license-files`. The
# model of what a package ships lives in scripts/ci/check_licence_metadata.py
# (ADR-1560, ADR-1699), which the Licence Provenance job runs over every
# package manifest; these tests hold the Python packages to it and plant the
# defects it must refuse.


@pytest.mark.parametrize(
    "pyproject", PACKAGE_PYPROJECTS, ids=lambda path: path.parent.relative_to(REPO_ROOT).as_posix()
)
def test_every_python_package_declares_the_licences_of_the_files_it_ships(pyproject: Path):
    """The PEP 639 expression is the union of the shipped files' SPDX identifiers
    (ADR-1250, ADR-1513), and the package ships each licence's text."""
    problems = metadata.python_package_problems(REPO_ROOT, pyproject)
    assert not problems, "\n".join(problems)


def _git_tree(root: Path, files: dict[str, str]) -> Path:
    """A git work tree at `root` holding `files` (path -> text), all staged."""
    for name, text in files.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_text(text, encoding="utf-8")
    # Without the caller's GIT_* variables: under a commit hook GIT_INDEX_FILE
    # (and GIT_DIR) would turn these commands into writes to the caller's index.
    environment = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    git = shutil.which("git")
    assert git is not None, "git is not on PATH"
    for command in (["init", "-q"], ["add", "-A"]):
        run_command(
            [git, "-C", str(root), *command],
            allowed_executables=[git],
            check=True,
            timeout_seconds=GIT_TIMEOUT_S,
            env=environment,
        )
    return root


def _hatch_package(licence: str, module_licence: str) -> dict[str, str]:
    tag = "SPDX-" + "License-Identifier:"
    return {
        "REUSE.toml": "version = 1\n",
        "pkg/pyproject.toml": (
            f"# {tag} EUPL-1.2\n"
            f'[project]\nname = "pkg"\nversion = "1"\nlicense = "{licence}"\n'
            'license-files = ["LICENSES/*"]\n'
            '[build-system]\nrequires = ["hatchling"]\nbuild-backend = "hatchling.build"\n'
            '[tool.hatch.build.targets.wheel.force-include]\n"../shared" = "pkg/shared"\n'
        ),
        "pkg/src/pkg/__init__.py": f"# {tag} {module_licence}\n",
        "shared/data.py": f"# {tag} {module_licence}\n",
    }


def _with_texts(root: Path, identifiers: list[str]) -> None:
    texts = licensing.load_manifest()["spdx_texts"]
    (root / "pkg" / "LICENSES").mkdir(parents=True, exist_ok=True)
    for identifier in identifiers:
        shutil.copyfile(
            REPO_ROOT / texts[identifier], root / "pkg" / "LICENSES" / f"{identifier}.txt"
        )


def test_the_licence_check_refuses_metadata_its_files_contradict(tmp_path: Path):
    """Planted defect: an EUPL-1.2 package that declares BSD-2-Clause-Patent."""
    _with_texts(tmp_path, ["BSD-2-Clause-Patent"])
    root = _git_tree(tmp_path, _hatch_package("BSD-2-Clause-Patent", "EUPL-1.2"))
    problems = metadata.python_package_problems(root, root / "pkg" / "pyproject.toml")
    assert any("but its files carry EUPL-1.2" in problem for problem in problems), problems
    assert any("lacks EUPL-1.2.txt" in problem for problem in problems), problems


def test_the_licence_check_counts_force_included_files_outside_the_project(tmp_path: Path):
    """A force-included file's licence joins the union; a matching declaration passes."""
    files = _hatch_package("EUPL-1.2 AND MIT", "EUPL-1.2")
    files["shared/data.py"] = "# " + "SPDX-" + "License-Identifier: MIT\n"
    _with_texts(tmp_path, ["EUPL-1.2", "MIT"])
    root = _git_tree(tmp_path, files)
    assert metadata.python_package_problems(root, root / "pkg" / "pyproject.toml") == []


def test_the_licence_check_refuses_a_shipped_file_without_a_licence(tmp_path: Path):
    files = _hatch_package("EUPL-1.2", "EUPL-1.2")
    files["pkg/src/pkg/untagged.py"] = "VALUE = 1\n"
    _with_texts(tmp_path, ["EUPL-1.2"])
    root = _git_tree(tmp_path, files)
    problems = metadata.python_package_problems(root, root / "pkg" / "pyproject.toml")
    assert problems == ["pkg/src/pkg/untagged.py states no licence (SPDX header or REUSE.toml)"]


def test_the_licence_check_refuses_a_text_that_is_not_the_repository_copy(tmp_path: Path):
    _with_texts(tmp_path, ["EUPL-1.2"])
    (tmp_path / "pkg" / "LICENSES" / "EUPL-1.2.txt").write_text(
        "not the licence\n", encoding="utf-8"
    )
    root = _git_tree(tmp_path, _hatch_package("EUPL-1.2", "EUPL-1.2"))
    problems = metadata.python_package_problems(root, root / "pkg" / "pyproject.toml")
    assert problems == ["pkg/LICENSES/EUPL-1.2.txt differs from LICENSES/EUPL-1.2.txt"]


def test_the_extension_closure_follows_cython_externs_and_includes(tmp_path: Path):
    """The compiled extension carries every repository file it includes."""
    root = _git_tree(
        tmp_path,
        {
            "pkg/core/ext.pyx": 'cdef extern from "../../src/a.c":\n    pass\n',
            "src/a.c": '#include "inc/b.h"\n#include <stdio.h>\n',
            "include/inc/b.h": '#  include "c.h"\n',
            "include/inc/c.h": "",
            "src/unused.h": "",
        },
    )
    closure = metadata.include_closure([root / "pkg/core/ext.pyx"], [root / "include"], root)
    names = {path.relative_to(root.resolve()).as_posix() for path in closure}
    assert names == {"pkg/core/ext.pyx", "src/a.c", "include/inc/b.h", "include/inc/c.h"}


# ------------------------------------------------------------ wheel contents


def _force_include_duplicates(project_dir: Path, data: dict) -> list[str]:
    """force-include sources that lie inside a wheel package directory: hatchling
    already ships them and refuses the wheel when it is asked to add them twice
    ("A second file is being added to the wheel archive at the same path")."""
    wheel = data.get("tool", {}).get("hatch", {}).get("build", {}).get("targets", {})
    wheel = wheel.get("wheel", {})
    packages = [(project_dir / package).resolve() for package in wheel.get("packages", [])]
    return [
        source
        for source in wheel.get("force-include", {})
        if any((project_dir / source).resolve().is_relative_to(p) for p in packages)
    ]


@pytest.mark.parametrize(
    "pyproject", PACKAGE_PYPROJECTS, ids=lambda path: path.parent.relative_to(REPO_ROOT).as_posix()
)
def test_no_wheel_force_includes_a_file_its_packages_already_ship(pyproject: Path):
    data = tomllib.loads(pyproject.read_text(encoding="utf-8"))
    duplicates = _force_include_duplicates(pyproject.parent, data)
    assert duplicates == [], f"{pyproject}: force-include repeats package files: {duplicates}"


def test_the_duplicate_check_refuses_a_path_inside_a_package(tmp_path: Path):
    """Planted defect: the force-include ai/ and dev-llm/ carried before."""
    data = tomllib.loads(
        '[tool.hatch.build.targets.wheel]\npackages = ["src/pkg"]\n'
        "[tool.hatch.build.targets.wheel.force-include]\n"
        '"src/pkg/data" = "pkg/data"\n"configs" = "pkg/configs"\n'
    )
    assert _force_include_duplicates(tmp_path, data) == ["src/pkg/data"]
