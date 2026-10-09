#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/docker-hub-mirror.sh merges the Docker Hub mirror into daemon.json.

The script's ``--print`` mode needs no root and no daemon, so the merge is
checked offline: existing keys survive, a second run adds nothing, an existing
mirror is kept, a missing file starts empty, DOCKER_HUB_MIRROR overrides the
mirror, and a malformed daemon.json or a wrong call is refused.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "docker-hub-mirror.sh"
BASH = shutil.which("bash") or "bash"
MIRROR = "https://mirror.gcr.io"


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


if __name__ == "__main__":
    unittest.main()
