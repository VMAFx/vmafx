#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hold the CUDA coordinated pin together: one release, one group, one gate.

Two things are asserted here, and they are different things.

The *gate* (``scripts/ci/check-cuda-pin-lockstep.py``) is exercised in
disposable Git repositories: every spelling must be caught when it drifts,
``--write`` must repair exactly the derived spellings and no others, and a CUDA
release literal in a spelling the gate does not know must fail rather than pass
unnoticed.

The *coverage* assertions run against the real tree and ``renovate.json``:
every site the gate finds must be owned by something -- a Renovate custom
manager that will rewrite it, or the gate's own ``--write``. A CUDA version
site added outside both is a failure here, which is the whole point: #1487 was
a Renovate pull request that moved two of sixteen sites because nothing
connected the other fourteen to it.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, ClassVar

ROOT = Path(__file__).resolve().parents[3]
GIT = shutil.which("git") or "/usr/bin/git"
GATE = "scripts/ci/check-cuda-pin-lockstep.py"
# The apt spelling also appears in the comment above the RUN, and a comment is
# prose, not a pin. Anchor fixtures on the install line itself.
APT_INSTALL = "--no-install-recommends \\\n    cuda-toolkit-13-3"

SPEC = importlib.util.spec_from_file_location("cuda_pin_lockstep", ROOT / GATE)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["cuda_pin_lockstep"] = MODULE
SPEC.loader.exec_module(MODULE)

# Spellings Renovate cannot write, with the reason each one is the gate's job.
# Renovate substitutes the whole looked-up version into the slot it matched, so
# a slot that wants less than the whole version can only be derived.
GATE_OWNED_KINDS = {
    "series": "$cudaMajorMinor wants major.minor, Renovate would write major.minor.patch",
    "apt": "cuda-toolkit-NN-N wants dashes and no patch component",
    "label": "prose inside an OCI description label, not a dependency reference",
}
# Spellings a Renovate custom manager rewrites in place.
RENOVATE_OWNED_KINDS = {"config", "action", "installer", "image"}


def read_config() -> dict[str, Any]:
    data = json.loads((ROOT / "renovate.json").read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise TypeError("renovate.json must contain a JSON object")
    return data


JS_GROUP = re.compile(r"\(\?<([A-Za-z_][A-Za-z0-9_]*)>")


def to_python(pattern: str) -> str:
    """Renovate's regexes use JavaScript named groups; Python spells them (?P<x>)."""
    return JS_GROUP.sub(r"(?P<\1>", pattern)


def compiled(patterns: list[str]) -> list[re.Pattern[str]]:
    """Renovate's /regex/ file-pattern form, compiled for local matching."""
    out = []
    for pattern in patterns:
        assert pattern.startswith("/") and pattern.endswith("/"), pattern
        out.append(re.compile(to_python(pattern[1:-1])))
    return out


def managers(config: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    """The CUDA manager and the base-image manager, located by their shape."""
    cuda = [
        manager
        for manager in config["customManagers"]
        if manager.get("depNameTemplate") == "nvidia/cuda"
    ]
    base = [
        manager
        for manager in config["customManagers"]
        if manager["datasourceTemplate"] == "docker" and "depNameTemplate" not in manager
    ]
    assert len(cuda) == 1, "expected exactly one nvidia/cuda custom manager"
    assert len(base) == 1, "expected exactly one base-image custom manager"
    return cuda[0], base[0]


def covers(manager: dict[str, Any], path: str, line: str) -> bool:
    """True when this manager selects the file and one matchString hits the line."""
    if not any(pattern.search(path) for pattern in compiled(manager["managerFilePatterns"])):
        return False
    # Renovate matches against whole file content; the matchStrings that anchor
    # on a line start are given one here, so a line-oriented check is faithful.
    return any(re.search(to_python(match), "\n" + line) for match in manager["matchStrings"])


class CudaPinCoverage(unittest.TestCase):
    """Every CUDA site in the tree is owned by Renovate or by the gate."""

    config: ClassVar[dict[str, Any]]
    sites: ClassVar[list[Any]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.config = read_config()
        cls.sites = MODULE.find_sites(ROOT)

    def test_the_tree_has_sites_to_protect(self) -> None:
        self.assertGreaterEqual(len(self.sites), 10, "the site scanner found almost nothing")
        kinds = {site.kind for site in self.sites}
        self.assertEqual(kinds, RENOVATE_OWNED_KINDS | set(GATE_OWNED_KINDS))

    def test_every_site_is_owned_by_renovate_or_by_the_gate(self) -> None:
        cuda, base = managers(self.config)
        for site in self.sites:
            with self.subTest(site=f"{site.where} {site.kind}"):
                if site.kind in GATE_OWNED_KINDS:
                    self.assertIn(
                        site.kind,
                        MODULE.DERIVED_KINDS,
                        "a gate-owned spelling must be one --write can derive",
                    )
                    continue
                self.assertIn(site.kind, RENOVATE_OWNED_KINDS)
                owner = base if site.kind == "image" else cuda
                self.assertTrue(
                    covers(owner, site.path, site.text),
                    f"no Renovate manager rewrites {site.where}: it would be left "
                    f"behind by a CUDA bump, exactly as in #1487",
                )

    def test_a_new_site_outside_the_manager_is_not_silently_owned(self) -> None:
        """The coverage check must actually reject something."""
        cuda, _ = managers(self.config)
        self.assertFalse(covers(cuda, "tools/newlane/setup.sh", "  cuda: '13.3.1'"))
        self.assertFalse(covers(cuda, "build-config.env", 'CUDA_SERIES="13.3"'))

    def test_the_group_rule_names_every_renovate_owned_site(self) -> None:
        rules = [
            rule
            for rule in self.config["packageRules"]
            if rule.get("groupName") == "CUDA release (coordinated pin)"
        ]
        self.assertEqual(len(rules), 1, "the CUDA group rule is missing or duplicated")
        rule = rules[0]
        cuda, base = managers(self.config)
        self.assertEqual(rule["matchPackageNames"], [cuda["depNameTemplate"]])
        self.assertIs(rule["automerge"], False, "a coordinated pin is not auto-mergeable")
        # The image pins resolve under the same package name, which is what puts
        # both managers' deps into one branch.
        self.assertIn("nvidia/cuda", (ROOT / "build-config.env").read_text(encoding="utf-8"))
        self.assertEqual(base["datasourceTemplate"], "docker")
        # Digest refreshes stay in the Docker digests batch: no coordination needed.
        self.assertNotIn("digest", rule["matchUpdateTypes"])
        self.assertNotIn("pin", rule["matchUpdateTypes"])

    def test_the_manager_extracts_a_version_nvidia_actually_publishes(self) -> None:
        """A bare `13.3.1` matches no nvidia/cuda tag; extractVersion is load-bearing."""
        cuda, _ = managers(self.config)
        pattern = re.compile(to_python(cuda["extractVersionTemplate"]))
        match = pattern.search("13.4.0-devel-ubuntu26.04")
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(match.group("version"), "13.4.0")
        self.assertIsNone(pattern.search("13.4.0-runtime-ubuntu26.04"))
        self.assertIsNone(pattern.search("latest"))

    def test_every_matchstring_hits_a_real_line_in_the_tree(self) -> None:
        """A manager whose regex matches nothing is wiring that does nothing."""
        cuda, _ = managers(self.config)
        patterns = compiled(cuda["managerFilePatterns"])
        selected = [
            path
            for path in subprocess.check_output(  # noqa: S603 -- fixed read-only Git command
                [GIT, "-C", str(ROOT), "ls-files"], text=True
            ).splitlines()
            if any(pattern.search(path) for pattern in patterns)
        ]
        self.assertTrue(selected)
        blob = "\n".join((ROOT / path).read_text(encoding="utf-8") for path in selected)
        for match in cuda["matchStrings"]:
            with self.subTest(matchString=match):
                found = re.search(to_python(match), blob)
                self.assertIsNotNone(found, "matchString selects nothing in the tree")
                assert found is not None
                self.assertRegex(found.group("currentValue"), r"^\d+\.\d+\.\d+$")


class CudaPinGate(unittest.TestCase):
    """Run the real gate in disposable repositories, one spelling at a time."""

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        for name in (GATE, "build-config.env", "dev/Containerfile"):
            target = self.repo / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        for name in (
            "Dockerfile",
            "docker/Dockerfile.node",
            "docker/Dockerfile.production-gpu",
            ".github/workflows/build.yml",
            ".github/workflows/libvmaf-build-matrix.yml",
        ):
            target = self.repo / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        self.git("init", "--quiet")
        self.git("add", "-A")

    def git(self, *args: str) -> None:
        subprocess.run(  # noqa: S603 -- fixed Git commands in a disposable fixture
            [GIT, *args], cwd=self.repo, env=self.env, check=True, capture_output=True
        )

    def gate(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- the gate under test, in its fixture
            [sys.executable, GATE, *args],
            cwd=self.repo,
            env=self.env,
            capture_output=True,
            text=True,
        )

    def edit(self, path: str, old: str, new: str) -> None:
        file = self.repo / path
        text = file.read_text(encoding="utf-8")
        self.assertIn(old, text, f"{path} no longer contains {old!r}")
        file.write_text(text.replace(old, new, 1), encoding="utf-8")
        self.git("add", "--", path)

    def test_the_fixture_starts_in_lockstep(self) -> None:
        result = self.gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("16 sites", result.stdout)

    def test_each_spelling_is_caught_when_it_drifts(self) -> None:
        cases = (
            (".github/workflows/build.yml", "cuda: '13.3.1'", "cuda: '13.4.0'", "action"),
            (
                ".github/workflows/build.yml",
                "$cudaVersion = '13.3.1'",
                "$cudaVersion = '13.4.0'",
                "installer",
            ),
            (
                ".github/workflows/build.yml",
                "$cudaMajorMinor = '13.3'",
                "$cudaMajorMinor = '13.4'",
                "series",
            ),
            (
                "dev/Containerfile",
                APT_INSTALL,
                APT_INSTALL.replace("13-3", "13-4"),
                "apt",
            ),
            (
                "docker/Dockerfile.production-gpu",
                "production CUDA 13.3.1 runtime",
                "production CUDA 13.4.0 runtime",
                "label",
            ),
            (
                "docker/Dockerfile.node",
                "nvidia/cuda:13.3.1-runtime",
                "nvidia/cuda:13.4.0-runtime",
                "image",
            ),
        )
        for path, old, new, kind in cases:
            with self.subTest(kind=kind):
                self.setUp()
                self.edit(path, old, new)
                result = self.gate()
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(f"{kind} pin reads", result.stderr)
                self.assertIn(path, result.stderr)

    def test_a_site_in_an_unknown_spelling_fails(self) -> None:
        """A seventeenth copy in a shape nobody taught the gate is still drift."""
        workflow = self.repo / ".github/workflows/newlane.yml"
        workflow.write_text(
            "name: New lane\njobs:\n  build:\n    env:\n"
            "      CUDA_TOOLKIT_RELEASE: 13.3.1  # cuda\n",
            encoding="utf-8",
        )
        self.git("add", "--", ".github/workflows/newlane.yml")
        result = self.gate()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("a spelling this gate does not know", result.stderr)
        self.assertIn("newlane.yml", result.stderr)

    def test_write_repairs_the_derived_spellings_only(self) -> None:
        self.edit(
            ".github/workflows/build.yml",
            "$cudaMajorMinor = '13.3'",
            "$cudaMajorMinor = '12.1'",
        )
        self.edit("dev/Containerfile", APT_INSTALL, APT_INSTALL.replace("13-3", "12-1"))
        self.edit(
            "docker/Dockerfile.production-gpu",
            "production CUDA 13.3.1 runtime",
            "production CUDA 12.1.0 runtime",
        )
        self.edit("docker/Dockerfile.node", "nvidia/cuda:13.3.1-runtime", "nvidia/cuda:12.1.0-x")
        result = self.gate("--write")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        # The three derived spellings are repaired ...
        self.assertIn("13.3", (self.repo / ".github/workflows/build.yml").read_text())
        self.assertIn("cuda-toolkit-13-3", (self.repo / "dev/Containerfile").read_text())
        self.assertIn(
            "CUDA 13.3.1 runtime",
            (self.repo / "docker/Dockerfile.production-gpu").read_text(),
        )
        # ... and the image pin is not, because its digest cannot be derived.
        self.assertIn("12.1.0", (self.repo / "docker/Dockerfile.node").read_text())
        self.assertIn("image pin reads '12.1.0'", result.stderr)

    def test_a_comment_naming_an_old_release_is_not_a_pin(self) -> None:
        """Prose about CUDA 12.4 in a comment must not fail the gate."""
        path = self.repo / "dev/Containerfile"
        path.write_text(
            path.read_text(encoding="utf-8") + "\n# CUDA 12.4 broke rsqrt; see ADR-0603.\n",
            encoding="utf-8",
        )
        self.git("add", "--", "dev/Containerfile")
        result = self.gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_missing_authority_is_a_configuration_error_not_drift(self) -> None:
        config = self.repo / "build-config.env"
        config.write_text(
            config.read_text(encoding="utf-8").replace('CUDA_VERSION="13.3.1"', ""),
            encoding="utf-8",
        )
        self.git("add", "--", "build-config.env")
        self.assertEqual(self.gate().returncode, 2)


if __name__ == "__main__":
    unittest.main()
