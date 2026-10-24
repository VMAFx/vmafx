#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts/ci/free-runner-disk.sh lists the runner directories it removes and refuses a wrong call.

The removal itself needs root on a hosted runner, so it is exercised by the dev container
build; here the ``--list`` mode is checked offline: it prints the absolute directories the
script removes and nothing else, an unknown argument and an extra argument are refused with
exit 2, the dev container job calls the script before its image build, and no workflow
or action under ``.github/`` removes one of the directories itself instead of calling the
script (one list, HISS-19).
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml  # type: ignore[import-untyped]

SCRIPT = Path(__file__).resolve().parents[1] / "free-runner-disk.sh"
BASH = shutil.which("bash") or "bash"
GITHUB_DIR = Path(__file__).resolve().parents[3] / ".github"
WORKFLOW = GITHUB_DIR / "workflows" / "dev-container-build.yml"


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(  # noqa: S603 -- fixed bash executable and script argv
        [BASH, str(SCRIPT), *args], capture_output=True, text=True, check=False, timeout=30
    )


class FreeRunnerDisk(unittest.TestCase):
    def test_list_prints_absolute_directories(self) -> None:
        result = run("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn("/usr/share/dotnet", lines)
        self.assertTrue(lines)
        for line in lines:
            self.assertTrue(line.startswith("/") and line != "/", line)

    def test_unknown_argument_is_refused(self) -> None:
        result = run("--remove-everything")
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage", result.stderr)

    def test_extra_argument_is_refused(self) -> None:
        result = run("--list", "extra")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")

    def test_dev_container_job_frees_disk_before_building(self) -> None:
        document = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
        job = document["jobs"]["dev-container-build-work"]
        self.assertEqual(job["runs-on"], "ubuntu-26.04")
        runs = [str(step.get("run", "")) for step in job["steps"]]
        free = [i for i, text in enumerate(runs) if "scripts/ci/free-runner-disk.sh" in text]
        build = [i for i, text in enumerate(runs) if "build-dev-container-stage.sh" in text]
        self.assertTrue(free, "the job does not call free-runner-disk.sh")
        self.assertTrue(build, "the job has no image build step")
        self.assertLess(free[0], build[0])

    def test_no_workflow_removes_the_directories_itself(self) -> None:
        listed = run("--list").stdout.splitlines()
        self.assertTrue(listed)
        self.assertEqual(inline_removals(GITHUB_DIR, listed), [])

    def test_an_inline_removal_is_found(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "workflows").mkdir()
            (root / "workflows" / "planted.yml").write_text(
                "steps:\n  - run: sudo rm -r" + "f /opt/ghc\n"
                "  - run: bash scripts/ci/free-runner-disk.sh\n",
                encoding="utf-8",
            )
            found = inline_removals(root, ["/usr/share/dotnet", "/opt/ghc"])
        self.assertEqual(found, ["workflows/planted.yml:2"])


def inline_removals(root: Path, directories: list[str]) -> list[str]:
    """Return ``path:line`` for each YAML line under root that runs rm on a listed directory."""
    found = []
    for path in sorted(root.rglob("*.y*ml")):
        lines = path.read_text(encoding="utf-8").splitlines()
        for number, line in enumerate(lines, start=1):
            words = line.split()
            if "rm" in words and any(directory in words for directory in directories):
                found.append(f"{path.relative_to(root).as_posix()}:{number}")
    return found


if __name__ == "__main__":
    unittest.main()
