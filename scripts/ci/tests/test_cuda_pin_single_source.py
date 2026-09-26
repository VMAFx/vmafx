#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Hold the CUDA coordinated pin together: one release, one owner, one gate.

Two things are asserted here, and they are different things.

The *gate* (``scripts/ci/check-cuda-pin-lockstep.py``) is exercised in
disposable Git repositories: every spelling must be caught when it drifts,
``--write`` must repair exactly the derived spellings and no others, and a CUDA
release literal in a spelling the gate does not know must fail rather than pass
unnoticed.

The *coverage* assertions run against the real tree and ``renovate.json``:
every site the gate finds must be owned by something -- a Renovate custom
manager, the gate's own ``--write``, or the fail-closed exact-metadata latch.
A CUDA version site added outside those owners is a failure here, which is the
whole point: #1487 was a Renovate pull request that moved two of sixteen sites
because nothing connected the other fourteen to it.
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
from html.parser import HTMLParser
from pathlib import Path
from typing import Any, ClassVar

ROOT = Path(__file__).resolve().parents[3]
GIT = shutil.which("git") or "/usr/bin/git"
GATE = "scripts/ci/check-cuda-pin-lockstep.py"

SPEC = importlib.util.spec_from_file_location("cuda_pin_lockstep", ROOT / GATE)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["cuda_pin_lockstep"] = MODULE
SPEC.loader.exec_module(MODULE)

# Spellings Renovate cannot write, with the reason each one is the gate's job.
# Renovate substitutes the whole looked-up version into the slot it matched, so
# a slot that wants less than the whole version can only be derived.
GATE_OWNED_KINDS = {
    "apt": "cuda-toolkit-NN-N wants dashes and no patch component",
    "label": "prose inside an OCI description label, not a dependency reference",
}
MANUAL_METADATA_KINDS = {
    "apt-lock-release": "binds exact package metadata to the reviewed CUDA release",
    "apt-toolkit-version": "exact NVIDIA toolkit Debian package version",
    "apt-nvcc-version": "exact NVIDIA nvcc component Debian package version",
    "apt-cudart-version": "exact NVIDIA cudart component Debian package version",
}
# Spellings a Renovate custom manager rewrites in place.
#
# "action", "installer", "series" and "envvar" are absent because no site in
# the tree spells the release those ways any more (ADR-1300). "image" is absent
# because nvidia/cuda base images were dropped in ADR-1306 in favor of digest-pinned
# Ubuntu 26.04 with explicit apt install via scripts/ci/install-cuda-toolkit.sh.
RENOVATE_OWNED_KINDS = {"config"}


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
        if manager.get("datasourceTemplate") == "custom.nvidia-cuda-redist"
    ]
    base = [
        manager
        for manager in config["customManagers"]
        if manager["datasourceTemplate"] == "docker" and "depNameTemplate" not in manager
    ]
    assert len(cuda) == 1, "expected exactly one custom.nvidia-cuda-redist CUDA manager"
    assert len(base) == 1, "expected exactly one base-image custom manager"
    return cuda[0], base[0]


def covers(manager: dict[str, Any], path: str, line: str) -> bool:
    """True when this manager selects the file and one matchString hits the line."""
    if not any(pattern.search(path) for pattern in compiled(manager["managerFilePatterns"])):
        return False
    # Renovate matches against whole file content; the matchStrings that anchor
    # on a line start are given one here, so a line-oriented check is faithful.
    return any(re.search(to_python(match), "\n" + line) for match in manager["matchStrings"])


class HrefCollector(HTMLParser):
    """Model Renovate custom datasource's documented HTML-to-release conversion."""

    def __init__(self) -> None:
        super().__init__()
        self.hrefs: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag != "a":
            return
        href = dict(attrs).get("href")
        if href is not None:
            self.hrefs.append(href)


class CudaPinCoverage(unittest.TestCase):
    """Every CUDA site in the tree is owned by Renovate or by the gate."""

    config: ClassVar[dict[str, Any]]
    sites: ClassVar[list[Any]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.config = read_config()
        cls.sites = MODULE.find_sites(ROOT)

    def test_the_tree_has_sites_to_protect(self) -> None:
        self.assertEqual(len(self.sites), 7, "expected exactly 7 CUDA pin sites")
        kinds = {site.kind for site in self.sites}
        self.assertEqual(
            kinds,
            RENOVATE_OWNED_KINDS | set(GATE_OWNED_KINDS) | set(MANUAL_METADATA_KINDS),
        )

    def test_every_site_is_owned_by_renovate_or_by_the_gate(self) -> None:
        cuda, _ = managers(self.config)
        for site in self.sites:
            with self.subTest(site=f"{site.where} {site.kind}"):
                if site.kind in GATE_OWNED_KINDS:
                    self.assertIn(
                        site.kind,
                        MODULE.DERIVED_KINDS,
                        "a gate-owned spelling must be one --write can derive",
                    )
                    continue
                if site.kind in MANUAL_METADATA_KINDS:
                    self.assertIn(site.kind, MODULE.EXACT_METADATA_KINDS)
                    self.assertNotIn(
                        site.kind,
                        MODULE.DERIVED_KINDS,
                        "live NVIDIA package metadata must never be guessed by --write",
                    )
                    continue
                self.assertIn(site.kind, RENOVATE_OWNED_KINDS)
                owner = cuda
                self.assertTrue(
                    covers(owner, site.path, site.text),
                    f"no Renovate manager rewrites {site.where}: it would be left "
                    f"behind by a CUDA bump, exactly as in #1487",
                )

    def test_a_new_site_outside_the_manager_is_not_silently_owned(self) -> None:
        """The coverage check must actually reject something."""
        cuda, _ = managers(self.config)
        # The manager only selects build-config.env; nothing else matches.
        self.assertFalse(covers(cuda, "tools/newlane/setup.sh", "  cuda: '13.4.1'"))
        self.assertFalse(covers(cuda, "build-config.env", 'CUDA_SERIES="13.4"'))
        # The OCI docker-tag form is no longer a recognised matchString.
        self.assertFalse(
            covers(
                cuda,
                "build-config.env",
                'CUDA_BUILDER="nvidia/cuda:13.4.2-devel-ubuntu26.04"',
            )
        )

    def test_cuda_manager_uses_redist_datasource_not_docker(self) -> None:
        """CUDA_VERSION must be resolved against the NVIDIA redist HTML index (ADR-1306).

        The old manager resolved nvidia/cuda Docker tags and was gated on OCI image
        publication; the new one resolves custom.nvidia-cuda-redist, which is backed
        by the NVIDIA apt/redist HTML index that lists redistrib_X.Y.Z.json files.
        CUDA 13.4.2 exists in the redist index but has no nvidia/cuda OCI image.
        """
        cuda, _ = managers(self.config)
        self.assertEqual(cuda["datasourceTemplate"], "custom.nvidia-cuda-redist")
        self.assertEqual(cuda["depNameTemplate"], "nvidia-cuda-redist")

    def test_no_docker_cuda_manager_exists(self) -> None:
        """nvidia/cuda Docker-tag manager must be absent (ADR-1306).

        Re-introducing it would re-gate CUDA bumps on OCI image publication.
        """
        docker_cuda = [
            m
            for m in self.config["customManagers"]
            if m.get("depNameTemplate") == "nvidia/cuda" and m.get("datasourceTemplate") == "docker"
        ]
        self.assertEqual(
            docker_cuda,
            [],
            "Found a nvidia/cuda Docker custom manager; ADR-1306 forbids re-introduction",
        )
        stale_rules = [
            rule
            for rule in self.config["packageRules"]
            if "nvidia/cuda" in rule.get("matchPackageNames", [])
            or rule.get("groupName") == "CUDA release (coordinated pin)"
        ]
        self.assertEqual(stale_rules, [], "the obsolete Docker CUDA group must stay deleted")

    def test_cuda_manager_extractversion_accepts_redist_links_only(self) -> None:
        """extractVersionTemplate must accept redistrib_X.Y.Z.json and reject everything else.

        The NVIDIA redist HTML index returns all <a href> values; the manager uses
        extractVersionTemplate to filter to redistrib_X.Y.Z.json entries only,
        leaving a clean semver stream for comparison against CUDA_VERSION.
        Unrelated directory links (cuda_nvcc/, ../), bare versions, or docker tags
        must not produce a version.
        """
        cuda, _ = managers(self.config)
        extract_tpl = cuda.get("extractVersionTemplate")
        self.assertIsNotNone(
            extract_tpl,
            "extractVersionTemplate is required on the CUDA manager",
        )
        pattern = re.compile(to_python(extract_tpl))  # type: ignore[arg-type]
        # Positive: redistrib_X.Y.Z.json
        for good in (
            "redistrib_13.4.2.json",
            "redistrib_13.3.1.json",
            "redistrib_13.0.0.json",
        ):
            with self.subTest(href=good):
                m = pattern.search(good)
                self.assertIsNotNone(m, f"{good!r} must match extractVersionTemplate")
                assert m is not None
                self.assertRegex(m.group("version"), r"^\d+\.\d+\.\d+$")
        # Negative: directory links, bare text, docker tags, other filenames
        for bad in (
            "cuda_nvcc/",
            "../",
            "redistrib_v2_13.4.2.json",
            "13.4.2",
            "redistrib_13.4.json",  # only 2-part — must not match
            "nvidia/cuda:13.4.2-devel-ubuntu26.04",
            "other_file.json",
        ):
            with self.subTest(href=bad):
                self.assertIsNone(
                    pattern.search(bad),
                    f"{bad!r} must not match extractVersionTemplate",
                )

    def test_representative_html_selects_13_4_2_and_rejects_noise(self) -> None:
        """Exercise the HTML href conversion and configured extractVersion together."""
        document = """
        <html><body>
          <a href='..'>..</a>
          <a href='cuda_nvcc/'>cuda_nvcc/</a>
          <a href='redistrib_13.3.1.json'>redistrib_13.3.1.json</a>
          <a href='redistrib_13.4.1.json'>redistrib_13.4.1.json</a>
          <a href='redistrib_13.4.2.json'>redistrib_13.4.2.json</a>
          <a href='redistrib_13.5.0.json.asc'>signature</a>
          <a href='redistrib_v2_13.5.0.json'>schema v2</a>
        </body></html>
        """
        parser = HrefCollector()
        parser.feed(document)
        cuda, _ = managers(self.config)
        pattern = re.compile(to_python(cuda["extractVersionTemplate"]))
        versions = [
            match.group("version")
            for raw_version in parser.hrefs
            if (match := pattern.fullmatch(raw_version)) is not None
        ]
        self.assertEqual(versions, ["13.3.1", "13.4.1", "13.4.2"])
        latest = max(versions, key=lambda version: tuple(map(int, version.split("."))))
        self.assertEqual(latest, "13.4.2")

    def test_custom_datasource_uses_nvidia_redist_html_index(self) -> None:
        """customDatasources must define nvidia-cuda-redist backed by the NVIDIA official index.

        The official NVIDIA redist HTML index at
        https://developer.download.nvidia.com/compute/cuda/redist/
        lists redistrib_X.Y.Z.json files. It listed redistrib_13.4.2.json before
        any nvidia/cuda:13.4.2-* OCI image existed on Docker Hub.
        """
        ds = self.config.get("customDatasources", {})
        self.assertIn("nvidia-cuda-redist", ds, "customDatasources must define nvidia-cuda-redist")
        entry = ds["nvidia-cuda-redist"]
        self.assertEqual(entry["format"], "html")
        url = entry["defaultRegistryUrlTemplate"]
        self.assertIn("developer.download.nvidia.com", url)
        self.assertIn("cuda/redist", url)
        self.assertNotIn(
            "transformTemplates",
            entry,
            "Renovate already converts HTML hrefs to releases; filtering belongs to extractVersion",
        )

    def test_redist_rule_handles_missing_timestamps_without_automerge(self) -> None:
        """The HTML datasource has no timestamps, so the global age gate needs an exception."""
        rules = [
            rule
            for rule in self.config["packageRules"]
            if rule.get("matchDatasources") == ["custom.nvidia-cuda-redist"]
        ]
        self.assertEqual(len(rules), 1)
        rule = rules[0]
        self.assertEqual(rule["matchPackageNames"], ["nvidia-cuda-redist"])
        self.assertEqual(rule["minimumReleaseAgeBehaviour"], "timestamp-optional")
        self.assertIs(rule["automerge"], False)
        self.assertNotIn("groupName", rule, "the deleted CUDA group must not be recreated")

    def test_renovate_moves_only_release_and_leaves_exact_metadata_latch(self) -> None:
        """A bot bump must stop until a human refreshes NVIDIA package metadata."""
        cuda, _ = managers(self.config)
        config_lines = (ROOT / "build-config.env").read_text(encoding="utf-8").splitlines()
        release_line = next(line for line in config_lines if line.startswith("CUDA_VERSION="))
        self.assertTrue(covers(cuda, "build-config.env", release_line))
        for name in (
            "CUDA_APT_LOCK_RELEASE",
            "CUDA_APT_TOOLKIT_VERSION",
            "CUDA_APT_NVCC_VERSION",
            "CUDA_APT_CUDART_VERSION",
        ):
            line = next(line for line in config_lines if line.startswith(f"{name}="))
            with self.subTest(name=name):
                self.assertFalse(covers(cuda, "build-config.env", line))

    def test_installer_contract_suite_is_wired_to_required_precommit_ci(self) -> None:
        precommit = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        match = re.search(
            r"(?ms)^\s+- id: test-install-cuda-toolkit\n(?P<body>.*?)(?=^\s+- id:|\Z)",
            precommit,
        )
        self.assertIsNotNone(match, "installer regression suite needs its own pre-commit hook")
        assert match is not None
        body = match.group("body")
        self.assertIn("test_install_cuda_toolkit.py", body)
        self.assertIn(r"install-cuda-toolkit\.sh", body)
        workflow = (ROOT / ".github/workflows/lint-and-format.yml").read_text(encoding="utf-8")
        self.assertIn("pre-commit run --show-diff-on-failure --color=always --all-files", workflow)

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
        self.assertIn("7 sites", result.stdout)

    def test_each_spelling_is_caught_when_it_drifts(self) -> None:
        cases = (
            (
                "build-config.env",
                'CUDA_APT_LOCK_RELEASE="13.4.2"',
                'CUDA_APT_LOCK_RELEASE="13.4.1"',
                "apt-lock-release",
            ),
            (
                "build-config.env",
                'CUDA_APT_TOOLKIT_VERSION="13.4.2-1"',
                'CUDA_APT_TOOLKIT_VERSION="13.4.1-1"',
                "apt-toolkit-version",
            ),
            (
                "build-config.env",
                'CUDA_APT_NVCC_VERSION="13.4.92-1"',
                'CUDA_APT_NVCC_VERSION="13.5.1-1"',
                "apt-nvcc-version",
            ),
            (
                "build-config.env",
                'CUDA_APT_CUDART_VERSION="13.4.92-1"',
                'CUDA_APT_CUDART_VERSION="12.9.1-1"',
                "apt-cudart-version",
            ),
            (
                "build-config.env",
                'CUDA_APT_PACKAGE="cuda-toolkit-13-4"',
                'CUDA_APT_PACKAGE="cuda-toolkit-13-5"',
                "apt",
            ),
            (
                "docker/Dockerfile.production-gpu",
                "production CUDA 13.4.2 runtime",
                "production CUDA 13.5.0 runtime",
                "label",
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

    def test_renovate_release_bump_fails_until_exact_metadata_is_refreshed(self) -> None:
        self.edit("build-config.env", 'CUDA_VERSION="13.4.2"', 'CUDA_VERSION="13.5.0"')
        result = self.gate()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("apt-lock-release pin reads", result.stderr)
        self.assertIn("apt-toolkit-version pin reads", result.stderr)
        self.assertIn("CUDA_APT_LOCK_RELEASE", result.stderr)

    def test_reintroduced_nvidia_cuda_image_is_unrecognised_and_fails(self) -> None:
        """An nvidia/cuda image reference is no longer a valid pin shape (ADR-1306)."""
        self.edit(
            "docker/Dockerfile.node",
            'ARG CUDA_RUNTIME="ubuntu:26.04@sha256:da6fc2be547864451aa253836dd926da33623312df4a9a243e35dc877c378a78"',
            'ARG CUDA_RUNTIME="nvidia/cuda:13.4.2-runtime-ubuntu26.04@sha256:1725dba28b39fd0c3c35665c98284b603bef7b30e8f7990a98d4c3cbb905016a"',
        )
        result = self.gate()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("a spelling this gate does not know", result.stderr)
        self.assertIn("docker/Dockerfile.node", result.stderr)

    def test_a_site_in_an_unknown_spelling_fails(self) -> None:
        """An eighth copy in a shape nobody taught the gate is still drift."""
        workflow = self.repo / ".github/workflows/newlane.yml"
        workflow.write_text(
            "name: New lane\njobs:\n  build:\n    env:\n"
            "      CUDA_TOOLKIT_RELEASE: 13.4.2  # cuda\n",
            encoding="utf-8",
        )
        self.git("add", "--", ".github/workflows/newlane.yml")
        result = self.gate()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("a spelling this gate does not know", result.stderr)
        self.assertIn("newlane.yml", result.stderr)

    def test_write_repairs_the_derived_spellings_only(self) -> None:
        self.edit(
            "build-config.env",
            'CUDA_APT_PACKAGE="cuda-toolkit-13-4"',
            'CUDA_APT_PACKAGE="cuda-toolkit-12-1"',
        )
        self.edit(
            "docker/Dockerfile.production-gpu",
            "production CUDA 13.4.2 runtime",
            "production CUDA 12.1.0 runtime",
        )
        result = self.gate("--write")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # The two derived spellings are repaired ...
        config = (self.repo / "build-config.env").read_text(encoding="utf-8")
        self.assertIn('CUDA_APT_PACKAGE="cuda-toolkit-13-4"', config)
        self.assertIn('CUDA_APT_NVCC_VERSION="13.4.92-1"', config)
        self.assertIn('CUDA_APT_CUDART_VERSION="13.4.92-1"', config)
        self.assertIn(
            "CUDA 13.4.2 runtime",
            (self.repo / "docker/Dockerfile.production-gpu").read_text(),
        )

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
            config.read_text(encoding="utf-8").replace('CUDA_VERSION="13.4.2"', ""),
            encoding="utf-8",
        )
        self.git("add", "--", "build-config.env")
        self.assertEqual(self.gate().returncode, 2)


if __name__ == "__main__":
    unittest.main()
