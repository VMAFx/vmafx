#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the actual base-image gate in small, disposable Git repositories."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"
GATE = "scripts/ci/check-base-image-single-source.sh"
BASE = 'ARG RELEASE_BUILDER_BASE="{pin}"\nFROM ${{RELEASE_BUILDER_BASE}} AS builder\n'


class BaseImageGate(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        for name in (
            "build-config.env",
            GATE,
            "scripts/ci/check-workflow-versions.py",
            "scripts/ci/check-container-image-references.py",
        ):
            source = ROOT / name
            if source.exists():
                target = self.repo / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
        config = (self.repo / "build-config.env").read_text()
        self.pin = next(
            line.split('"')[1]
            for line in config.splitlines()
            if line.startswith("RELEASE_BUILDER_BASE=")
        )
        self.run_command([GIT, "init", "--quiet"], check=True)

    def run_command(
        self, command: list[str], *, check: bool = False
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- fixed gate/Git commands in a disposable fixture
            command, cwd=self.repo, env=self.env, capture_output=True, text=True, check=check
        )

    def check(self, text: str, expected: int, name: str = "Dockerfile", write: bool = False) -> str:
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        self.run_command([GIT, "add", "--", name], check=True)
        result = self.run_command([BASH, GATE, *(["--write"] if write else [])])
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, expected, output)
        return output

    def test_external_from_is_rejected_with_or_without_digest(self) -> None:
        for ref in ("alpine", "alpine:latest", "registry.example/base:1", self.pin):
            with self.subTest(ref=ref):
                output = self.check(f"FROM {ref}\n", 1)
                self.assertIn("hardcodes a base image", output)

    def test_platform_and_continuations_do_not_hide_external_from(self) -> None:
        for instruction in (
            "from --platform=$BUILDPLATFORM alpine:latest AS source\n",
            "FROM --platform=linux/amd64 \\\n              # continuation comment\n              alpine:latest AS source\n",
            "FROM alpine AS alpine\n",
            "FROM future\nFROM scratch AS future\n",
        ):
            with self.subTest(instruction=instruction):
                self.check(instruction, 1)

    def test_copy_image_cannot_hide_behind_flags_or_case(self) -> None:
        for instruction in (
            "COPY --from=alpine /x /x\n",
            "copy --chown=0:0 --from=alpine:latest /x /x\n",
            f"COPY --link --from={self.pin} /x /x\n",
            "COPY --chown=0:0 \\\n              --from=alpine:latest /x /x\n",
            "COPY --from=${RELEASE_BUILDER_BASE} /x /x\n",
        ):
            with self.subTest(instruction=instruction):
                self.check(BASE.format(pin=self.pin) + instruction, 1)

    def test_named_stages_scratch_indices_and_central_arguments_pass(self) -> None:
        self.check(
            BASE.format(pin=self.pin)
            + "FROM --platform=$BUILDPLATFORM builder AS reused\n"
            + "FROM scratch\nCOPY --chown=0:0 --from=reused /x /x\nCOPY --from=0 /y /y\n",
            0,
        )

    def test_continued_platform_and_copy_keep_real_stage_references(self) -> None:
        self.check(
            BASE.format(pin=self.pin)
            + "FROM --platform=linux/amd64 \\\n              # retained continuation\n              builder AS reused\n"
            + "COPY --chown=0:0 \\\n              --from=builder /x /x\n",
            0,
        )

    def test_unregistered_missing_or_composed_argument_fails(self) -> None:
        for instruction in (
            "ARG NEW_BASE=alpine:latest\nFROM ${NEW_BASE}\n",
            "ARG RELEASE_BUILDER_BASE\nFROM ${RELEASE_BUILDER_BASE}\n",
            "FROM ${RELEASE_BUILDER_BASE}\n",
            "ARG \\\n              RELEASE_BUILDER_BASE=alpine\nFROM ${RELEASE_BUILDER_BASE}\n",
            "ARG RELEASE_BUILDER_BASE=ignored\nFROM ${RELEASE_BUILDER_BASE:-alpine}\n",
        ):
            with self.subTest(instruction=instruction):
                self.check(instruction, 1)

    def test_local_image_exceptions_are_bound_to_exact_consumer_and_value(self) -> None:
        self.check("FROM vmaf:latest\n", 0, "Dockerfile.ffmpeg")
        self.check("FROM alpine:latest\n", 1, "Dockerfile.ffmpeg")
        self.check("FROM vmaf:latest\n", 0, "Dockerfile.ffmpeg")
        self.check("FROM vmaf:latest\n", 1)

    def test_local_runner_argument_cannot_be_replaced_by_an_external_image(self) -> None:
        name = "dev/Containerfile.runner"
        self.check("ARG BASE_IMAGE=vmaf-dev-mcp:local\nFROM ${BASE_IMAGE}\n", 0, name)
        self.check("ARG BASE_IMAGE=alpine:latest\nFROM ${BASE_IMAGE}\n", 1, name)

    def test_argument_drift_fails_and_write_repairs_it(self) -> None:
        for declaration in ("ARG", "arg"):
            with self.subTest(declaration=declaration):
                text = BASE.format(pin="debian:wrong").replace("ARG", declaration)
                self.check(text, 1)
                self.check(text, 0, write=True)
                self.assertIn(self.pin, (self.repo / "Dockerfile").read_text())

    def test_write_cannot_legitimize_a_hardcoded_external_image(self) -> None:
        self.check("FROM alpine:latest\n", 1, write=True)
        self.assertEqual((self.repo / "Dockerfile").read_text(), "FROM alpine:latest\n")

    def test_intentional_distro_matrix_remains_outside_release_scope(self) -> None:
        matrix = self.repo / "docker/dev/alpine.Dockerfile"
        matrix.parent.mkdir(parents=True)
        matrix.write_text("FROM alpine:latest\n")
        self.run_command([GIT, "add", "--", "docker/dev/alpine.Dockerfile"], check=True)
        self.check(BASE.format(pin=self.pin), 0)


if __name__ == "__main__":
    unittest.main()
