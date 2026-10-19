#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/docker-hub-mirror.sh merges the Docker Hub mirror into daemon.json.

The script's ``--print`` mode needs no root and no daemon, so the merge is
checked offline: existing keys survive, a second run adds nothing, an existing
mirror is kept, a missing file starts empty, DOCKER_HUB_MIRROR overrides the
mirror, and a malformed daemon.json or a wrong call is refused.

The daemon mirror only reaches pulls made by a step. A job ``container``, a
``services`` image and a ``uses: docker://`` step are pulled while the job is
set up, before any step runs, so those references must name a registry other
than Docker Hub themselves. WorkflowImages refuses a Docker Hub reference in
any workflow; an image given as a ``${{ }}`` expression cannot be resolved
statically and is not checked.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

SCRIPT = Path(__file__).resolve().parents[1] / "docker-hub-mirror.sh"
BASH = shutil.which("bash") or "bash"
MIRROR = "https://mirror.gcr.io"
WORKFLOWS = Path(__file__).resolve().parents[3] / ".github" / "workflows"
DOCKER_HUB_HOSTS = {
    "docker.io",
    "index.docker.io",
    "registry-1.docker.io",
    "registry.hub.docker.com",
}


def is_docker_hub(image: str) -> bool:
    """True when the reference resolves to Docker Hub: no registry host, or a Docker Hub host."""
    first, slash, _ = image.partition("/")
    if not slash:
        return True
    if "." not in first and ":" not in first and first != "localhost":
        return True
    return first.lower() in DOCKER_HUB_HOSTS


def image_of(value: object) -> str:
    if isinstance(value, dict):
        value = value.get("image", "")
    return value if isinstance(value, str) else ""


def setup_pulls(document: object) -> list[tuple[str, str]]:
    """(job, image) for every image the runner pulls while it sets a job up."""
    jobs = document.get("jobs") if isinstance(document, dict) else None
    pulls: list[tuple[str, str]] = []
    for name, job in (jobs or {}).items():
        if not isinstance(job, dict):
            continue
        images = [image_of(job.get("container"))]
        images += [image_of(service) for service in (job.get("services") or {}).values()]
        for step in job.get("steps") or []:
            uses = step.get("uses", "") if isinstance(step, dict) else ""
            if isinstance(uses, str) and uses.startswith("docker://"):
                images.append(uses.removeprefix("docker://"))
        pulls += [(str(name), image) for image in images if image and "${{" not in image]
    return pulls


def docker_hub_pulls(text: str) -> list[tuple[str, str]]:
    return [
        (job, image) for job, image in setup_pulls(yaml.safe_load(text)) if is_docker_hub(image)
    ]


def run(*args: str, mirror: str | None = None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env.pop("DOCKER_HUB_MIRROR", None)
    if mirror is not None:
        env["DOCKER_HUB_MIRROR"] = mirror
    return subprocess.run(  # noqa: S603 -- this repository's script, fixed argv
        [BASH, str(SCRIPT), *args], capture_output=True, text=True, timeout=60, env=env, check=False
    )


@unittest.skipUnless(shutil.which("jq"), "jq is not installed")
class MergeConfig(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.dir = Path(temporary.name)

    def merged(self, text: str | None, mirror: str | None = None) -> dict[str, object]:
        path = self.dir / "daemon.json"
        if text is not None:
            path.write_text(text, encoding="utf-8")
        done = run("--print", str(path), mirror=mirror)
        self.assertEqual(done.returncode, 0, done.stderr)
        loaded: dict[str, object] = json.loads(done.stdout)
        return loaded

    def test_existing_keys_survive(self) -> None:
        runner = '{"exec-opts":["native.cgroupdriver=cgroupfs"],"cgroup-parent":"/actions_job"}'
        merged = self.merged(runner)
        self.assertEqual(merged["cgroup-parent"], "/actions_job")
        self.assertEqual(merged["exec-opts"], ["native.cgroupdriver=cgroupfs"])
        self.assertEqual(merged["registry-mirrors"], [MIRROR])

    def test_second_run_adds_nothing_and_existing_mirror_is_kept(self) -> None:
        self.assertEqual(
            self.merged(json.dumps({"registry-mirrors": [MIRROR]}))["registry-mirrors"], [MIRROR]
        )
        kept = self.merged('{"registry-mirrors":["https://other.example"]}')["registry-mirrors"]
        self.assertEqual(kept, [MIRROR, "https://other.example"])

    def test_missing_file_starts_empty(self) -> None:
        self.assertEqual(self.merged(None), {"registry-mirrors": [MIRROR]})

    def test_mirror_is_configurable(self) -> None:
        merged = self.merged(None, mirror="https://mirror.example")
        self.assertEqual(merged["registry-mirrors"], ["https://mirror.example"])

    def test_malformed_daemon_json_is_refused(self) -> None:
        path = self.dir / "broken.json"
        path.write_text('{"exec-opts": [', encoding="utf-8")
        self.assertNotEqual(run("--print", str(path)).returncode, 0)

    def test_wrong_usage_is_refused(self) -> None:
        self.assertEqual(run("--print").returncode, 2)
        self.assertEqual(run("extra").returncode, 2)


class WorkflowImages(unittest.TestCase):
    def test_no_workflow_pulls_from_docker_hub_at_job_setup(self) -> None:
        found = {
            path.name: pulls
            for path in sorted(WORKFLOWS.glob("*.y*ml"))
            if (pulls := docker_hub_pulls(path.read_text(encoding="utf-8")))
        }
        self.assertEqual(found, {}, "name a mirror.gcr.io (or other registry) reference")

    def test_planted_docker_hub_references_are_refused(self) -> None:
        planted = """
jobs:
  a:
    container: semgrep/semgrep@sha256:0123
    services:
      db: {image: postgres:16}
      cache: docker.io/library/redis:7
    steps:
      - uses: docker://alpine:3.20
      - uses: docker://index.docker.io/fsfe/reuse:6
  b:
    container: {image: ubuntu}
"""
        self.assertEqual(
            docker_hub_pulls(planted),
            [
                ("a", "semgrep/semgrep@sha256:0123"),
                ("a", "postgres:16"),
                ("a", "docker.io/library/redis:7"),
                ("a", "alpine:3.20"),
                ("a", "index.docker.io/fsfe/reuse:6"),
                ("b", "ubuntu"),
            ],
        )

    def test_other_registries_and_expressions_pass(self) -> None:
        allowed = """
jobs:
  a:
    container: mirror.gcr.io/semgrep/semgrep:latest@sha256:0123
    services:
      db: {image: ghcr.io/vmafx/db:1}
      local: localhost:5000/x
    steps:
      - uses: actions/checkout@v6
      - uses: docker://mirror.gcr.io/fsfe/reuse:6
  b:
    container: ${{ matrix.image }}
"""
        self.assertEqual(docker_hub_pulls(allowed), [])


if __name__ == "__main__":
    unittest.main()
