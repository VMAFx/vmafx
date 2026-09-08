#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the container and Renovate consume the shared Level Zero setting."""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Protocol, cast

ROOT = Path(__file__).resolve().parents[3]
BASH = shutil.which("bash") or "/bin/bash"
GIT = shutil.which("git") or "/usr/bin/git"
SPEC = importlib.util.spec_from_file_location(
    "workflow_versions", ROOT / "scripts/ci/check-workflow-versions.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class WorkflowGate(Protocol):
    RUN_RE: re.Pattern[str]
    LEVEL_ZERO_CONFIG: str
    LEVEL_ZERO_URL: str

    def check_level_zero_container(self, root: Path) -> list[str]: ...

    def load_config(self, root: Path) -> dict[str, str]: ...


GATE = cast(WorkflowGate, MODULE)


class LevelZeroSingleSource(unittest.TestCase):
    def setUp(self) -> None:
        self.text = (ROOT / "dev/Containerfile").read_text(encoding="utf-8")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        self.env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        (self.repo / "dev").mkdir()

    def check(self, text: str) -> list[str]:
        (self.repo / "dev/Containerfile").write_text(text)
        return GATE.check_level_zero_container(self.repo)

    def test_existing_container_passes(self) -> None:
        self.assertEqual(self.check(self.text), [])

    def test_workflow_checker_entrypoint_retains_container_validation(self) -> None:
        subprocess.run(  # noqa: S603 -- isolated disposable Git fixture
            [GIT, "init", "-q", str(self.repo)], check=True, env=self.env
        )
        (self.repo / "build-config.env").write_text((ROOT / "build-config.env").read_text())
        for text, status in (
            (self.text, 0),
            (self.text.replace(f". {GATE.LEVEL_ZERO_CONFIG}", "true"), 1),
        ):
            with self.subTest(status=status):
                (self.repo / "dev/Containerfile").write_text(text)
                result = (
                    subprocess.run(  # noqa: S603 -- shipped checker in a disposable Git fixture
                        [
                            shutil.which("python3") or "/usr/bin/python3",
                            str(ROOT / "scripts/ci/check-workflow-versions.py"),
                        ],
                        cwd=self.repo,
                        env=self.env,
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                )
                self.assertEqual(result.returncode, status, result.stdout + result.stderr)

    def test_missing_copy_source_or_reintroduced_argument_fails(self) -> None:
        for text in (
            self.text.replace(f"COPY build-config.env {GATE.LEVEL_ZERO_CONFIG}", ""),
            self.text.replace(f". {GATE.LEVEL_ZERO_CONFIG}", "true"),
            self.text + "\nARG LEVEL_ZERO_VER=1.32.0\n",
            self.text.replace("${LEVEL_ZERO_VERSION}", "1.32.0"),
            self.text.replace("${LEVEL_ZERO_VERSION}", "${UNREGISTERED_VERSION}", 1),
        ):
            with self.subTest(change=text[:80]):
                self.assertTrue(self.check(text))

    def test_actual_download_command_uses_changed_config_not_inherited_value(self) -> None:
        runs = [
            match.group(0)
            for match in GATE.RUN_RE.finditer(self.text)
            if GATE.LEVEL_ZERO_URL in match.group(0)
        ]
        self.assertEqual(len(runs), 1)
        command = runs[0].replace("\\\n", " ")
        command = re.sub(r"^RUN(?:\s+--mount=\S+)+\s+", "", command)
        config = self.repo / "build-config.env"
        command = command.replace(GATE.LEVEL_ZERO_CONFIG, shlex.quote(str(config)))
        tools = self.repo / "bin"
        tools.mkdir()
        for name in ("curl", "apt-get", "find"):
            tool = tools / name
            tool.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$PROBE_LOG"\n')
            tool.chmod(0o700)
        log = self.repo / "commands.log"
        for version in ("1.32.0", "1.33.9"):
            with self.subTest(version=version):
                config.write_text(f'LEVEL_ZERO_VERSION="{version}"\n')
                log.write_text("")
                result = subprocess.run(  # noqa: S603 -- actual RUN body with all external commands stubbed
                    [BASH, "-c", command],
                    cwd=self.repo,
                    env={"PATH": str(tools), "PROBE_LOG": str(log), "LEVEL_ZERO_VERSION": "0.0.0"},
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                calls = log.read_text()
                for package in ("libze1", "libze-dev"):
                    self.assertIn(f"/v{version}/{package}_{version}%2Bu24.04_amd64.deb", calls)
                self.assertNotIn("0.0.0", calls)

    def test_renovate_tracks_only_the_shared_level_zero_owner(self) -> None:
        config = json.loads((ROOT / "renovate.json").read_text())
        managers = [
            m
            for m in config["customManagers"]
            if m.get("depNameTemplate") == "oneapi-src/level-zero"
        ]
        self.assertEqual(len(managers), 1)
        manager = managers[0]
        self.assertEqual(manager["managerFilePatterns"], [r"/^build-config\.env$/"])
        matches: list[re.Match[str]] = []
        for pattern in manager["matchStrings"]:
            python_pattern = pattern.replace("(?<currentValue>", "(?P<currentValue>")
            matches.extend(re.finditer(python_pattern, (ROOT / "build-config.env").read_text()))
            self.assertIsNone(re.search(python_pattern, self.text))
        self.assertEqual(len(matches), 1)
        self.assertEqual(matches[0]["currentValue"], GATE.load_config(ROOT)["LEVEL_ZERO_VERSION"])

    def test_rocm_pins_are_owned_by_the_base_manager(self) -> None:
        config = json.loads((ROOT / "renovate.json").read_text())
        self.assertFalse(
            any(m.get("depNameTemplate", "").startswith("rocm/") for m in config["customManagers"])
        )
        manager = next(
            m
            for m in config["customManagers"]
            if m["datasourceTemplate"] == "docker" and "depNameTemplate" not in m
        )
        pattern = manager["matchStrings"][0]
        pattern = re.sub(r"\(\?<([A-Za-z]+)>", r"(?P<\1>", pattern)
        values = GATE.load_config(ROOT)
        for key in ("ROCM_BUILDER", "ROCM_RUNTIME"):
            with self.subTest(key=key):
                match = re.search(pattern, f'{key}="{values[key]}"')
                self.assertIsNotNone(match)
                assert match is not None
                image_tag, digest = values[key].split("@", 1)
                name, version = image_tag.rsplit(":", 1)
                self.assertEqual(match["depName"], name)
                self.assertEqual(match["currentValue"], version)
                self.assertEqual(match["currentDigest"], digest)


if __name__ == "__main__":
    unittest.main()
