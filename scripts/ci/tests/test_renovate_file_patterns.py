#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Require positive file-selection fixtures for Renovate custom managers.

The repository uses anchored, flag-free regexes with the /regex/ delimiter
contract verified against Renovate 44.56.3's installed matcher. These patterns
use syntax shared by Python re and RE2; no Renovate installation is needed for
the local/CI guard. See docs/research/renovate-file-pattern-delimiters.md.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any, ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.renovate_regex import to_python
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
GIT = shutil.which("git") or "/usr/bin/git"
CONFIG = "build-config.env"
CONFIG_LINE = re.compile(r'^([A-Z][A-Z0-9_]*)="([^"]*)"', re.MULTILINE)
DOCKERFILE = re.compile(r"^(?:Dockerfile[^/]*|docker/Dockerfile[^/]*|dev/Containerfile[^/]*)$")
CUDA_COMPAT = "docker/dev/ubuntu-26.04-cuda.Dockerfile"
# The controller store tests' PostgreSQL images (#2656): Go string constants
# of the form postgres:<tag>@sha256:<digest>, read by a custom manager.
STORETEST = "cmd/vmafx-controller/store/storetest/storetest.go"
IMAGE_CONSTANT = re.compile(
    r'^const (\w+) = "postgres:([^"@]+)@(sha256:[a-f0-9]{64})"$', re.MULTILINE
)


def image_keys(config_text: str) -> set[str]:
    """The build-config.env keys whose value is an image reference, by the shape
    scripts/ci/check-base-image-single-source.sh uses: a registry path or a tag
    separator, and not a URL."""
    return {
        key
        for key, value in CONFIG_LINE.findall(config_text)
        if not value.startswith("http") and ("/" in value or ":" in value)
    }


def base_files(files: list[str], read: Callable[[str], str]) -> set[str]:
    """build-config.env and every Dockerfile in the single-source gate's scope that
    mirrors one of its image keys as an ARG default. Derived from the tree, so a
    new mirror the custom manager does not select fails the tests below instead
    of being left to the built-in manager (docker/Dockerfile.tester kept ROCm
    10.0.0 in the 10.1.0 bump, #2170)."""
    keys = image_keys(read(CONFIG))
    arg = re.compile(r"^\s*ARG\s+(" + "|".join(sorted(keys)) + r")=", re.MULTILINE)
    scope = [
        path
        for path in files
        if (DOCKERFILE.match(path) and not path.startswith("docker/dev/")) or path == CUDA_COMPAT
    ]
    return {CONFIG} | {path for path in scope if arg.search(read(path))}


class RenovateFilePatterns(unittest.TestCase):
    config: ClassVar[dict[str, Any]]
    files: ClassVar[list[str]]
    base: ClassVar[set[str]]

    @classmethod
    def setUpClass(cls) -> None:
        cls.config = json.loads((ROOT / "renovate.json").read_text(encoding="utf-8"))
        environment = {
            key: value for key, value in os.environ.items() if not key.startswith("GIT_")
        }
        result = run_command(
            [GIT, "-C", str(ROOT), "ls-files"],
            allowed_executables=(GIT,),
            env=environment,
            capture_output=True,
            text=True,
            check=True,
        )
        assert isinstance(result.stdout, str)
        cls.files = result.stdout.splitlines()
        cls.base = base_files(cls.files, lambda path: (ROOT / path).read_text(encoding="utf-8"))

    def patterns(self, manager: dict[str, Any]) -> list[re.Pattern[str]]:
        patterns = manager["managerFilePatterns"]
        self.assertTrue(patterns)
        compiled = []
        for pattern in patterns:
            self.assertTrue(pattern.startswith("/") and pattern.endswith("/"), pattern)
            compiled.append(re.compile(pattern[1:-1]))
        return compiled

    def test_every_custom_pattern_selects_a_tracked_input(self) -> None:
        for manager in self.config["customManagers"]:
            name = manager.get("depNameTemplate", manager.get("description", "custom"))
            for pattern in self.patterns(manager):
                with self.subTest(manager=name, pattern=pattern.pattern):
                    selected = [path for path in self.files if pattern.search(path)]
                    self.assertTrue(selected, "Pattern selects no tracked files")
                    for path in selected:
                        self.assertIsNone(pattern.search(f"archive/{path}"))
                        self.assertIsNone(pattern.search(f"{path}.bak"))

    def test_base_image_manager_selects_config_and_all_mirrors(self) -> None:
        managers = [
            manager
            for manager in self.config["customManagers"]
            if manager["datasourceTemplate"] == "docker" and "depNameTemplate" not in manager
        ]
        self.assertEqual(len(managers), 1)
        patterns = self.patterns(managers[0])
        selected = {
            path for path in self.files if any(pattern.search(path) for pattern in patterns)
        }
        self.assertEqual(selected, self.base)

    def test_builtin_docker_exclusions_are_covered_by_the_custom_manager(self) -> None:
        rules = [
            rule
            for rule in self.config["packageRules"]
            if rule.get("matchManagers") == ["dockerfile"] and rule.get("enabled") is False
        ]
        self.assertEqual(len(rules), 1)
        self.assertEqual(set(rules[0]["matchFileNames"]), self.base - {CONFIG})

    def test_base_files_follow_the_config(self) -> None:
        # The derivation must see the known mirrors, and a Dockerfile that stops
        # mirroring a config key must drop out of it.
        self.assertIn("docker/Dockerfile.tester", self.base)
        self.assertIn("dev/Containerfile", self.base)
        fake = {
            CONFIG: 'ROCM_BUILDER="rocm/dev-ubuntu-26.04:1.0-full@sha256:00"\nROCM_VERSION="1.0"\n',
            "docker/Dockerfile.a": 'ARG ROCM_BUILDER="x"\n',
            "docker/Dockerfile.b": 'ARG ROCM_VERSION="1.0"\n',
            "docker/dev/alpine.Dockerfile": 'ARG ROCM_BUILDER="x"\n',
        }
        self.assertEqual(base_files(list(fake), fake.__getitem__), {CONFIG, "docker/Dockerfile.a"})

    def test_rocm_review_rule_matches_the_pinned_image(self) -> None:
        # The manual-review rule (ADR-1225) named rocm/dev-ubuntu-24.04 after the
        # pin moved to the 26.04 image, so the 10.1.0 bump (#2170) arrived without
        # its rocm / manual-review labels.
        config = (ROOT / CONFIG).read_text(encoding="utf-8")
        pinned = {
            value.split(":", 1)[0]
            for key, value in CONFIG_LINE.findall(config)
            if key in {"ROCM_BUILDER", "ROCM_RUNTIME"}
        }
        self.assertEqual(len(pinned), 1)
        rules = [
            rule
            for rule in self.config["packageRules"]
            if rule.get("groupName") == "ROCm (hip runtime)"
        ]
        self.assertEqual(len(rules), 1)
        self.assertLessEqual(pinned, set(rules[0]["matchPackageNames"]))
        self.assertIn("manual-review", rules[0]["labels"])

    def test_glibc_floor_runner_pin_is_frozen(self) -> None:
        # ADR-1354: Renovate must not move the native-bundle verify job off the
        # oldest image that can load it, and the rule must still point at the
        # job it protects.
        rules = [
            rule
            for rule in self.config["packageRules"]
            if rule.get("matchDatasources") == ["github-runners"] and rule.get("enabled") is False
        ]
        self.assertEqual(len(rules), 1)
        rule = rules[0]
        self.assertEqual(rule["matchDepNames"], ["ubuntu"])
        self.assertEqual(rule["matchCurrentValue"], "24.04")
        self.assertEqual(rule["matchFileNames"], [".github/workflows/supply-chain.yml"])
        workflow = (ROOT / rule["matchFileNames"][0]).read_text(encoding="utf-8")
        match = re.search(r"(?ms)^  verify-native-artifacts:\n(.*?)(?=^  [\w-]+:\n|\Z)", workflow)
        self.assertIsNotNone(match, "supply-chain.yml has no verify-native-artifacts job")
        self.assertRegex(match.group(1) if match else "", r"(?m)^    runs-on: ubuntu-24\.04$")

    def storetest_manager(self) -> dict[str, Any]:
        managers = [
            manager
            for manager in self.config["customManagers"]
            if any(pattern.search(STORETEST) for pattern in self.patterns(manager))
        ]
        self.assertEqual(len(managers), 1)
        manager: dict[str, Any] = managers[0]
        return manager

    def test_storetest_postgres_images_are_tracked(self) -> None:
        # No built-in manager reads a Go string constant, so without the custom
        # manager neither image digest is ever updated. Every image constant of
        # the file must be a match, with its tag and digest.
        manager = self.storetest_manager()
        self.assertEqual(manager["datasourceTemplate"], "docker")
        self.assertEqual(manager["depNameTemplate"], "postgres")
        text = (ROOT / STORETEST).read_text(encoding="utf-8")
        constants = {name: (tag, digest) for name, tag, digest in IMAGE_CONSTANT.findall(text)}
        self.assertEqual(set(constants), {"Image", "OldestImage"})
        patterns = [re.compile(to_python(match)) for match in manager["matchStrings"]]
        found = {
            (match["currentValue"], match["currentDigest"])
            for pattern in patterns
            for match in pattern.finditer(text)
        }
        self.assertEqual(found, set(constants.values()))
        # An image without a digest is not a match (testutil/pg refuses it).
        undigested = 'const Image = "postgres:18.6-alpine"\n'
        self.assertFalse(any(pattern.search(undigested) for pattern in patterns))

    def test_oldest_postgres_image_stays_on_its_major(self) -> None:
        # OldestImage is the oldest release external servers may run: digest,
        # minor and patch updates only. Image keeps its major updates.
        rules = [
            rule
            for rule in self.config["packageRules"]
            if rule.get("matchFileNames") == [STORETEST]
        ]
        self.assertEqual(len(rules), 1)
        rule = rules[0]
        self.assertEqual(rule["matchDepNames"], ["postgres"])
        text = (ROOT / STORETEST).read_text(encoding="utf-8")
        tags = {name: tag for name, tag, _ in IMAGE_CONSTANT.findall(text)}
        current = re.compile(to_python(rule["matchCurrentValue"][1:-1]))
        allowed = re.compile(to_python(rule["allowedVersions"][1:-1]))
        self.assertTrue(current.search(tags["OldestImage"]))
        self.assertFalse(current.search(tags["Image"]))
        major = int(tags["OldestImage"].split(".", 1)[0])
        self.assertTrue(allowed.search(f"{major}.99-alpine"))
        self.assertFalse(allowed.search(f"{major + 1}.0-alpine"))


if __name__ == "__main__":
    unittest.main()
